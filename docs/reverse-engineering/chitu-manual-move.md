# chitu V1.3.3 - manual Z move ("manual-control" screen) findings (static reverse engineering)

Same binary, tooling, conventions and evidence style as `chitu-print-control.md` (read its sections 0, 1, 3, 6.1 first). Marking: **CONFIRMED** = read in disassembly (address given); **HYPOTHESIS** = inferred / needs a live check (section 8).
All addresses are virtual addresses in the non-PIE image. Disassembly was read with `tools/re/fn.py` / `callers.py`; every claim below can be re-derived with `fn.py app_manual_control_callback`, `fn.py axis_move`, etc.

## 1. Executive summary

- The touchscreen jog screen does **not** use G-code, `system_cmd_*` or a UI queue. Its click handler calls the C **axis layer** directly, on the UI thread: `axis_move(0, flags, mm, speed)`, `axis_home(0)`, `axis_halt(0)`, plus `axis_is_home(0)` for the "TOP" button. Axis 0 is Z (the only axis registered, `board_init` 0x25e48).
- `axis_move/home/halt` only **enqueue / reset** commands in mutex-protected `ts_queue`s owned by the axis object; the actual stepping is done by `axis_handle(0)`, called every ~1 ms from the `core_routine` pool thread (0x262d4, `usleep(1000)` at 0x262e8). So the call is asynchronous, returns at once, and is safe from any thread (the hook's UI-thread tick is fine).
- **There is no soft limit and no clamp in the axis layer** (flags at axis+94/+95 are only ever zeroed, section 5). Relative jogs are not limited at all: pressing "-100mm" on an unhomed or low plate drives the stepper that far. The stock screen relies on the user; a remote jog **must** clamp itself.
- Position is readable (`axis_get_position(0)` 0x25664, f32 mm; zero callers in stock) and a busy flag exists (`axis_is_idle(0)` 0x255d8; `axis_is_home(0)` 0x2562c).

**Recommended mechanism**: from the existing UI tick (installed on `ui.update_cb` 0x1078f8), call the same axis functions by fixed address, after a version gate. Jog = `axis_move(0, 1 /*relative*/, +-step_mm, *(float*)0x1095ac /*stock manual speed*/)` only when `axis_is_idle(0)==1 && axis_is_home(0)==1 && printer==NULL && ui.current!=16`, with the delta clamped by the hook to `[0 - pos, travel_max - pos]` (`travel_max = *(float*)0x1094f8`). Home = `axis_halt(0); axis_home(0)` (what the "Z 0" button does). Stop = `axis_halt(0)`. Status = `axis_get_position(0)`, `!axis_is_idle(0)`, `axis_is_home(0)`.

---

## 2. Q1 - Window 4 callback and how it is reached

- CONFIRMED `app_init` 0x14088-0x14178 registers: `movw r1,#0xaef8; movt r1,#1; mov r0,#4; bl window_register_callback` (0x140ac-0x140b8) -> **window 4 = `app_manual_control_callback`** `_Z27app_manual_control_callbackPP8widget_tS0_Pv` at **0x1aef8** (symbol in `nm`).
- Reached from the main screen (window 3, `app_main_callback` 0x1a164): CONFIRMED the main click handler (event code 7 -> `cmp r3,#52 ; ldrls pc,[pc,r3,lsl #2]` at 0x1a5c0-0x1a5c4, `r3 = widget_id-1`) has the table slot at 0x1a620 = 0x1aae4 = `mov r1,#0 ; mov r0,#4 ; b 1aa50 -> ... b ui_set_window_index` (0x1aae4-0x1aa5c). Slot index (0x1a620-0x1a5cc)/4 = 21 => **main-screen widget id 22** switches to window 4 with param 0. (Which icon id 22 is on screen: HYPOTHESIS "Move/Manual".) `ui_set_window_index` is the deferred two-word store described in the print doc; `ui_loop` then creates window 4 and sends event 46.
- Event dispatch in the callback (0x1af08-0x1af20): `lv_event_get_code`: **46** (created) -> 0x1af7c; **47** (destroyed) -> 0x1af60; **7** (clicked) -> button switch at 0x1af24 on `widget_t.id` = `*(u16*)(r5+8)`, `id-1` indexes a 9-entry jump table (0x1af38-0x1af58); other events ignored.

## 3. Q2 - Every button and the exact calls

Event 46 (enter), 0x1af7c-0x1b060: `app_top_return_enabled(true, tr(6))` (shows the top-bar "return" button with title string 6, 0x1af7c-0x1af8c) then `lv_label_set_text` on 8 labels from widget pointers `[window+4..+32]`: strings `"1mm"` (0xcfa40), `"10mm"` (0xcfa44), `"100mm"` (0xcfa4c), `"TOP"` (0xcfa54) for the first group and `"1mm"`, `"10mm"`, `"100mm"`, `"Z 0"` (0xcfa58) for the second (verified with `vaddr.py str`). Finally clears byte `0x101219` (= `0x101210+9`, the "home hint msgbox keep-open" flag, `strb ...[r3,#9]` 0x1b05c). **No hardware action on enter.**

Click table (CONFIRMED jump-table targets 0x1af38+, each `axis_move` is `b`-tail-called; arg regs: r0 = axis 0, r1 = flags, s0 = f32 mm, s1 = f32 mm/s; hard-float ABI):

| widget id | label | target | call | evidence |
|---|---|---|---|---|
| 1 | `1mm` (up group) | 0x1b070 | `axis_move(0, 1, +1.0f, machine_param[+0xc4])` | `vmov.f32 s0,#1.0` 0x1b07c, `vldr s1,[0x1094e8+196]` 0x1b080, `mov r1,#1` |
| 2 | `10mm` | 0x1b090 | `axis_move(0, 1, +10.0f, speed)` | 0x1b09c |
| 3 | `100mm` | 0x1b0b0 | `axis_move(0, 1, +100.0f, speed)` (const 0x42c80000 at 0x1b198) | 0x1b0bc |
| 4 | `TOP` | 0x1b0d0 | `if (axis_is_home(0)) axis_move(0, 0 /*absolute*/, machine_param[+0x10], speed)`; else if `!app_msgbox_is_active()` pushes msgbox 18 = `app_msgbox_axis_z_home_hint` 0x1ad20 ("please home first", 3 s auto-close) | 0x1b0d4-0x1b0fc, 0x1b174-0x1b194 |
| 5 | `1mm` (down group) | 0x1b100 | `axis_move(0, 1, -1.0f, speed)` | `s0 = #-1.0` 0x1b10c |
| 6 | `10mm` | 0x1b154 | `axis_move(0, 1, -10.0f, speed)` | 0x1b160 |
| 7 | `100mm` | 0x1b120 | `axis_move(0, 1, -100.0f, speed)` (0xc2c80000 at 0x1b19c) | 0x1b12c |
| 8 | `Z 0` | 0x1b140 | **home**: `axis_halt(0); axis_home(0)` | 0x1b144-0x1b150 |
| 9 | (stop; no label set by the screen) | 0x1b064 | `axis_halt(0)` | 0x1b068-0x1b06c |

(The id->label pairing for 1-8 follows the label-widget order written in event 46 and the signs of the constants; the physical position of id 9 is HYPOTHESIS.) Step sizes are **hard-coded literals, not stored anywhere**: 1, 10, 100 mm; no setting or param; units mm (f32). Speed is the single global `machine_param+0xc4` (f32, mm/s; HYPOTHESIS: "manual-move speed", written by the params G-code handler at 0x2f908; default value unknown until read live, L3). Absolute `TOP` target is `machine_param+0x10` (f32 mm) = the axis max travel (it is also the first word handed to `axis_params_updata(0, 0x1094f8)` at 0x25e54-0x25e5c: `machine_param+0x10` = start of axis-0 params; axis stride 0x48, `params_gcode_callback` 0x2f41c).

Event 47 (leave/destroy), 0x1af60-0x1af78: **`axis_halt(0)`** then `strb 1,[0x101219]`. So leaving the screen (e.g. top-bar return) cancels any jog in flight. Other screens' top-bar buttons refuse to navigate unless `axis_is_idle(0)` (0x1fb3c-0x1fb44 in `app_top_callback`, for the explorer-shortcut path).

## 4. Q3 - Threading, queues, busy state

`axis` object: global `axis` at **0x108e90** (`board_init` 0x25e30-0x25e44; the table of axis pointers is at **0x101804** `axis_constant`, `axis_register(0, 0x108e90)`). Field map (names are descriptive, chosen for these notes; offsets are what the code does):

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | `stepper*` | motor driver (`stepper_move/halt/...`) | 0x24bb4, 0x25410 |
| +0x04 / +0x08 | `endstop*` | min / max endstop | `endstop_enable(ax+4/+8)` 0x25028, 0x25070 |
| +0x0c | f32 | **max travel mm** (= `machine_param+0x10`) | `axis_params_updata` `vstr s14,[r5,#12]` 0x256f0; `+0x3c = +0xc / mm_per_step` |
| +0x10 | f32 | max speed mm/s | 0x256f8; `+0x40 = +0x10 / mm_per_step` steps/s |
| +0x18 | f64 | mm per step | 0x256fc, used 0x24c30, 0x25674 |
| +0x20 s8 | direction sign (stepper steps = logical x this) | 0x256e4, 0x25284 |
| +0x21 u8 | home type (0 min, 1 max, 2 both: `tst r1,#253` 0x25010) | 0x256f4, 0x24d48 |
| +0x22/+0x23 u8 | endstop polarity | 0x25700-0x25710 |
| +0x24 s8 | home direction sign | 0x25718, 0x24d4c |
| +0x28/+0x2c f32 | home fast / slow speed | `vldr s1,[r3,#40]` 0x24de0, `#44` 0x24e1c |
| +0x30/+0x34 f32 | home retract distances | 0x24d88, 0x24dfc |
| +0x3c / +0x40 | s32 | max travel steps / max speed steps/s | 0x249d0-0x249d8, 0x2575c |
| **+0x44** | u8 | **homed flag** (set by queued command 1 when homing finishes, 0x253f4; cleared at start of `axis_home` 0x24d78) | `axis_is_home` 0x25648 |
| **+0x48** | s32 | **current position, steps** (logical) | `axis_refresh` 0x244f8 |
| +0x4c | s32 | planned target steps (after commands already dequeued) | 0x250d8, `axis_halt` 0x24be8 |
| +0x58 | s32 | position offset set when an endstop is hit (0 at min / max-steps at max) | 0x25508, 0x2557c |
| +0x5c..0x5f | u8 x4 | min-endstop-armed, max-endstop-armed, **soft-limit-min, soft-limit-max** | 0x25004, 0x25044, 0x24fb0 |
| +0x60 | u64 | delay-command deadline | 0x24bec, 0x2545c |
| +0x68 | `ts_queue*` | **command queue** (64 x 40-byte entries, calloc 2560, `axis_init` 0x24818, params 0x40/0x28 at 0x24b70) | `ldr r0,[r3,#104]` 0x24c6c |
| +0x6c | `ts_queue*` | **execution queue** (8 x 40 B, calloc 320, 0x248c4) | 0x248d4 |
| +0x70 | mutex | `axis_handle`/`axis_is_idle`/`axis_is_home`/`axis_halt` lock | 0x24f5c, 0x255e8 |
| +0x88 | plan ring (16 x 20-byte) | trapezoid planner input | 0x24994 |

**Execution model (CONFIRMED)**: `axis_move` (0x24bf8) converts `mm -> steps = (int)(mm / mm_per_step)` and `speed -> steps/s` (`vdiv.f64`, `vcvt.s32.f64`, 0x24c30-0x24c50), logs both with `printf` (0x24c54), builds a 40-byte command `{type=0, steps@+4, speed@+12, flags@+20}` and `ts_queue_enqueue`s it to the command queue (0x24c8c). Flag semantics from `axis_handle`: bit0 (1) = **relative** (if clear, `delta -= target(+0x4c)`, i.e. absolute; 0x24f9c-0x24fac), bit1 (2) = keep batching, bit2 (4) = min-endstop stop, bit3 (8) = max-endstop stop. If bits 2|3 are set `axis_move` splits into delay/move/delay (0x24c9c-0x24d1c).
`axis_handle(0)` (0x24f30) runs in `core_routine` (0x262b4; `tpool_enqueue_routine` from `core_init` 0x26800), loop: `axis_handle(0); gcode_loop(); firmware_loop(); exposure_loop(); usleep(1000)` (0x262d0-0x262ec). It dequeues commands, clamps speed to +0x40 (0x25094-0x250a8), plans, `stepper_move`s (0x252b4), calls `axis_refresh` (0x25404) which refreshes position +0x48 from `stepper_get_info`, and resets the exec queue once the stepper is idle and its queue empty (0x25404-0x25448).

**Answers**:
- Synchronous? **No.** `axis_move/home/halt` return immediately; motion is done by the `core_routine` pool thread. UI never blocks (unless the 64-entry command queue is full; `ts_queue_enqueue` blocks on a condvar then, HYPOTHESIS for the exact full-queue behaviour; unreachable with refusal-when-busy).
- Overlap? **Allowed by the API and by the stock screen (no check):** repeated button presses just append relative moves that sum. `TOP` is the only button that checks (`axis_is_home`). Pressing home calls `axis_halt` first (queues reset, `stepper_halt`, target := position, 0x24bb8-0x24bec) so it is always safe to abort with home/halt.
- Busy flag: **`axis_is_idle(0)` (0x255d8)** returns 1 only if both queues have no available items (command queue 0x255f8, exec queue 0x2561c). Because the exec queue is only emptied after the stepper reports idle (0x25410-0x25448), `idle==1` means the physical move is finished (HYPOTHESIS for the last few ms; L6). It locks mutex +0x70, which `axis_handle` holds for each call, so it can stall the caller for at most microseconds-1 ms.
- Thread-safety: `axis_move` (queue mutex only), `axis_halt` (stepper_halt + mutex +0x70 + queue resets), `axis_home`, `axis_is_*` are all callable from any thread; the printer worker calls them from a pool thread itself (`home_state_handler` 0x38b70, `stage*` 0x386cc..., `stopping_state_handler` 0x37ba8..). `axis_get_position` takes no lock (plain aligned 32-bit read of +0x48, benign).

## 5. Q4 - Limits and safety

- **Max travel**: `machine_param+0x10` (f32 mm, address **0x1094f8**), copied into axis+0x0c / +0x3c. Value is read from the device parameter file (`%s/100MachineParams.gcode`, string 0xdfc6c; `M5000..` handler `params_gcode_callback` 0x2f38c), **not in the firmware image** (`/work/build/tk/scripts/parameter`, which `fw/build-firmware.sh` creates, is empty), so it must be read live (L2/L3).
- **Soft limits exist in code but are never enabled** (CONFIRMED): `axis_handle` clamps a negative delta to the lower bound if byte axis+0x5e != 0 (0x24fb0-0x24fc8, 0x25330-0x2536c) and a positive delta to `max steps (+0x3c)` if axis+0x5f != 0 (0x24fcc-0x24ff4). The only writers of +0x5e/+0x5f in the whole binary are in `axis_init` and **write zero** (0x24a3c, 0x24a4c; grep of `dis.txt` for stores at `#94`/`#95`). So **neither up nor down is clamped**; also the manual-move flag set (1 / 0) carries no endstop bits, so jogging **does not stop on an endstop**. Jogging down is not limited at "0": a jog of -100 mm from 5 mm commands -100 mm (plate into the vat/screen) - only mechanical stall/skipped steps stop it. (HYPOTHESIS: the 0.0 lower bound is the endstop/home reference, section 6.)
- **Homing required?** Not by the API. Stock requires it only for `TOP` (absolute move). Relative jogs work unhomed, but then the position is meaningless (it counts steps since boot; offset +0x58 is only set at an endstop hit, 0x25508/0x2557c). `axis_is_home` is false after boot until `axis_home` completes (+0x44 cleared at 0x24d78, set by queued command type 1, 0x253f4; `axis_is_home` returns `+0x44 == 1` 0x25654-0x25660).
- **Direction convention (CONFIRMED sign, HYPOTHESIS physical)**: `TOP` = absolute `machine_param+0x10` (max), and the up-group buttons use positive deltas, so **+ = up (away from vat), position 0 = home reference (build plate at the bottom/endstop)**. The Z home sequence moves negative toward the endstop when `home type == 0` (min): `axis_home` retract, then `axis_move(13 = rel|min|max endstop, -(s16)*2*travel, fast)`, retract, slow re-approach (0x24d84-0x24e5c); on a min hit `+0x58 := 0` (0x25574-0x2557c) so position 0 = endstop. If the parameter file says home type 1 (max), the sign flips (position := max steps at the top, 0x25500-0x25508). Live check L4.
- **Print in progress**: the axis layer has **no knowledge of prints** (no check in `axis_move`). Stock avoids jogging during a print purely by UI structure: window 4 is reachable only from window 3 (main), and printing runs in window 16 which replaces it; the print worker owns the axis through `home_state_handler`, `stage1/3`, `pausing`, `stopping` (see callers: 0x386cc, 0x38d50, 0x37ddc, 0x37ba8). `printer_t*` (0x101754) is non-NULL only while window 16 exists. A remote jog during a print would append moves to the same queues and corrupt the peel/lift sequence. The hook must refuse whenever `*(void**)0x101754 != NULL` or `ui.current == 16` (or `ui.next == 16`).
- Other concurrent axis users: the G-code script engine (`devices_gcode_callback` 0x3cc48/0x3cc94/0x3cd24: G28/G1 -> `axis_home`/`axis_move`) used by `autorun.gcode`, factory init, firmware update (window 6). Refuse while window 6 is current or the gcode `ehsm` (0x102dc4) is running (state getter not analysed: HYPOTHESIS, L9).
- Other safety side effects: none. No UV/LED/projector check on this screen (no calls to `dlpc_*`, `fpga` exposure, `uv`, `light` anywhere in 0x1aef8-0x1b19c); no motor enable/disable (the only `axis_enable` caller is `board_init` 0x25e70 which enables axis 0 once at boot). If the vat has resin and the exposure light is lit, nothing here turns it off (it is off outside a print).

## 6. Q5 - Position

CONFIRMED: `axis_get_position(0)` at **0x25664** returns f32 mm in `s0` = `(float)(*(s32*)(axis+0x48) * *(f64*)(axis+0x18))` (`vldr s0,[r3,#72]; vldr d16,[r3,#24]; vcvt; vmul; vcvt` 0x25670-0x25680). Zero callers in the stock binary (callers.py), the touchscreen never shows Z position. +0x48 is refreshed by `axis_refresh` (0x244bc: `pos = offset(+0x58) + stepper_steps * dir(+0x20)`, 0x244e4-0x244f8) from the `core_routine` thread whenever the exec queue is non-empty / at endstop events / on halt. Units: logical steps -> mm via +0x18. Meaningful only when `axis_is_home(0)==1`. A sibling `axis_get_speed` (0x25688) exists (unused). Raw alternative: `*(s32*)(0x108e90+0x48)`.

## 7. Q6 - Enter/leave behaviour a remote jog must replicate

- On enter: only UI (return button, labels, clears 0x101219). Nothing to replicate.
- On leave: `axis_halt(0)` (0x1af64). A remote jog is not tied to a window, so there is no "leave"; but if `ui.current == 4` when the hook jogs and the user then taps Return the jog is halted by stock code (acceptable, and safe).
- No motor-enable, no UV/light/projector action, no "heating" or door check. (`app_top_callback` blocks some navigation while `!axis_is_idle(0)`, 0x1fb40-0x1fb44; a remote jog therefore also temporarily blocks that top-bar shortcut: benign.)
- The home-hint dialog (`app_msgbox_axis_z_home_hint` 0x1ad20) is a pure UI nicety for `TOP`; the hook should return an API error "not homed" instead.

## 8. Q7 - Recommended mechanism (minimal fixed-address surface)

All calls are made from the tick on the UI thread (they are also thread-safe, but this keeps one policy point and lets the tick read `ui`/`printer` consistently). ABI: ARM hard-float (`-mfloat-abi=hard`, arguments in `s0/s1`, return float in `s0`): the stock code uses `vldr s0/s1` before `b axis_move` (0x1b080-0x1b08c). Compile the hook with the same ABI (the existing toolchain is `gnueabihf`; verify in the L10 check).

```c
typedef void  (*axis_move_t)(int axis, int flags, float mm, float mm_per_s); /* 0x24bf8 */
typedef void  (*axis_home_t)(int axis);                                      /* 0x24d28 */
typedef void  (*axis_halt_t)(int axis);                                      /* 0x24b9c */
typedef int   (*axis_is_idle_t)(int axis);                                   /* 0x255d8 */
typedef int   (*axis_is_home_t)(int axis);                                   /* 0x2562c */
typedef float (*axis_get_position_t)(int axis);                              /* 0x25664 */
#define Z_TRAVEL  (*(volatile float*)0x1094f8)   /* machine_param+0x10, mm      */
#define Z_SPEED   (*(volatile float*)0x1095ac)   /* machine_param+0xc4, mm/s    */
```

- **jog(+/- step_mm)**: preconditions: `*(void**)0x101754 == NULL`, `ui.current != 16 && ui.next != 16`, `ui.current != 6`, `axis_is_home(0)==1`, `axis_is_idle(0)==1`, `0 < step <= 100`. `pos = axis_get_position(0)`; `target = clamp(pos + step, 0.0, Z_TRAVEL)` (hook clamp: the axis layer has none); `d = target - pos`; if `|d| < 0.01` return OK/no-op; `axis_move(0, 1, d, speed)` with `speed = Z_SPEED` if `> 0` and `<= axis max speed` else a conservative fixed value (e.g. 5 mm/s; HYPOTHESIS the stock value is sane, L3). Optionally clamp lower bound to a small positive margin (e.g. 0.0 mm stock-equivalent; stricter e.g. 0.5 mm is the product decision).
- **home**: preconditions: no print/window 16/window 6. Do exactly the stock sequence `axis_halt(0); axis_home(0);` (the halt is required: `axis_home` assumes empty queues). Poll `axis_is_idle(0) && axis_is_home(0)` for completion. Home may be issued while busy (it aborts the current move first).
- **stop**: `axis_halt(0)` (always allowed). Note a halt discards the planned target; position (+0x48) stays valid as the real stepper count.
- **status**: `{homed = axis_is_home(0), busy = !axis_is_idle(0), z_mm = axis_get_position(0), travel_mm = Z_TRAVEL}` snapshot into the hook-owned struct each tick (cheap: one short mutex).
- **Overlap policy**: refuse a new jog while `busy` (HTTP 409); stock allows stacking but we should not.
- Optional UX: refuse/ignore if `ui.current == 4` too (user is on the jog screen) or let it run, both are safe (HYPOTHESIS; L8).

Fingerprints (16 bytes at function entry, file offset = vaddr - 0x10000, from `vaddr.py hex`). Add to the version gate of section 8 of the print doc:

| symbol | vaddr | 16 bytes |
|---|---|---|
| `axis_move` | 0x24bf8 | `f0 43 2d e9 04 48 01 e3 10 40 40 e3 c0 0a f7 ee` |
| `axis_home` | 0x24d28 | `70 40 2d e9 04 58 01 e3 10 50 40 e3 02 8b 2d ed` |
| `axis_halt` | 0x24b9c | `04 38 01 e3 10 30 40 e3 70 40 2d e9 00 60 a0 e1` |
| `axis_is_idle` | 0x255d8 | `04 38 01 e3 10 30 40 e3 70 40 2d e9 00 41 93 e7` |
| `axis_is_home` | 0x2562c | `04 38 01 e3 10 30 40 e3 70 40 2d e9 00 51 93 e7` |
| `axis_get_position` | 0x25664 | `04 38 01 e3 10 30 40 e3 00 31 93 e7 12 0a 93 ed` |
| `app_manual_control_callback` (optional) | 0x1aef8 | `70 40 2d e9 00 40 a0 e1 02 00 a0 e1 01 50 a0 e1` |

(`axis_*` entries share the prefix `04 38 01 e3 10 30 40 e3` = `movw/movt r3,#0x1804` = the axis table address, itself a useful sanity check that `0x101804` is the table.) Also sanity-check at install: `*(u32*)0x101804 == 0x108e90` (axis 0 registered, after `board_init`).

## 9. Q8 - Risks and live checks

Risks (numbered as in the print doc):
1. **No clamp anywhere**: a buggy/unclamped remote jog (or float parsing error) can drive the plate into the vat/screen or past the top. Hook-side clamping to `[0, travel]`, step cap 100 mm and homed-only are mandatory; physical stall is the only other protection.
2. **Print/gcode collision**: jogging during a print or autorun/firmware gcode corrupts the sequence (no axis-level arbitration). Guard on `printer`, windows 16 and 6.
3. **Wrong/zero travel or speed params**: `machine_param+0x10`/`+0xc4` come from the device parameter file; if 0 the clamp would forbid all up moves / `axis_move` with speed 0 steps/s may stall. Validate at install (read once on the tick: `travel > 10 && travel < 500`, `speed > 0`), else disable jog.
4. **Home hazards**: homing drives toward the endstop at the configured fast speed with plate possibly low/with resin; same as the stock "Z 0" button. Needs the user's confirmation in the web UI.
5. **Direction/sign assumption** (+ = up, 0 = bottom) is inferred from `TOP`/code, not observed.
6. **Float ABI/argument passing** mistakes (hook built soft-float) would send garbage distances. Verify with the log line `axis_move` prints (0x24c54) and a 1 mm test.
7. **Truncation**: `(int)(mm/step)` truncates toward zero; tiny deltas may be 0 steps. Position error accumulates only if the tick rounds; compute the delta from the live position each time.
8. **Leaving window 4 halts**: user tapping Return mid remote jog stops it (benign), but the web UI must handle a jog that ends short (read position, not assume).
9. **Stale idle**: `axis_is_idle` may read 1 for a few ms between command dequeue and exec enqueue? Code path says the command stays in the cmd queue until moved to the exec queue in the same locked call (0x2519c-0x251b0), so it is gapless under the mutex (HYPOTHESIS; L6).
10. **Position valid only when homed**; a power-cycled or halted-then-manually-moved plate loses position. After any `axis_halt` the position is still a stepper count but a skipped-step/stall is invisible.

The ids L1 to L10 label the live checks in the table of this section.

Live checks (SSH + gdbserver; empty vat, plate removed or high; printer at rest; per project etiquette):

| id | confirms | check |
|---|---|---|
| L1 | version/fingerprints | `x/16xb` each address in section 8 matches; `x/wx 0x101804` == 0x108e90 after boot. |
| L2 | axis object | `x/40wx 0x108e90`: `+0x0c` f32 travel (expect ~100-200 mm), `+0x18` f64 mm/step, `+0x20` direction (+-1), `+0x21` home type (0=min, 1=max), `+0x24`, `+0x44` homed, `+0x48` pos, `+0x5e/+0x5f` == 0 (soft limits off). |
| L3 | machine params | `x/wf 0x1094f8` (travel), `x/wf 0x1094fc` (max speed), `x/wf 0x1095ac` (manual speed; must be >0 and <= max speed), `x/wf 0x1095f8`.. compare with the axis +0x0c/+0x10. |
| L4 | sign and reference | Touchscreen: Main -> window 4, press `Z 0`, watch `*(s32*)0x108ed8` (axis+0x48) reach 0 (min) or `+0x3c` (max); then press up-group `1mm`: position increases and plate rises. Confirms + = up and 0 = homed bottom. |
| L5 | no clamp | With gdb, `x/bx 0x108eee` and `x/bx 0x108eef` (axis+0x5e/+0x5f) == 0 while screen is open (do NOT test a down move below 0). |
| L6 | busy semantics | During a 100 mm touchscreen jog, sample `call axis_is_idle(0)` (0 while moving, 1 after) and watch `+0x48` converge; confirm no early idle==1 between segments (queue-to-exec hand-off). |
| L7 | halt | Press stop/Return mid-jog: motion stops, `+0x4c == +0x48`, a following jog works. |
| L8 | hook coexistence | Hook jog while `ui.current==4`, `==3`, `==13`; confirm no UI glitch and top-bar nav blocking only during motion. |
| L9 | gcode engine / print guards | Start a (dry) print and verify `*(void**)0x101754 != 0`; check which gcode `ehsm` state (0x102dc4) is active during autorun to define a busy getter if needed. |
| L10 | ABI | Call `axis_move(0,1,1.0f,speed)` from the hook; the stock log line (format at 0xcfc6c, `printf` 0x24c54) must show 1.0 mm / the speed and +0x48 must advance by `1/mm_per_step`. |

## 10. Reproducing

`tools/dock.sh gktwo-fw 'python3 tools/re/fn.py app_manual_control_callback'`, `... fn.py axis_move|axis_home|axis_halt|axis_handle|axis_refresh|axis_is_idle|axis_is_home|axis_get_position|axis_params_updata|axis_init|board_init|core_routine`, `callers.py axis_move axis_home axis_halt axis_is_idle axis_is_home axis_get_position axis_handle axis_params_updata`, `vaddr.py str 0xcfa40 0xcfa44 0xcfa4c 0xcfa54 0xcfa58`, `vaddr.py hex <addr> 16`.

## 11. Not analysed

- Which icon is main-screen widget 22 and where widget ids 1-9 sit on the jog screen (LVGL widget tree); id 9 = stop is inferred.
- `devices_gcode_callback` (G-code motion) beyond its call sites; `fpga_stepper_*` / `stepper_*` driver internals (planner `axis_plan_calculate` 0x23d3c, acceleration).
- Default values of `machine_param` (device-side file); `stepper_get_queue_length` semantics; full-queue blocking of `ts_queue_enqueue`.
