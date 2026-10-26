# chitu V1.3.3 - print-control findings (static reverse engineering)

Binary: `/customer/resources/chitu` from `ChituUpgrade.bin` GKtwoV1.3.3 (extracted to `/work/tk/scripts/customer/resources/chitu` in the `gktwo-work` volume by `tools/re/extract-chitu.sh`).
Method: `arm-linux-gnueabihf-objdump -d`, `nm`, `readelf`, `gdb-multiarch`, plus small Python helpers. All in `tools/re/` (see "Reproducing").
Marking: **CONFIRMED** = read directly in disassembly/symbols (address given). **HYPOTHESIS** = inferred or needs the live check given.
All addresses are virtual addresses in the non-PIE image (`.text` is ARM mode, no Thumb; file offset = vaddr - 0x10000 for `.text/.rodata`).

## 0. Note: there is NO usable DWARF

CONFIRMED. `.debug_info` is 0x252 bytes and holds only 5 glibc CRT compile units (`start.S`, `init.c`, `crti.S`, ...). `gdb ptype print_context_t` prints `No symbol "print_context_t" in current context`
(output captured by `tools/re/regen.sh` into `/work/re/summary.txt`). The `print_context_t`, `ts_queue_t`, `widget_t` names exist **only inside C++ mangled symbol names**
(`_ZL16app_print_updatePP8widget_tP15print_context_t.isra.10`, `camera_rountine(void*, ts_queue_t*, ts_queue_t*, ts_queue_t*)`), not as type definitions.

Consequences:
- Step 1's `ptype` dumps are impossible. Every struct layout below is **reconstructed from load/store offsets in disassembly**, each row citing the instruction address. Treat field *names* as descriptive labels chosen for these notes; offsets/sizes are what the code does.
- Function "signatures" come from demangled C++ names (`app_*`, `explorer_*`) or are inferred from register use (C-linkage `printer_*`, `dlpc_*`, `gcode_*`).
- `app_print_update` is a GCC `.isra` clone: its second parameter is passed as the constant `0x101748` (= `print_context + 0x500`), not the whole struct.
- The binary has 2349 text symbols (`nm`), so function and global-variable *addresses* are fully available. That is what the hook needs.

## 1. Executive summary

**The print path does not go through `dlpc_*` or `gcode_*`.** The commonly assumed entry points are not the print API:
- `dlpc_start_print/stop_print/set_external_print_*` are raw I2C register writes to the DLP controller (slave 0x1b). `dlpc_start_print` is called only by `dlpc_test`; the other three have **zero** callers (section 7).
- `gcode_pause/resume/stop/start` drive a separate G-code-script state machine (`ehsm` at `0x102dc4`) used for firmware update / `autorun.gcode` / `100MachineParams.gcode`. Pause/resume/stop have **zero** callers (section 3).

**The real print engine is the `printer_*` API** (`printer_create` 0x39728, `printer_start` 0x3a1fc, `printer_pause` 0x3a224, `printer_stop` 0x3a248, `printer_resume` 0x3a26c, `printer_get_state` 0x3ac18, ...):
- `printer_t` is a 752-byte (0x2f0, verified from the `calloc` in `printer_create`) heap object. `printer_pause/stop/resume/start(printer, arg)` just **enqueue `{cmd,arg}` on a mutex+condvar-protected `ts_queue_t` at `printer+0x2a0`**; a worker thread (`printer_main_routine` 0x39290, run on the 16-thread pool) consumes it and drives an `ehsm` state machine. These four calls are **thread-safe to call from any thread** as long as the `printer_t*` is valid.
- The `printer_t*` lives in a global at **`0x101754`** (`print_context + 0x50c`). It is non-NULL only while the print window (window index 16) is alive: created in that window's "created" handler, freed in its "destroyed" handler.
- Print **start** is a *UI-window switch*, not a function call: the stock flow ends with `ui_set_window_index(16, &item)`, which stores `{index, param}` into the global `ui` struct (0x1078e8); the main loop's `ui_loop()` (0x4fed8) notices `current != next` and creates window 16, whose handler `app_print_callback` (0x1d4c4) does `printer_create(path)` and `printer_start`.

**Recommended mechanism: UI-thread marshalling via the `ui` update-callback slot.**
1. `libgkhook.so` (constructor) verifies the V1.3.3 fingerprint (section 8), then waits for `*(void(**)(void))0x1078f8 == 0x13d3c` (set once by `app_init`; its only writer) and atomically replaces it with its own tick function, which runs queued commands and then tail-calls the saved original (`app_update` 0x13d3c -> `app_msgbox_run`). `ui_loop` calls this slot **every iteration of `main`'s loop on the UI thread** (0x4ff1c `blx r3`).
2. Commands are posted from the socket thread into a hook-owned mutex queue and executed in the tick:
   - **start `<path>`**: validate the file, fill a static 0x50c-byte `explorer_item_tag` (name @+0, full path @+0x100, header magic @+0x500), call `ui_set_window_index(16, &item)`. Reuses the stock path unchanged (heater wait msgbox, file open, `printer_create`, `printer_start`).
   - **pause / resume**: `printer_pause(*(void**)0x101754)` / `printer_resume(...)` when the pointer is non-NULL and `ui.current == 16`.
   - **stop**: `printer_stop(...)`, then in later ticks, when state reaches 0/2/9, `ui_set_window_index(3, 0)` (what the stock stop-dialog does) so the print window is torn down and `printer_destory` runs.
   - **status**: snapshot the printer fields in the tick into a hook-owned struct; the socket thread reads only the snapshot (never dereference the `printer_t*` off-thread: it can be freed).
3. Rejected: calling `printer_*` straight from the socket thread (races `printer_destory`; also skips UI bookkeeping); `dlpc_*`/`gcode_*` (wrong subsystem); binary patching (not needed). Touch injection (`ts_uinput`) stays the fallback; I did not analyse its coordinates.

Biggest risks (details in section 6): (a) **start with an invalid/mismatching file likely segfaults chitu** (printer_create returns NULL and the code continues to use it) so the hook must pre-validate like the stock info dialog; (b) non-UI-thread writes to `ui.next/param` are a two-word non-atomic store; (c) stop/finish UI follow-up and the pause-at-layer-boundary latency are HYPOTHESES needing the dry-run checks in section 9.

---

## 2. Q1 - From the touchscreen "print" to exposure starting

CONFIRMED chain (all call sites from `tools/re/callers.py`):

| # | Where | What |
|---|---|---|
| 1 | main window 3 (`app_main_callback` 0x1a164) -> explorer window 13 (`ui_set_window_index(13, &entry)`; the exact caller/index is HYPOTHESIS, the main-screen button is not analysed) | `app_explorer_callback` (0x16134, "created" event 46, 0x16438-0x1646c) stores `*param` (int `entry`: 0 = USB, 1 = internal eMMC) in global `explorer_entry` (0xff5a0). CONFIRMED. |
| 2 | `utils_explorer_set_path`/`opendir` | Fills the item list (section 5/Q4). Tapping a file -> `explorer_item_callback` 0x15970 -> info dialog `explorer_info_msgbox_callback` 0x14e34. |
| 3 | `explorer_info_msgbox_callback`, print button (0x14f84-0x14fcc) | `entry==0` (USB): `app_msgbox_push(5, 1, app_msgbox_print_file_tip /*0x15ae0*/, item)` (0x14fc8). `entry==1` (eMMC): enqueues pool job `check_ctb_file_normal` 0x142e0 (= `print_file_open` + `print_file_check` -> global `check_file_normal_result` 0xff320; 0x152c0-0x15308), and when it finishes `app_explorer_updata` (0x149e8-0x14a08) pushes the same `print_file_tip` dialog (or an error dialog, index 20, on failure). The info dialog also checks resolution/version (below). |
| 4 | `app_msgbox_print_file_tip` 0x15ae0, event 46 | `memcpy(0x100c78 /*static item*/, item, 0x50c)` (0x15b28). |
| 5 | same, click handler (0x15bc8..0x15cc0) | `explorer_entry==1`: `ui_set_window_index(16, 0x100c78)` (0x15cc0). `explorer_entry==0` (USB): first copies the file to eMMC via `utils_explore_operation_start` (0x1608c, `"%s%s"` format 0xcf6e0), then continues. (Free-space check `utils_get_dir_available_size(emmc)` first.) |
| 6 | `ui_set_window_index` 0x4ffe0 | Only does `strd r0,r1,[ui+8]` i.e. `ui.next = idx; ui.param = ptr`. No switch happens here. |
| 7 | `ui_loop` 0x4fed8, called from `main` 0x13c40 every iteration | `if ui.current != ui.next`: `window_send_destroyed_event(old)` (code 47), `window_destory`, `window_create(next)`, `window_send_created_event(next, ui.param)` (code 46, 0x5145c/0x51474). |
| 8 | window 16 handler = `app_print_callback` 0x1d4c4 (registered by `app_init` 0x14140: `window_register_callback(16, 0x1d4c4)`), case code 46 at 0x1d784 | `memset(print_context,0,0x510)`; `memcpy(print_context /*0x101248*/, param, 0x50c)` (0x1d7a0-0x1d7bc). `printer = printer_create(print_context+0x100 /*0x101348*/, 0, 0)` (0x1d870), stored at `0x101754`. Then `print_file_open`, thumbnail setup via `print_file_read_image`, label/arc widgets. |
| 9 | same, end of case 46 (0x1db5c-0x1dba0) | `app_print_update(...)`. If `machine_param[0x100] != 0` (byte at 0x1095e8): `app_msgbox_push(19, 1, app_msgbox_heat_tip /*0x1c8b8*/, 0)` (0x1db7c) which waits for the heater (compares `machine_param+0xf4` and `+0xf8`, floats, vs constant at 0x11837c) then calls `printer_start(printer, 0)` (0x1cb28) automatically, or on the "skip" button (0x1cb64). Else `printer_start(printer, 0)` directly (0x1dba0). |
| 10 | `printer_start` 0x3a1fc | enqueue `{cmd=0,arg=start_layer}` on `printer+0x2a0`. Worker: `ehsm_event(printer, 4)` after `printer+0x258 = arg` (0x39418-0x39428) -> IDLE -> HOME -> STAGE1/2/3 ... = exposure. |

**File path form**: absolute path, NUL-terminated string at `item+0x100` (item = `explorer_item_tag`); the explorer builds it with `snprintf(buf,1023,"%s%s", dir, d_name)` (0x4dd7c, format at 0xcf6e0), e.g. `/media/emmc/foo.ctb` (directory string from `hotplug_get_emmc_path`, `"/media/emmc"` string at 0xe1c70).
**Context structs**: `explorer_item_tag` (0x50c bytes) is copied twice (0x100c78 then `print_context` 0x101248, which is a 0x510-byte global of that same item type followed by the printer pointer):

| off | field | evidence |
|---|---|---|
| +0x000 | `char name[256]` (d_name) | `strncpy(item, d_name, 255)` 0x4dd5c; compared with `"ChituUpgrade.bin"` (0xcf6f4) in `explorer_get_file_type` 0x14df4 |
| +0x100 | `char path[1024]` full path | 0x4dd78 `add r0,r0,#256`; `print_file_open(item+0x100)` 0x15e00/0x1d888 |
| +0x500 | `u32 header` = file magic | `utils_get_header` result stored 0x4dde4; tested vs `0x12fd0106/07` (CTB) and `0xff220810` (JXS) at 0x1cef0-0x1cf0c and 0x14d2c |
| +0x504 | `u8 is_dir` | `strb ... [item+0x100+0x404]` 0x4ddc0 |
| +0x50c | (in `print_context` only) `printer_t* printer` | `ldr sl,[0x101240+0x514]` 0x1d4e4; `str r0,[r4,#1300]` 0x1d878 |

(`print_context+0x500` is what `app_print_update` receives as its 2nd arg; if the header is not a known magic it returns without updating widgets, 0x1cf0c -> 0x1d304. The hook's fake item must therefore carry the right magic. HYPOTHESIS: magic = the first 4 bytes of the file, little-endian - confirm per live check L3.)

**Validation the stock UI does before step 3 (hook must replicate; CONFIRMED reads, semantic HYPOTHESIS):** `print_file_get_format(path)` (0x30554; table at 0xe0ae4 has only two non-NULL entries: slot 1 = CTB `ctb_print_file_identify` 0x30ee4/`open` 0x30f80, slot 6 = JXS2 0x36618/0x366b0; so **only .ctb and .jxs2 are implemented in this build**) must be non-zero; `print_file_open(path)` non-NULL; file X/Y resolution (`file+0x1004`, `+0x1008`, u32 each) must equal the u16 values at `0x104a24`/`0x104a26`; `file+0x1e10` (u32, format/version field) must be 6 or 1 (0x153b0-0x153bc `cmp r3,#6; cmpne r3,r6(=1)`), otherwise the file is flagged unprintable (0x15188-0x153c4).
**Latent crash** (CONFIRMED code, crash HYPOTHESIS): in `app_print_callback`, if `printer_create` returns NULL (0x1d874-0x1d87c) it jumps to 0x1dc74 which only calls `ui_set_window_index(3,0)` and falls back into the common code at 0x1d980, which dereferences `printer` (`printer_get_file_info` = `ldr r0,[r0,#188]` on NULL) in `app_print_update` (0x1cedc). Hence pre-validation in the hook is mandatory.

## 3. Q2 - Threads, ownership, command queue

**`pthread_create` call sites** (CONFIRMED: only 3 in the whole binary, via `tools`-scan of `dis.txt`):

| call site | in | start routine (r2) |
|---|---|---|
| 0x285bc | `ctb_file_check_start` | 0x284ac (CTB integrity check, one-shot) |
| 0x4ed74 | `utils_explore_operation_start` | 0x4e40c (file copy/delete worker, one-shot) |
| 0x5ed84 | `tpool_create` | 0x5e94c (pool worker) |

`tpool_get_handle` (0x5f0cc) lazily calls `tpool_create(16)` (0x5f0e4 `mov r0,#16`): a **16-thread pool**, so most "threads" are `tpool_enqueue_routine` jobs. Callers (0x5f0f4): `core_init`, `devices_init` x3, `temp_sensor_init`, `log_init`, `rcp_init` x2 (cloud/MQTT), `netif_ping_network_handler`, `samba_set_mount_path`, `system_cmd_action`, `start_capture` (camera), `print_file_check_start`, explorer-info/explorer, and **the printer**: `printer_create` (0x39dd4) enqueues `printer_main_routine` (0x39290) and that enqueues `printer_image_parse_routine` (0x3917c, 0x392e4).

| role | thread |
|---|---|
| UI / LVGL / `ui_loop` / `app_*_callback` / `printer_create`/`destory` | **main thread**: `main` 0x13c20 = `gui_init, log_init, param_init, devices_init, core_init, ui_init, app_init; for(;;){ ui_loop(); lv_timer_handler(); }` (0x13c24-0x13c48) |
| print-state machine owner | pool thread running `printer_main_routine` (loops: `ts_queue_try_dequeue(cmd queue)` -> `ehsm_event` -> `ehsm_run`) |
| layer-image decode | pool thread running `printer_image_parse_routine` |
| hotplug / network / camera / cloud | other pool jobs |

**Command queue (CONFIRMED)**. `printer_t` queues, all created in `printer_create` with mutex+cond (0x39888-0x39c44): `+0x2a0` command-in, `+0x2a4` command-ack, `+0x2a8`, `+0x2ac`, `+0x2b0` / `+0x2b4` (image-parse in/out).
Producers: `printer_start` 0x3a1fc, `printer_pause` 0x3a224, `printer_stop` 0x3a248, `printer_resume` 0x3a26c, each `ts_queue_enqueue(printer->q@0x2a0, &{cmd, arg})`:

```
0003a224 <printer_pause>:   ldr r0,[r0,#672]  ; mov r3,#1 ; str r3,[sp] ; mov r1,sp ; bl 38904 <ts_queue_enqueue.constprop.5>
```
Consumer `printer_main_routine` (0x3933c-0x39438): `ts_queue_try_dequeue(q@0x2a0)` then jump table on `cmd`:

| cmd | producer | action in consumer |
|---|---|---|
| 0 | `printer_start(p, layer)` | `p->current_layer(+0x258) = arg; ehsm_event(p, 4)` (start) |
| 1 | `printer_pause(p)` | `ehsm_event(p, 5)` |
| 2 | `printer_stop(p)` | `ehsm_event(p, 6)` |
| 3 | `printer_resume(p)` | `ehsm_event(p, 4)` (same event as start; the SUSPEND state reacts) |
| 7 / 8 | internal | worker-start handshake / exit (used by `printer_create`/`printer_destory`) |

`ts_queue_enqueue` (0x38904) takes `pthread_mutex_lock(q+4)`, copies into a ring (`q->ring{buf,+4 cap,+8 elem_size,+12 wr,+16 rd}`), `pthread_cond_signal(q+32)`, blocks on the condvar if full. So it is a genuine multi-producer-safe queue, **posting pause/resume/stop/start from any thread is safe** while `printer` is valid. Element size looks like 8 bytes (cmd, arg) (HYPOTHESIS; irrelevant to callers).
No other queue exists for UI commands: `msgbox_queue` (0x10121c, 12-byte `{flag,idx,cb,param}` entries, producer `app_msgbox_push` 0x1b500, consumer `app_msgbox_run` 0x1b2f4 on the UI thread) is for dialogs, and `firmware_message_queue`/`gcode_message_queue` (0x100c70/0x100c74) are for the G-code engine.

**Who calls the print controls today (CONFIRMED, `callers.py`)**:
- `printer_pause`: only `app_msgbox_print_tip` 0x1c85c (after user confirms "pause"). `printer_stop`: only 0x1c894 (confirms "stop"). `printer_resume`: only `app_print_callback` 0x1dd44 (tail call, no confirm; when state==8). `printer_start`: `app_print_callback` 0x1dba0 and `app_msgbox_heat_tip` 0x1cb28/0x1cb64. All on the UI thread.
- `gcode_start` callers: `app_msgbox_print_file_tip` 0x15c70 (only for explorer file types 2/3 = `.gcode`/`.bin` firmware), `factory_init`, `autorun_gcode_handler`. `gcode_pause/resume/stop`: **none**. They `utils_ehsm_event(&0x102dc4, 5/6/7)` of the G-code script engine; irrelevant to prints.

## 4. Q3 - Where live print state lives

All print state is in the **heap** `printer_t` (`calloc(1, 0x2f0)` = 752 bytes at 0x39760, VERIFIED in the `printer_create` disassembly: `mov r0,#752` 0x3975c, `bl calloc` 0x39760) whose pointer is the global **`0x101754`** (CONFIRMED). No fixed-address printer fields exist. Read it only on the UI thread (or via a snapshot).
Globals: `0x101754` `printer_t* printer` (== `print_context+0x50c`; NULL when no print window), `0x101248` `print_context` (0x510 B; path at `0x101348`), `0x1078e8` `ui` (below), `0x101224` `msgbox_is_busy` (u8), `0x1094e8` `machine_param` (0x100 = preheat flag, `+0xf4`/`+0xf8` heater values, HYPOTHESIS names).

`printer_t` offsets (CONFIRMED from accessors/handlers; names are descriptive, chosen for these notes):

| off | type | meaning | evidence |
|---|---|---|---|
| +0x0b8 | ptr | opaque arg from `printer_create` | 0x3977c |
| +0x0bc | `print_file*` | open file handle | `printer_get_file` 0x3ad20 |
| +0x0c4.. | copy | file exec args (0x40..), read via `printer_get_file_args` | 0x3ac58, 0x39770 |
| +0x180 | rwlock | guards args | `printer_get_args` 0x3a9bc |
| +0x1c..+0x8c | `ehsm_state[8]` x16 B | IDLE+28, HOME+44, STAGE1+60, STAGE2+76, STAGE3+92, SUSPEND+108, PAUSING+124, STOPPING+140 | `utils_ehsm_create_state` calls 0x39c50-0x39d30 |
| +0x254 | u32 | arg1 of `printer_create` | 0x39778 |
| **+0x258** | u32 | **current layer** | `printer_get_current_layer` 0x3ac20; set from start arg 0x39428 |
| +0x25c | s32 | error code (0 = none) | read at every state entry, e.g. 0x37ac4 |
| **+0x260** | s32 | **state enum** | `printer_get_state` 0x3ac18 |
| +0x268 | u8 | 0 = timer running, 1 = timer stopped (paused/idle) | `printer_get_print_tick` 0x3acb8 |
| **+0x270** | u64 | **estimated total print time, ms** | `printer_get_total_print_tick` 0x3ac84, set from `printer_estimate_ticks` at 0x38ae4 |
| +0x278 | u64 | last timestamp (ms) | 0x3acd4 |
| **+0x280** | u64 | **elapsed print time, ms** (excludes pauses) | accumulated in `printer_get_print_tick` |
| +0x288 | mutex | guards 0x268-0x284 | 0x3ac6c |
| +0x2a0..+0x2b4 | `ts_queue*` x6 | see section 3 | |
| +0x2b8 | u8 | HOME wait flag | 0x38aa8 |
| +0x2c1 | u8 | "all layers done" (set when `current_layer == total`) | 0x38e4c |
| +0x2c2 | u8 | pause pending (set on pause event in STAGE3) | 0x38c04 |
| +0x2c4 | u8 | stopping-homing flag | 0x37a54 |
| +0x2d8 | ptr | callback manager (state-change callbacks) | 0x39d68 |

Elapsed time note: `+0x280` is only brought up to date by `printer_get_print_tick()` (0x3ac98, mutex-protected, adds now - last); `now` = `clock_gettime(CLOCK_MONOTONIC_RAW)` in ms (`utils_get_current_tick` 0x5fdb4, clock id 4). Call that function (UI thread) rather than reading +0x280.
Remaining = `printer_get_total_print_tick() - printer_get_print_tick()` (stock UI, 0x1d1e0-0x1d23c; it prints `h:m:s` via `utils_second2time` of ms/1000).

**Total layers / layer height / resolution** - `file_info = *(printer+0xbc) + 0x1004` (`printer_get_file_info` 0x3ac48): `+0` u32 X res, `+4` u32 Y res, `+8` f32 layer height (mm), **`+12` u32 total layers** (`app_print_update` 0x1cf48 / 0x1d0f0 reads `[r7,#12]`; `stage3` end-of-print test compares `current_layer(+0x258)` with `file+0x1010` = `file_info+12`, 0x38dd4-0x38de4). Percentage in the stock UI = current/total (0x1d3d0). Whether the current layer is 0- or 1-based is HYPOTHESIS (live check L2).
**File name**: `print_context+0x000` (name) and `+0x100` (full path) at `0x101248` / `0x101348` - plain C strings, valid as long as the print window is alive (CONFIRMED copy at 0x1d7b0).

**State enum** (`printer+0x260`). Written at entry of each `ehsm` handler (CONFIRMED stores):

| val | state | evidence |
|---|---|---|
| 0 | IDLE (initial; also "finished by user stop") | 0x395cc `str r3(=0)`, taken when `+0x25c==0` and `+0x2c1==0` |
| 1 | HOME (homing before first layer) | 0x38b5c (`r5=1`) |
| 2 | **COMPLETE** (all layers printed, `+0x2c1==1`) | 0x39568 (`r6`=ehsm event 2); `app_print_update` 0x1cf60 `cmp r8,#2` -> pushes `app_msgbox_print_complete` 0x1cc94 |
| 3 | STAGE1 (layer start / peel-lift) | 0x3864c |
| 4 | STAGE2 (exposure; UI copies the layer bitmap here, 0x1cfa4) | 0x390e0 |
| 5 | STAGE3 (end of layer / lower / decision point) | 0x38c80 |
| 6 | PAUSING | 0x37d28 |
| 7 | STOPPING | 0x37ad0 |
| 8 | SUSPEND (paused; UI shows the "resume" icon 109 vs 108, 0x1d1bc) | 0x37eb0 |
| 9 | ERROR (`+0x25c != 0`) | 0x394fc; UI pushes `app_msgbox_print_error` 0x1cda8 (0x1cf34) |

Mapping to labels is CONFIRMED for the numeric values; the *names* (STAGE1/2/3 = the `ehsm` state names from strings at 0xe12e4/0xe12ec/0xe12f4, SUSPEND/PAUSING/STOPPING 0xd1398/0xe12fc/0xe1304) are from `utils_ehsm_create_state` string arguments. Which physical phase STAGE1/2/3 are is HYPOTHESIS; for the web UI treat 3/4/5 as "printing".
Transitions CONFIRMED: STAGE3 at end of layer: `current==total` -> sets +0x2c1, goes STOPPING (0x38e40-0x38e50); else if `+0x2c2` (pause requested) -> PAUSING (0x38dfc); else -> STAGE1 (0x38e2c). Therefore **pause takes effect at the end of the current layer** (HYPOTHESIS for STAGE1/2, since the pause event is only latched in STAGE3 at 0x38bf8; confirm L4). Stop event is acted on immediately in STAGE3 (0x38be0 -> STOPPING) and from HOME goes straight to IDLE (0x38a80).

## 5. Q4 - Internal-storage file list; will an externally-written file appear?

CONFIRMED:
- Internal storage = eMMC partition `mmcblk0p2` mounted at **`/media/emmc`** (`mount -t vfat,exfat -o iocharset=utf8 /dev/mmcblk0p2 /media/emmc`, string 0xe177c; `p1` at `/media/emmc0`; both made with `mkfs.exfat` if missing, 0xe16e0/0xe170c). `hotplug_get_emmc_path` 0x4088c returns the path from a 296-byte-per-entry table at 0x117c50. This is the same mount Samba shares, so a file written by smbd or by the hook's web server is visible to chitu immediately in the kernel.
- The list is **not cached across opens**: opening the explorer window (code 46 in `app_explorer_callback` 0x16134) stores `entry` then `utils_explorer_set_path(<explorer object, literal at 0x16a50>, emmc_path)` (0x164c8) -> `utils_explorer_opendir` 0x4e778 -> `utils_explorer_update_items` 0x4dbfc which does `opendir`, loops `readdir64`, and for every entry `malloc`s a fresh array of `explorer_item_tag` (0x50c each) and fills name/path/`stat`/`is_dir`/file header (0x4dd38-0x4dde4). Same re-scan on `udisk_state` change in `app_explorer_updata` (0x14894) and after delete (0x14d18). It is a flat directory listing; no extension filter in the scan - classification happens afterwards in `explorer_get_file_type` 0x14d2c (suffix `bmp`/`gcode`, header magics, name `ChituUpgrade.bin`) and `print_file_get_format` (CTB / JXS2 only). Sorting: `qsort(..., 0x14344)` in `explorer_item_callback`.
- No inotify/epoll/fanotify anywhere (no such imports; the only dir polling is `hotplug_run` 0x4045c over `opendir`).
- **Answer: a `.ctb` written to `/media/emmc` by another process appears the next time the user opens the internal-storage list (window 13 re-created); no reboot needed.** While that list is already on screen it will not refresh itself (no timer/inotify). A forced refresh would be: `ui_set_window_index(3,0)` then `ui_set_window_index(13,&one)` on later ticks (`ui_loop` ignores `next==current`, and `ui_refresh_window` 0x4ffac has **no callers** and would pass a NULL param to window 13) - HYPOTHESIS, not needed for the MVP. Live check L5.
- exFAT/vfat caveat (HYPOTHESIS): a file still being written (upload in progress) will show in the list with its partial size and a partial header; the web server should write to a temp name outside the chitu-visible directory, or rename when complete. Also validate first 4 bytes before offering start.

## 6. Q6 - Recommended invocation mechanism (details and risks)

### 6.1 Hook points (all CONFIRMED addresses)

| item | address | note |
|---|---|---|
| `ui` struct | 0x1078e8 (bss) | `{u32 last@0, u32 current@4, u32 next@8, void* param@12, void(*update_cb)(void)@16}` (`ui_init` 0x4fe48, `ui_loop` 0x4fed8, `ui_set_window_index` 0x4ffe0) |
| `ui.update_cb` | **0x1078f8** | written only by `ui_register_update_callback` 0x4ff98, called once from `app_init` 0x14078 with `0x13d3c`; called at `ui_loop` 0x4ff1c every loop on the UI thread |
| original callback | 0x13d3c `app_update` | a single `b app_msgbox_run` (0x1b2f4) |
| print window index | 16 | `window_register_callback(16, 0x1d4c4)` at 0x14140-0x14148 |
| main window index | 3 | `window_register_callback(3, 0x1a164)`; initial window after boot is 3, or 1 (guide) if `machine_param+0x11d`==1 (0x14188-0x14194) |
| window indices registered | 0 top, 1 guide, 3 main, 4 manual-control, 6 system-update, 7 language, 8 system-setting, 9 device-info, 10 use-info, 11 tech-support, 12 exposure, 13 explorer, 16 **print**, 21 network, 24 shared-folder, 25 xc-network | `app_init` 0x14088-0x14178 |
| printer API | `printer_pause` 0x3a224, `printer_resume` 0x3a26c, `printer_stop` 0x3a248, `printer_get_state` 0x3ac18, `printer_get_current_layer` 0x3ac20, `printer_get_total_print_tick` 0x3ac68, `printer_get_print_tick` 0x3ac98 | all ARM (ABI: r0 = `printer_t*`), return in r0/r0:r1 (u64) |

Calling convention: plain AAPCS C (`void printer_pause(void* p)`); `printer_get_*_tick` return `uint64_t` in r0:r1. The functions are non-static symbols but called inside the executable, so **PLT/`LD_PRELOAD` symbol interposition cannot intercept them**: the hook must call them by fixed address (`((void(*)(void*))0x3a224)(p)`), after the V1.3.3 fingerprint passes.

### 6.2 Why this over alternatives

- **UI-thread tick (recommended)**: single-threaded LVGL/UI rule is respected; `printer_create` (blocking: waits for the worker handshake, 0x39dec-0x39e00) and window changes happen on the thread that owns them; stock start path reused including the heater wait and thumbnails.
- *Direct `printer_*` from the socket thread*: pause/resume/stop would work at the queue level, but `printer` can be freed by `printer_destory` (0x1d630, in the window-destroyed handler) at any time -> use-after-free; no start possible this way.
- *Touch injection*: works with zero code-address knowledge but is slow/fragile (explorer scroll position, dialogs, heater wait). Keep as fallback.
- *Replace `ui.update_cb` with lock-free write*: a single aligned 32-bit store on ARMv7 is atomic. The tick must **chain to the original** (`app_msgbox_run`, drives dialogs; skipping it breaks every popup).

### 6.3 Command semantics

**start `<path>`** (tick): require `ui.current != 16`, `printer==NULL`, `msgbox_is_busy (0x101224)==0`, no pending `ui.next != ui.current`; file under `/media/emmc/`, `.ctb`; read first 4 bytes; pre-validate (`print_file_get_format` 0x30554 != 0; `print_file_open` 0x305d8 != NULL; `print_file_check` 0x30a5c (full-file check, slow: stock runs it on a pool thread as `check_ctb_file_normal`, so run it on the hook's own thread, not the UI tick) then `print_file_close` 0x3073c; X/Y resolution vs `*(u16*)0x104a24/0x104a26`) - the cheap checks (format/open/resolution) may run on the UI tick or hook thread (HYPOTHESIS that `print_file_open` handles are independent; the stock info dialog calls it on the UI thread); fill static `item[0x50c]` (zeros, `name`, `path` @+0x100, `u32 magic` @+0x500, `is_dir=0` @+0x504); `ui_set_window_index(16, item)`. The window is created on the *next* `ui_loop` pass; `item` must stay valid until `app_print_callback` has memcpy'd it (it copies immediately, 0x1d7bc), so a static buffer is fine.
**pause/resume**: require `p = *(void**)0x101754 != NULL` and `ui.current==16`; `state==8` -> `printer_resume(p)`; state in {3,4,5} -> `printer_pause(p)`. Stock pause shows a confirm dialog first (`app_msgbox_print_tip` stage) - skipping it is purely UI (the dialog's only side effects are `stage` @0x101240 and closing itself; CONFIRMED 0x1c854-0x1c85c), HYPOTHESIS that no state is left inconsistent (L4).
**stop**: `printer_stop(p)`; stock then keeps the dialog with `stage=1` until state in {0,2,9,8} and calls `ui_set_window_index(3,0)` (0x1c690-0x1c698). The hook should mirror this: set a `stopping` flag, and on later ticks when `printer_get_state(p)` is 0 (or 2/9) call `ui_set_window_index(3, 0)`. On leaving window 16 the destroyed handler (code 47, 0x1d5c4-0x1d63c) frees images, `printer_destory(printer)` (0x1d630), NULLs 0x101754, calls `app_top_return_standby_handler`. Note: state 2/9 make `app_print_update` pop the complete/error dialogs (once, on the state change, 0x1cf28); the user (or hook) dismisses them with `ui_set_window_index(3,0)` as their OK handlers do (0x1cd98/0x1ceac). CONFIRMED: the handlers' widget 4 (tick) does `ui_set_window_index(3,0)` and returns 1, which closes the dialog; widgets 2/3 return 1 only (dialog closed, window 16 stays). Window 16's destroy handler does **not** close an open dialog, so the hook must close it first: `app_msgbox_close_avtive_msgbox` 0x1b1a0 closes the open dialog without calling its callback (stock `app_top_callback` calls it before `ui_set_window_index(13, ...)`, 0x1fb48). It reads `msgbox_queue+4` (0x101220, the open dialog's window copy) without checking `msgbox_is_busy`, and that pointer is stale while no dialog is open, so call it only while 0x101224 == 1. Open dialog callback: `[[[[[0x101220]+8]]+0x2c]+0x10]+4` (window copy -> widget list -> widget 0 -> window copy -> 12-byte entry `{flag, idx, cb, param}`). Queue `*0x10121c` = `{buf, cap, size, write, read}`; a dialog is queued but not shown while busy == 0 and write != read (`app_msgbox_run` shows it on its next call).
**status**: tick copies `{window=ui.current, printer!=NULL, state, layer, total_layers, elapsed_ms (via 0x3ac98), total_ms (0x3ac68), path (0x101348), err}` under a hook mutex.
**refresh_files**: not required (section 5).

### 6.4 Risks

1. **Invalid file => NULL printer => crash** (section 2). Mitigated by pre-validation; verify with a deliberately bad file in dry mode (L6).
2. `ui.update_cb` replaced too early/late: wait with a short sleep loop on the hook thread until `*(u32*)0x1078f8 == 0x13d3c`; abort (log) if it holds anything else. `app_init` runs before the first `ui_loop`, so the slot is stable thereafter.
3. Non-UI writes to `ui.next/param` (e.g. calling `ui_set_window_index` from the socket thread) are a two-word `strd` race with `ui_loop`'s `ldmib` read; so only do it from the tick.
4. Starting while a dialog is open or from a non-main/non-explorer window may leave overlapping `msgbox` state; require windows 3 or 13 and `msgbox_is_busy==0`.
5. The heater wait (`machine_param[0x100]!=0`) will make "start" pause at a dialog until heated or skipped; the hook should report this as state "preheating" (printer exists, state 0, window 16, heat dialog active).
6. Pause latency up to one layer (section 4). Tell the web UI "pause requested".
7. `printer_stop` while the printer is already IDLE (state 0/2/9) enqueues event 6, which the IDLE handler appears to ignore (it only matches events 2/3/4, 0x39444-0x39468) - HYPOTHESIS; the hook should simply not send it (check state first).
8. SD/eMMC concurrency: the web upload and chitu's `opendir` are independent, but `ctb` open reads the whole header; never start a half-uploaded file.

## 7. Q5 - `dlpc_set_external_print_control/configuration`

CONFIRMED (disassembly 0x3daa4, 0x3dae0): both are thin wrappers that build a short buffer and call `i2c_write(dlpc_fd /*[0x104a00]*/, slave 0x1b, buf, len)`:
- `dlpc_set_external_print_configuration(u8 a, u8 b)`: buf = `{0xA8, a, b}`, len 3.
- `dlpc_set_external_print_control(u8 a, u16 b, u16 c)`: buf = `{0xC1, a, b(le), c(le)}`, len 6.
- `dlpc_start_print()` 0x3de64: four I2C writes (regs 0x1c5, 0x1c3, 0x605, then `{0xc1,0x05,..0xffff}`); `dlpc_stop_print()` 0x3def8: one write `{0xc1,0x01,0x00,0x00}`.
`callers.py` finds **no callers** for `dlpc_set_external_print_control/configuration` or `dlpc_stop_print`, and `dlpc_start_print` is called only by the debug routine `dlpc_test` (function at 0x3e008; 0x3e06c is the call site of `dlpc_start_print`), which has no callers either. (No address-taken references either: no literal-pool/data refs.) **They are DLP-controller (projector) register accessors, not an app-level external-control entry point, and are dead code in this build.** Calling them would poke the projector directly and could interfere with the running exposure engine; do not use.

## 8. Q7 - V1.3.3 fingerprint for the hook's version check

CONFIRMED:
- File size **1074744** bytes (`stat -c %s`), sha256 `4f2b7342028aa44814782506d98611281a2be46e7c100b7451cb4630e3789779`, md5 `d9713705d20ebc094fb2dca09ff0248f`. Built with GCC 8.2.1 20180802; `PT_LOAD` va 0x10000 -> file off 0.
- 16 bytes at `dlpc_start_print` 0x3de64 (file offset 0x2de64): `10 40 2d e9 00 4a 04 e3 08 d0 4d e2 10 40 40 e3`.

Recommended (additional windows): the hook only calls things other than `dlpc_start_print`, so also check these (each 16 bytes, all printed by `tools/re/regen.sh`):

| symbol | vaddr | 16 bytes |
|---|---|---|
| `printer_start` | 0x3a1fc | `04 e0 2d e5 0c d0 4d e2 a0 02 90 e5 00 30 a0 e3` |
| `printer_pause` | 0x3a224 | `04 e0 2d e5 0c d0 4d e2 a0 02 90 e5 01 30 a0 e3` |
| `printer_stop` | 0x3a248 | `04 e0 2d e5 0c d0 4d e2 a0 02 90 e5 02 30 a0 e3` |
| `printer_resume` | 0x3a26c | `04 e0 2d e5 0c d0 4d e2 a0 02 90 e5 03 30 a0 e3` |
| `printer_create` | 0x39728 | `f0 4f 2d e9 1c d0 4d e2 00 70 a0 e1 01 90 a0 e1` |
| `printer_get_state` / `_current_layer` | 0x3ac18 | `60 02 90 e5 1e ff 2f e1 58 02 90 e5 1e ff 2f e1` |
| `ui_loop` | 0x4fed8 | `70 40 2d e9 e8 48 07 e3 10 40 40 e3 21 00 94 e9` |
| `ui_set_window_index` | 0x4ffe0 | `e8 38 07 e3 10 30 40 e3 f8 00 c3 e1 1e ff 2f e1` |
| `app_update` (original update_cb) | 0x13d3c | `6c 1d 00 ea f0 47 2d e9 01 40 a0 e1 18 d0 4d e2` |
| `app_msgbox_close_avtive_msgbox` | 0x1b1a0 | `70 40 2d e9 1c 42 01 e3 10 40 40 e3 00 30 94 e5` |
| `app_msgbox_print_complete` | 0x1cc94 | `30 40 2d e9 00 40 a0 e1 0c d0 4d e2 03 00 a0 e1` |

Simplest robust check: `st_size == 1074744` of `/proc/self/exe` plus these byte windows (read from the live process memory at the fixed addresses; non-PIE so no relocation) - and refuse to act (log only) on mismatch. Optionally also a runtime read of `*(u32*)0x1078f8 == 0x13d3c` at install time.

## 9. Live checks to run (SSH + gdbserver; printer dry: empty vat, small .ctb)

The ids L1 to L10 label the live checks in the table of this section.

Script: `tools/re/live-checks.gdb` (`pstate` command dumps all fields below). Start: `gdbserver --attach :2345 $(pidof chitu)` on the printer; `gdb-multiarch -ex "target remote <printer-ip>:2345" -x tools/re/live-checks.gdb <chitu>` on the dev PC. No DWARF: use raw addresses.

| id | confirms | check |
|---|---|---|
| L1 | `printer_t*` global 0x101754 and offsets | While printing: `x/wx 0x101754` -> non-zero heap ptr `P`; `x/s 0x101348` is the file path; `x/wd P+0x260` (state 3-5), `x/wd P+0x258` (layer, increments), `x/2wx P+0x270` (total ms), `x/wx *(P+0xbc)+0x1004+12` (total layers). After returning to main: `x/wx 0x101754` == 0. |
| L2 | layer base (0/1-based), elapsed/total timing | Dump `P+0x258` right after `printer_start` and at last layer: does it end at `total` (1-based end) and what is the first value? Compare `printer_get_print_tick` result with wall clock (break at 0x3ac98, `finish`, `p $r0`). |
| L3 | item struct + magic | `break *0x14d2c` (explorer_get_file_type) while the list is open: `x/s $r0`, `x/s $r0+0x100`, `x/wx $r0+0x500` and compare with `xxd -l4 file.ctb` (expect `0x12fd0106/07` style magic = first 4 bytes little-endian). Also `break *0x1d7a0` at touch-start: `x/wx $r1+0x500`. |
| L4 | pause latency & stock side effects | After `call`/hook of `printer_pause(P)` (gdb: `call ((void(*)(void*))0x3a224)(*(void**)0x101754)`, ok on a stopped inferior only) watch `P+0x260`: STAGE -> PAUSING(6) -> SUSPEND(8); how many layers elapsed? Does the on-screen icon flip (app_print_update path 0x1d1b0) without the dialog? Then `printer_resume`. |
| L5 | file list rescan | Over SSH/Samba copy a `.ctb` to `/media/emmc`; open Main -> Internal storage: file present without reboot? While the list is on screen, add another file: appears only after leaving/re-entering (expected). |
| L6 | invalid-file crash | Dry, with the hook (or gdb: write `ui.next=16`, `ui.param=item` with a garbage `.ctb` path in `item+0x100`): expect SIGSEGV in `printer_get_file_info` (0x3ac48, fault addr 0xbc). If confirmed, pre-validation is mandatory (it is anyway). |
| L7 | stop path and end state | `printer_stop(P)` mid-print: final `P+0x260` value (expected 0 via STOPPING -> IDLE with 0x2c1==0 / 0x25c==0), whether a complete/error dialog appears, and that `ui_set_window_index(3,0)` returns to main and `x/wx 0x101754` becomes 0. Natural finish: state 2 + "print complete" dialog (0x1cc94). |
| L8 | update_cb slot | `x/wx 0x1078f8` == `0x13d3c` after boot; `watch *(int*)0x1078f8` should never trigger after `app_init`. Also `watchpoint` on 0x1078f0 while pressing Print on the touchscreen: see `{16, 0x100c78}` stored on the UI thread (`info threads`). |
| L9 | heater dialog | If `x/bx 0x1095e8` (machine_param+0x100) != 0, start via the hook and check the heat dialog behaviour/timeouts before `printer_start` fires (0x1cb28). |
| L10 | version | `stat -c %s /customer/resources/chitu` == 1074744; `sha256sum` equals section 8; `cat /proc/$(pidof chitu)/maps` shows chitu at 0x10000 (non-PIE, no ASLR for the main image). |

## 10. Reproducing

All in `tools/re/` (run inside `tools/dock.sh gktwo-fw '...'` from the project root):
- `regen.sh`: builds `/work/re/{dis.txt,nm.txt,funcs.txt,info.txt,summary.txt}` and prints the DWARF-absence evidence, size/sha, fingerprint bytes.
- `fn.py <symbol-substring|0xADDR>`: print one function's disassembly from `dis.txt` (e.g. `fn.py app_print_callback`).
- `callers.py <symbol|hex>...`: list every `bl`/`b` call site (and literal-pool refs) with the enclosing function.
- `vaddr.py str|hex|word|map ...` and `strs.py <regex>`: read strings/bytes by virtual address; list strings with vaddr.
- `live-checks.gdb`: gdbserver session helper (section 9).

Key commands behind this document: `callers.py printer_start printer_pause printer_stop printer_resume printer_create dlpc_start_print gcode_pause ...`; `fn.py printer_main_routine`, `fn.py app_print_callback`, `fn.py app_print_update`, `fn.py 0x4fed8` (ui_loop), `fn.py app_msgbox_print_file_tip`, `fn.py app_msgbox_print_tip`, `fn.py app_msgbox_heat_tip`, `fn.py utils_explorer_update_items`, `fn.py stage3_state_handler`, `fn.py idle_state_handler`.

## 11. Open items / not analysed

- `.jxs2` path and `cbddlp` (no slot in the format table, so unsupported in this build despite the spec's list).
- Touch-injection (`ts_uinput`) coordinates/flow: not analysed.
- Exact meaning of STAGE1/2/3, `machine_param` fields, `printer_get_args` indices (31/32 shown as floats on the print screen): names are inferred.
- `app_top_callback` (window 0, status bar) and `app_top_return_*` flags at 0x105261/0x105262: only observed being cleared on print-window teardown.
