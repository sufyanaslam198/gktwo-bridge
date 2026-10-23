# GKtwo Bridge development guide

This guide is for a developer who wants to build, test, change and extend GKtwo Bridge. It explains how the parts fit together, how to build them and how to change them safely.

GKtwo Bridge is not affiliated with Uniformation or ChiTu Systems. It is tested only on the Uniformation GKtwo with stock firmware V1.3.3.

Contents:

1. [Architecture](#architecture)
2. [gkhook internals](#gkhook-internals)
3. [The ABI layer](#the-abi-layer)
4. [How to add a new call into chitu safely](#how-to-add-a-new-call-into-chitu-safely)
5. [gksdcp internals](#gksdcp-internals)
6. [Build environment](#build-environment)
7. [Tests](#tests)
8. [Development on hardware](#development-on-hardware)
9. [Constraints](#constraints)
10. [Releases](#releases)
11. [Repository layout](#repository-layout)

## Architecture

The image adds three programs to the stock firmware. Dropbear is an SSH server. gksdcp is a server for SDCP (UDP port 3000, TCP port 3030) and for the web page (port 80). gkhook is a library that `chitu` loads at boot. Nothing in `chitu` is patched on disk.

```
 slicer, browser
    |  SDCP: UDP 3000, TCP 3030 (WebSocket, upload)
    |  web page: TCP 80
    v
 gksdcp  (own process, /customer/gk/bin/gksdcp)
    |  one JSON line per connection
    v
 hook socket  /tmp/gkhook.sock  (Unix stream socket)
    |
    v
 +------------------- chitu process -------------------+
 |  gkhook socket threads  <->  queue + status snapshot |
 |                                  ^        |          |
 |                                  | drain  | publish  |
 |  chitu UI loop --> ui.update_cb = gk_tick            |
 |                      |  calls printer_*, axis_*,     |
 |                      |  ui_set_window_index          |
 |                      v                               |
 |                 stock callback app_update            |
 +------------------------------------------------------+

 SSH (port 22) --> Dropbear (own process)
```

### Data flow

1. A client sends an SDCP command on the WebSocket (or an upload chunk to port 3030).
2. gksdcp validates the command and sends one JSON line to the hook socket.
3. A gkhook connection thread reads the line. It answers a status request from the snapshot. It puts every other command in the queue.
4. The UI tick of `chitu` takes the command from the queue and acts on `chitu`.
5. The tick publishes a new status snapshot and chains the stock callback.

The wire protocol is in [gkhook-protocol.md](gkhook-protocol.md).

### Boot chain

| Step | File | Action |
|---|---|---|
| 1 | `/etc/init.d/rcS` | The image inserts `fw/rcS.hook` before the `modules_stage0` line. The inserted code runs `/customer/gk/customerStart` if it exists. |
| 2 | `/customer/gk/customerStart` | Starts a detached shell that waits 35 s and then runs `customer.sh`. It returns at once. |
| 3 | `/customer/gk/customer.sh` | Runs each executable `/customer/gk/init.d/R??*` with `start`, in name order. It logs to `/tmp/gk-boot.log`. |
| 4 | `init.d/R96dropbear` | Creates missing host keys. Starts Dropbear on port 22. |
| 5 | `init.d/R97gksdcp` | Starts gksdcp with `nice -n 10` and `oom_score_adj` 1000. gksdcp opens UDP port 3000, TCP port 3030 and (if built in) the web page on TCP port 80. |

The stock boot scripts start `chitu` through `/customer/app0.sh`. The image replaces that file. The new `app0.sh` starts `chitu` with `LD_PRELOAD=/customer/gk/lib/libgkhook.so` when the library is readable. If the library is missing, it starts `chitu` without the preload. A missing library therefore never stops the printer from booting.

## gkhook internals

Source: `gkhook/src/`. Every file is small. Read them in this order: `hook.c`, `queue.c`, `server.c`, `tick.c`, `zjog.c`, `validate.c`.

### Constructor sequence

`gkhook_init` in `hook.c` runs when the loader loads the library. It does these steps in order:

1. It logs the socket path and the files folder.
2. It compares `/proc/self/exe` with `/customer/resources/chitu` (device and inode). If the process is not `chitu`, it logs `gkhook: inactive: not chitu` and returns. Other processes that load the library stay unaffected.
3. It calls `unsetenv("LD_PRELOAD")`, so child processes (`system`, `mount`, `sh`) do not load gkhook.
4. It runs the fingerprint gate (`verify_fingerprint`). The gate checks that the file size is 1074744 bytes. Then it reads 16 bytes at each address in a table and compares them with the expected bytes. It reads live memory with `pread` on `/proc/self/mem`, so a bad address fails and does not fault.
5. If the gate passes, it sets `gk_abi_enabled`, sets the start validators and starts the installer thread.
6. It starts the server thread in all cases. If the gate failed, every request gets the error `wrong chitu version`.

### Installer thread

The installer waits until `chitu` registers its stock update callback in `ui.update_cb` (address `0x1078f8`). It polls every 50 ms for approximately 60 s and every 500 ms after that.

- If the slot holds the stock callback `app_update` (`0x13d3c`), the installer swaps in `gk_tick` with `__sync_bool_compare_and_swap`. This is one atomic store.
- If the slot holds any other non-zero value, the installer logs the value and leaves `chitu` alone.
- After the swap, the installer reads the axis table at `0x101804`. If entry 0 equals the axis 0 object (`0x108e90`), it sets `gk_z_enabled`. Otherwise Z control stays disabled and every `z_*` command fails with `Z control unavailable`.

### The thread rule

This rule is the most important rule of the code base.

| Thread | Allowed |
|---|---|
| UI tick (`gk_tick`, inside the `chitu` UI loop) | The only caller of `printer_*`, `ui_set_window_index` and `axis_*`. The only reader of the `ui` struct and of the global at `0x101754`. |
| Socket threads (the accept thread and the connection threads) | The queue and the status snapshot. The connection thread also runs the cheap file check: `print_file_get_format`, `print_file_open`, `print_file_close` and the printer resolution globals. |
| Checker thread (detached, one per start) | `print_file_open`, `print_file_check` and `print_file_close`. Nothing else. |

`chitu` is not thread safe. A call from another thread can corrupt its state or crash the printer. Do not add an exception.

### Server threads

`server.c` has one accept thread. It accepts a connection and starts a detached connection thread with a 256 KB stack (the same size as the checker thread, because the cheap file check runs in the connection thread). The connection thread reads the line (1 second limit), runs the request, sends one reply and closes the connection.

- At most 8 connection threads run at the same time (an atomic counter). If the limit is reached, or `pthread_create` fails, the accept thread sends `{"ok":false,"error":"hook busy"}` with a non-blocking send and closes the connection. The accept thread never blocks on a client.
- A slow client or a command that waits for the tick holds only its own thread. A `status` request does not wait for it.
- Connection threads share only the queue (one mutex), the abi flag and the read-only paths. `start` requests hold `start_mu`, so the path checks and `gkq_begin_start` run for one request at a time.
- Keep every buffer of the connection code on the stack and below 16 KB in total. Do not add a static buffer.

### Queue and snapshot

`queue.c` holds all shared state behind one mutex.

- The FIFO holds up to 16 commands. `gkq_post` adds a command. `gkq_take` removes one (tick side).
- `gkq_post_wait` adds a command and waits for the result of the tick. The wait uses a result slot (16 slots) keyed by an ID that is never reused. If the tick does not answer in time, the call withdraws the command and returns `printer not responding`. A late result finds no slot and is dropped.
- `gkq_publish_status` stores the snapshot. `gkq_read_status` copies it. While a start is in flight, the copy reports the state `starting`. The failure message of the last start is added when the status is read.
- Unknown numeric fields in the snapshot are `-1`. The protocol layer writes them as `null`.

### Start flow with generations

A start takes several steps on several threads. A generation number stops a late step from changing a newer start.

1. A connection thread receives `start`. It refuses the request in three cases. The state is not `idle`, `complete` or `error`. A finished print is still on screen. Z moves. It runs the path and cheap file checks.
2. `gkq_begin_start` returns a new generation (greater than 0). It returns 0 if a start is already in flight. The state is now `starting`.
3. The connection thread spawns a detached checker thread and answers `{"ok":true}`.
4. The checker runs the full validation (`print_file_check`) and reads the file magic. On failure it ends the start with a message. On success it posts a `CMD_START` command that carries the generation and the magic.
5. The tick takes `CMD_START`. It drops the command if `gkq_start_valid` says the generation is no longer in flight. It checks the window, the printer, the message box, the Z axis and the magic again. Then it fills chitu's static item buffer and calls `ui_set_window_index(16, item)`.
6. The tick waits until the printer state shows a print. It then ends the start with success. If the window does not open in 5 s, the start ends with `start failed`.
7. Timeouts: 10 s after the checker finishes without a command (`start failed`) and 600 s from the start (`file check timed out`).

A stop during a start cancels the start for the current generation.

### The tick

`gk_tick` in `tick.c` runs inside the `chitu` UI loop. It must be fast, must not block and must not abort. It does these steps:

1. Read the view: current window, next window, printer pointer, printer state.
2. Drain the queue. Handle each command: start, pause, resume, stop, `z_*`.
3. Finish a stop: when the printer has stopped, return to the main window.
4. Advance the start state machine (see above).
5. Publish a new snapshot when something changed or at least every 250 ms.
6. Always call `gk_app_update()`, the stock callback. Every code path chains it.

Commands that need a decision do not run when the state is wrong. A stop during the stock heat dialog is refused with `printer is heating - cancel it on the touchscreen`. gkhook cannot close that dialog safely.

### Validation

`validate.c` repeats every check that the stock file dialog makes. An invalid file makes the stock `printer_create` return NULL and then crash `chitu`. Therefore a file must pass these checks before a start:

| Check | Where | Error text |
|---|---|---|
| In the files folder, no `/` or `..` in the name, `.ctb` extension, regular file | server | `file is not in the files folder`, `bad file path`, `only .ctb files can be printed`, `file not found` |
| The file can be read | server, checker, tick | `cannot read file` |
| Magic `0x12fd0106` or `0x12fd0107` | server, checker, tick | `not a ctb file` |
| Known format | server | `unsupported file format` |
| `print_file_open` succeeds | server | `cannot open file` |
| Resolution equals the printer resolution | server | `file resolution does not match printer` |
| Format field of the opened file (`+0x1e10`) is 1 or 6, as the touchscreen checks. This is not the version in the CTB header. | server | `unsupported ctb version` |
| `print_file_check` returns 0 | checker | `file check failed` |

### Z jog guards and clamp

`zjog.c` decides if a Z command can run. The tick is the only caller. The checks run in this order:

1. Z control enabled, else `Z control unavailable`.
2. No printer, window 16 not current or next, no start in flight, else `printer busy`.
3. Not in the firmware update window (6), else `firmware update in progress`.
4. `z_stop` halts the axis and returns.
5. The Z speed value is greater than 0 and at most 30 mm/s, else `Z speed setting invalid`.
6. The axis is idle, else `Z is moving`.
7. `z_home` halts, then homes.
8. The axis is homed, else `home Z first`.
9. For an upward move, the travel value is between 0 and 300 mm, else `Z travel limit unknown`. The position must be a finite number, else `Z position unknown`.
10. The target is clamped to [0, travel]. A remaining delta under 0.01 mm gives `at top limit` or `at bottom limit`. The relative move is never more than 100 mm.

The values come from `chitu` memory. The code validates them before use.

### Log file

`gk_log` appends one line to `/tmp/gkhook.log`. Each line starts with the Unix time. The file is in RAM and the printer clears it at restart. gkhook logs errors, start and stop events, and state transitions only. It never logs on every tick. Set the environment variable `GKHOOK_LOG` to use another file in tests.

## The ABI layer

`gkhook/src/chitu_abi.h` holds the fixed addresses and the typed wrappers for `chitu` V1.3.3. This `chitu` is a non-PIE ARM executable that loads at virtual address `0x10000`. Every address in the header is valid only for that exact file.

- **Function addresses** come from the symbol list of `chitu` (`nm`). Section 6.1 of the findings lists them.
- **Global addresses** (`ui`, the printer pointer, machine parameters) come from the findings, because some are not in the symbol list.
- **Wrappers** call a function through its address. Each wrapper checks `gk_abi_enabled` first. The ARM `.so` builds with `-DNDEBUG`, so the check never aborts.
- **Hard-float ABI.** The printer uses ARM hard-float. Float arguments and results use the registers `s0` and `s1`. Declare them as `float` in the wrapper. Do not pass them as integers.
- **`GK_ABI_INDIRECT`.** This is the only test seam. When a host test defines it, `GK_ADDR(a)` calls `gk_abi_resolve(a)`. The mock in `gkhook/tests/mock_chitu.c` maps an address to a fake function or fake memory. Production code uses the fixed address directly.

The evidence for each address is in the findings:

- [chitu-print-control.md](reverse-engineering/chitu-print-control.md): print control, the `ui` struct, the printer state enum, fingerprint windows.
- [chitu-manual-move.md](reverse-engineering/chitu-manual-move.md): the axis layer and the machine parameters.

## How to add a new call into chitu safely

Follow these steps in order. A wrong call can move the build plate, corrupt `chitu` memory or crash the printer.

1. **Find the function.** Run the tools in the `gktwo-fw` container. Run `tools/dock.sh gktwo-fw 'bash tools/re/extract-chitu.sh'` once. It needs the stock `ChituUpgrade.bin` in the repository root. It extracts the stock `chitu` file to `/work/tk/scripts/customer/resources/chitu` and prints its SHA-256 value. Then run `tools/dock.sh gktwo-fw 'bash tools/re/regen.sh'` once. It writes the disassembly and symbol lists to `/work/re`. Then use these tools:
   - `python3 tools/re/strs.py '<regex>'` finds strings in `chitu` and prints their addresses.
   - `python3 tools/re/callers.py <symbol|hex-address>` lists the call sites and the data references of an address.
   - `python3 tools/re/fn.py <symbol|0xaddress>` prints the disassembly of one function.
   - `python3 tools/re/vaddr.py hex <address> <n>` prints `n` bytes at a virtual address. Use `str` for a string and `word` for a 32-bit value.

   Example: `tools/dock.sh gktwo-fw 'python3 tools/re/callers.py axis_move'`.
2. **Confirm the address.** Search `/work/re/nm.txt` for the symbol. The address in the header must equal the symbol address. For a global that is not in the list, find at least two code references that agree.
3. **Document it.** Add the function to the findings in `docs/reverse-engineering/`: the address, the arguments, the return value, the thread rule and the evidence.
4. **Add a fingerprint window.** Print 16 bytes at the entry point with `vaddr.py hex`. Add the entry to the `windows` table in `gkhook/src/fingerprint.c`. Add the same bytes to the independent copy in `gkhook/tests/test_fingerprint.c`. Do not copy the bytes between the two files with a script. Read them again from the findings.
5. **Add a typed wrapper** in `chitu_abi.h`. Add the address as a `GK_` macro. Use `GK_CHECK()` and `GK_ADDR()`. Use the exact C types of the hard-float ABI.
6. **Call it only from the tick.** Put the decision in a separate small file, like `zjog.c`. The tick passes the state it has read to that file. Do not call the function from the socket thread or the checker thread.
7. **Add guards.** Refuse the call in every state where it can do harm: during a print, during a start, during a firmware update, when the axis moves. Validate every value that comes from `chitu` memory. Clamp every value that goes to `chitu`.
8. **Add tests with the mock.** Add the fake function to `mock_chitu.c`. Write tests for every guard and for every clamp edge. Run `make -C gkhook host && make -C gkhook test`.
9. **Test on hardware** (see [Development on hardware](#development-on-hardware)). Use an empty, dry vat. Keep your hands clear of the build plate. Start with the smallest possible action. Watch `/tmp/gkhook.log`.

## gksdcp internals

Source: `gksdcp/src/` and `gksdcp/www/index.html`. gksdcp uses only the C library, pthreads and the small JSON tokenizer jsmn (`jsmn.h`, MIT licence). [`sdcp.md`](sdcp.md) describes what gksdcp does for its clients.

gksdcp is one process with one event loop (`poll`). One extra thread talks to gkhook (`hookw.c`). The thread keeps a snapshot of the status. It sends commands to gkhook one at a time and calls back on the loop thread.

| File | Task |
|---|---|
| `main.c` | Parses the options (section 14 of [`sdcp.md`](sdcp.md)) and starts the parts. |
| `loop.c`, `loop.h` | The event loop. TCP listeners, connections, HTTP request heads, routing, WebSocket frames, send queues with limits, timers. |
| `http.c` | Parses the HTTP request head. The `Origin` rule. |
| `ws.c`, `crypto.c` | The WebSocket handshake and framing. SHA-1, MD5 and base64. |
| `json.c`, `jsmn.h` | Parse and write JSON. |
| `sdcp.c` | The envelope: the command table, responses, notices, errors and the pushes of status and attributes. |
| `status.c` | The status and attributes frames. The values of `CurrentStatus` and `PrintInfo.Status`. |
| `cmd_print.c` | Commands 128 to 133, 386 and 387. |
| `cmd_files.c` | Commands 192, 258 and 259. |
| `cmd_z.c` | The commands 4096 to 4099. |
| `upload.c` | The upload endpoint and Command 255. |
| `usbcopy.c` | The copy of a USB file for a print, and the USB state. |
| `history.c` | The print history and Commands 320 and 321. |
| `files.c` | Name rules, the client paths `/local/` and `/usb/`, mount detection, space checks. |
| `ident.c`, `clock.c` | `MainboardID`, the printer name and the learned time. |
| `discovery.c` | The UDP discovery on port 3000. |
| `hookw.c`, `hook_io.c` | The thread and the client of the hook socket. |

`gkhook/tests/test_server.c` uses `hook_io.c` as the real client of the hook socket.

### The embedded page

`gksdcp/www/index.html` is a single file with its own CSS and script. The Makefile runs `xxd -i www/index.html` to make `build/index_html.h`. The header defines `www_index_html` and `www_index_html_len`. The binary serves that array on `GET /` of the web port. Therefore a page change needs a rebuild of gksdcp.

The Makefile variable `WEBUI` (default 1) controls this. With `WEBUI=0` the page is not built in, and gksdcp sets the web port to 0. `tools/build-all.sh` passes `WEBUI` to the build.

The page is a plain SDCP client. It connects to `ws://<host>:3030/websocket` and uploads to `http://<host>:3030/uploadFile/upload`.

### Uploads

`upload.c` writes each chunk to a hidden temporary file named `.<name>.<id>.part` in the files folder. It holds a lock on the file with `flock`. After the last chunk it checks the MD5 value (if the client asks for it), calls `fsync` and then `rename` to the final name. On any error, the code removes the temporary file. gksdcp removes old leftovers at start, or later when the storage is mounted. Section 8 of [`sdcp.md`](sdcp.md) gives the rules.

### Add a command

1. Write a function `void cmd_x(struct sdcp_req *r)`. Answer with `sdcp_reply`. Use `sdcp_notice` for a reason that a person can read.
2. Register it in the `*_init` function of its file with `sdcp_register(<Cmd>, cmd_x)`.
3. If the command talks to gkhook, use `hookw_call`. The callback carries the connection identifier, never a pointer to the connection.
4. Add a test in `tests/gksdcp/` and describe the command in [`sdcp.md`](sdcp.md).

## Build environment

The build runs in Docker. The host needs Docker and a shell (Git Bash on Windows, or any Linux shell).

### Images and volume

The `Dockerfile` in the repository root has one stage for each image. The build context is the repository root. `.dockerignore` is an allowlist, so `secrets/`, `ChituUpgrade.bin` and `out/` never go into an image.

| Stage | Base | Content | Used for |
|---|---|---|---|
| `cc` | Debian buster | ARM cross compiler against glibc 2.28 | The developer image `gktwo-cc`: ARM builds of Dropbear, gksdcp and gkhook |
| `fw` | Debian bookworm | Java, ubi_reader and mtd-utils for the firmware toolkit, qemu, host gcc, Python, ARM binutils, gdb-multiarch | The developer image `gktwo-fw`: firmware build, verification, host tests, reverse engineering |
| `addons` | `cc` | The source. Builds `dropbearmulti`, `gksdcp` (`WEBUI=1`), `gksdcp-noweb` (`WEBUI=0`) and `libgkhook.so`, and runs `tools/check-glibc.sh` | The input of `addons-out` and `bridge` |
| `addons-out` | empty | `bin/` and `licenses/` from `addons` | The ARM archive of a release |
| `bridge` (default) | `fw` | The repository in `/src`, the add-ons in `/opt/gktwo` and the entrypoint `fw/image-entrypoint.sh` | The published image `ghcr.io/sufyanaslam198/gktwo-bridge` |

Build the developer images and create the volume once:

```
docker build --target cc -t gktwo-cc .
docker build --target fw -t gktwo-fw .
docker volume create gktwo-work
```

Build the `bridge` image with `docker build -t gktwo-bridge .`. Its entrypoint runs the same pipeline as `tools/build-all.sh` on the folder mounted at `/data` (see `docs/install.md`, section 6). With arguments, it runs them instead, for example `docker run --rm -it gktwo-bridge bash`. The firmware scripts take these variables, so that one script serves both layouts:

| Variable | `tools/build-all.sh` | `bridge` image | Content |
|---|---|---|---|
| `SRC` | `/src` | `/src` | The repository: `fw/`, `overlay/`, `tools/`, `tests/` |
| `DATA` | `/src` | `/data` | `ChituUpgrade.bin`, `secrets/`, `out/` |
| `BIN` | `/work/bin` | `/work/bin` (copied from `/opt/gktwo/bin`) | `dropbearmulti`, `gksdcp`, `libgkhook.so` |

The volume `gktwo-work` holds the build outputs in `/work/bin` and the reverse-engineering outputs in `/work/re`. The stock `chitu` file is in `/work/tk`. Work on Linux storage only. A Windows or OneDrive folder does not keep modes, links and line endings.

### tools/dock.sh

`tools/dock.sh <image> '<command>'` runs a command in an image. It mounts the repository at `/src` and the volume at `/work`. Set `DOCK_ARGS` for extra `docker run` options, for example `DOCK_ARGS=--privileged`.

### Build each part

| Part | Command |
|---|---|
| Dropbear | `tools/dock.sh gktwo-cc 'deps/dropbear/build.sh'` |
| gksdcp (ARM) | `tools/dock.sh gktwo-cc 'make -C gksdcp arm && mkdir -p /work/bin && cp gksdcp/build/gksdcp /work/bin/'` |
| gkhook (ARM) | `tools/dock.sh gktwo-cc 'make -C gkhook arm && mkdir -p /work/bin && cp gkhook/build/libgkhook.so /work/bin/'` |
| glibc check | `tools/dock.sh gktwo-cc 'tools/check-glibc.sh /work/bin/gksdcp /work/bin/libgkhook.so /work/bin/dropbearmulti'` |
| Image | see the next section |

`deps/dropbear/build.sh` builds Dropbear 2022.83 as one multi-call binary `dropbearmulti`. It downloads the source, makes sure that the SHA-256 value is correct and caches the source in `deps/dropbear/src`.

`tools/check-glibc.sh <elf>...` exits with 0 only if every file is an ARM executable that needs GLIBC 2.28 or older.

### One command

`tools/build-all.sh` does the complete build. It needs the key pair `secrets/gktwo_ed25519` and `secrets/gktwo_ed25519.pub`, and the stock `ChituUpgrade.bin` in the repository root. It builds the three parts, checks glibc, builds the image, verifies it and runs the end-to-end test. It then writes `out/ChituUpgrade.bin` and `out/ChituUpgrade.bin.sha256`. The output contains `VERIFY OK` and `E2E OK` and ends with the SHA-256 value.

### Firmware pipeline

`fw/build-firmware.sh` must run as root in `gktwo-fw`. It does these steps:

1. Clones `weashadow/UltraFirmwareToolkit` and checks out the pinned commit `6286345080449c6b59015a897aafd75eb8479542`. The build stops if the checkout does not match this commit.
2. Unpacks the stock firmware into the partition trees `rootfs`, `customer`, `miservice` and `appconfigs`.
3. Stages the overlay from `overlay/customer/gk` and adds the binaries, the library and the Dropbear links. Creates the Dropbear host keys once in `secrets/dropbear/`.
4. Inserts `fw/rcS.hook` into `rootfs/etc/init.d/rcS`, sets the root password from `secrets/root-password` and installs the SSH public key.
5. Replaces `customer/app0.sh` with the preload launcher. The build stops if the stock file is not the expected one-line script.
6. Repacks the partitions and the image.
7. Runs `fw/fix-header-version.py`, because the toolkit writes the version in a wrong form (`VGKtwo1.3.4`). The script rewrites the header to `GKtwoV1.3.4`.
8. Checks that the new version is greater than the stock version.

`fw/verify-image.sh <image>` compares the image with the stock firmware and prints PASS or FAIL for each check. It ends with `VERIFY OK` or `VERIFY FAILED`.

| Variable | Description |
|---|---|
| `ALLOW_CHANGED` | Paths that can differ from stock, for example `rootfs/etc/init.d/rcS rootfs/etc/shadow customer/app0.sh`. Every other difference fails. |
| `EXPECT_ADDED` | A file that lists each added path, its mode and its link target (`fw/expected-added.txt`). Every listed path must exist with that mode. Any other added path fails. |
| `PHASE1=1` | Also checks the boot hook, that the root hash matches `secrets/root-password`, Dropbear, the keys, gksdcp, gkhook, glibc (`glibc_ok`), a run of gksdcp under qemu that probes discovery and the upload endpoint (`gksdcp_runs_arm`), the `app0.sh` preload and `customer_fits`. |

The check `customer_fits` fails if the content under `customer/gk` is 409600 bytes or more.

`fw/e2e-ssh-test.sh` needs `--privileged`. It builds a root from the extracted image and starts the real init scripts in a chroot under qemu. It tests key login, password login, `scp`, the host key, `R97gksdcp` (with the refusal to store files on tmpfs, discovery, the WebSocket and the web page) and that the gkhook library loads. It ends with `E2E OK`.

## Tests

Run all tests before you open a pull request.

### gkhook host tests

```
tools/dock.sh gktwo-fw 'make -C gkhook host && make -C gkhook test'
```

The tests use a mock of `chitu` (`gkhook/tests/mock_chitu.c`). They build `tick.c`, `validate.c` and `zjog.c` with `-DGK_ABI_INDIRECT`.

| Test | Covers |
|---|---|
| `test_load` | The library loads and stays inactive in a process that is not `chitu` |
| `test_fingerprint` | Size gate, every window, and one changed byte |
| `test_proto` | Request parse and reply build |
| `test_queue` | Queue, result slots, generations |
| `test_server` | The socket server against the real hook client of gksdcp |
| `test_validate` | File checks |
| `test_tick` | Start flow, pause, resume, stop, publish time, the call chain to the stock callback |
| `test_zjog` | Every Z guard and clamp |

`test_fingerprint` also compares the table with the real `chitu` file when the file exists at `/work/tk/scripts/customer/resources/chitu`. Run `tools/dock.sh gktwo-fw 'bash tools/re/extract-chitu.sh'` to create it. Without the file, this comparison is skipped.

### gksdcp tests

```
tools/dock.sh gktwo-fw 'make -C gksdcp host && make -C gksdcp test && python3 -m pytest -q -p no:cacheprovider tests/gksdcp'
```

`make -C gksdcp test` runs the C unit tests in `gksdcp/tests/`. The pytest tests start the host build of gksdcp (`gksdcp/build/gksdcp-host`) and a mock hook (`tests/gksdcp/mock_hook.py`). They talk SDCP with the client in `tests/gksdcp/sdcp_client.py`. They cover discovery, WebSocket handling, status, print control, files, uploads, USB print, print history, Z commands, the web page, and leaks of file descriptors, memory and temporary files when connections open and close (`test_churn.py`). The tests keep sockets and temporary folders under `/tmp` in the container, not on the mounted repository.

The image `gktwo-fw` has no node, so the tests in `test_page_js.py` skip there.

### Page script tests

`tests/gksdcp/test_page_js.py` runs the web page script under node with a fake DOM and a fake WebSocket. Run it in a node container. On Windows with Git Bash:

```
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src:ro" -w /src node:20-bookworm-slim bash -c 'apt-get -qq update >/dev/null && apt-get -qq install -y python3 python3-pytest >/dev/null && cp -r /src /tmp/w && cd /tmp/w && python3 -m pytest -q -p no:cacheprovider tests/gksdcp/test_page_js.py'
```

On Linux, replace `$(pwd -W)` with `$(pwd)` and omit `MSYS_NO_PATHCONV=1`.

### Manual check with an SDCP client

Start the host build against a mock hook, then connect a WebSocket client to its port. Use `tests/gksdcp/conftest.py` as the example of the options. The option `-w <port>` sets the port of the web page, so that you can open the page in a browser without a printer.

## Development on hardware

Read the safety rules first.

**WARNING: Do not test motion with a vat that holds resin. Do not put your hands near the build plate.** The build plate can move and cause injury or damage.

**CAUTION: Do not test motion until the guards for it exist. The guards must pass the host tests.** A call without guards can drive the build plate into the vat or the screen.

Safety rules:

- Use an empty, dry vat for every test.
- Keep your hands clear of the build plate and the vat.
- Stop a print or a move from the touchscreen if the web page does not respond.
- Do not start a print on the touchscreen when a web Z move runs.
- Keep the stock `ChituUpgrade.bin` V1.3.3 on a FAT32 USB flash drive. Flash it to restore stock firmware.

### Hot-swap

A hot-swap replaces the built files on a printer that already runs the image. Never overwrite a running executable or a loaded library in place. Copy each file to a temporary name and then rename it. The printer's Dropbear has no SFTP, so use `scp -O`.

1. Build the files. `tools/build-all.sh` keeps them in `/work/bin` (see [Build environment](#build-environment)). Copy them out of the volume to the `out/` folder on the host:
   ```
   tools/dock.sh gktwo-cc 'mkdir -p /src/out && cp /work/bin/libgkhook.so /work/bin/gksdcp /src/out/'
   ```
   The source of `app0.sh` is `overlay/customer/app0.sh`.
2. Copy each file to a temporary name. The option `-i secrets/gktwo_ed25519` selects the key. Without it, SSH asks for the password.
   ```
   scp -O -i secrets/gktwo_ed25519 out/libgkhook.so root@<printer-ip>:/customer/gk/lib/libgkhook.so.new
   scp -O -i secrets/gktwo_ed25519 out/gksdcp root@<printer-ip>:/customer/gk/bin/gksdcp.new
   scp -O -i secrets/gktwo_ed25519 overlay/customer/app0.sh root@<printer-ip>:/customer/app0.sh.new
   ```
3. Set the modes and move each file over its target:
   ```
   ssh -i secrets/gktwo_ed25519 root@<printer-ip> 'chmod 755 /customer/gk/lib/libgkhook.so.new /customer/gk/bin/gksdcp.new /customer/app0.sh.new; mv /customer/gk/lib/libgkhook.so.new /customer/gk/lib/libgkhook.so; mv /customer/gk/bin/gksdcp.new /customer/gk/bin/gksdcp; mv /customer/app0.sh.new /customer/app0.sh'
   ```
4. Restart gksdcp: `ssh -i secrets/gktwo_ed25519 root@<printer-ip> '/customer/gk/init.d/R97gksdcp restart'`.
5. Restart the printer. `chitu` loads gkhook only at boot.

`/customer` has little free space when the image is installed. The `.new` files must fit next to the files that they replace. Delete the `.new` files if a copy fails.

### Read the log

Run `ssh root@<printer-ip> 'cat /tmp/gkhook.log'`. Expect these lines after a boot: `gkhook: chitu V1.3.3 fingerprint match`, `gkhook: tick installed`, `gkhook: z control enabled`. The boot scripts log to `/tmp/gk-boot.log` and gksdcp logs to `/tmp/gksdcp.log`.

### Live inspection with gdbserver

1. On the printer, run `gdbserver --attach :2345 $(pidof chitu)`. The printer must have a `gdbserver` binary.
2. On the host, run:
   ```
   gdb-multiarch -q -ex "target remote <printer-ip>:2345" -x tools/re/live-checks.gdb <chitu-binary>
   ```
   `<chitu-binary>` is the stock `chitu` file, for example `/work/tk/scripts/customer/resources/chitu` in the volume. The script `tools/re/extract-chitu.sh` creates it.
3. In gdb, run `pstate`. It prints the printer structure and the `ui` globals. Run it when the printer is idle, prints, pauses and finishes.

A stopped `chitu` halts the printer UI. Run `continue` quickly. Do this only with an empty vat.

### Read memory without a debugger

To read `chitu` memory without a stop, use `/proc/<pid>/mem` and `dd`. This only reads. Example for the printer pointer at `0x101754` (decimal 1054548):

```
pid=$(pidof chitu)
dd if=/proc/$pid/mem bs=1 skip=1054548 count=4 2>/dev/null | od -An -tx4
```

Do not write to this file.

### Recovery

| Problem | Action |
|---|---|
| gkhook misbehaves | Run `rm /customer/gk/lib/libgkhook.so` over SSH and restart the printer. `app0.sh` then starts plain `chitu`. |
| The printer does not boot or SSH does not work | Copy the stock `ChituUpgrade.bin` V1.3.3 to a FAT32 USB flash drive and flash it. This restores stock firmware. |
| A move or a print does not stop | Use the touchscreen. If it does not respond, set the printer power switch to OFF. |

## Constraints

| Item | Value |
|---|---|
| C library | glibc 2.28. `tools/check-glibc.sh` fails for a symbol that needs a newer version. |
| Architecture | ARMv7 hard-float |
| Compiler flags | `-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror`. ARM: `-march=armv7-a -mfpu=neon -mfloat-abi=hard -O2`. Both ARM builds add `-DNDEBUG`. The gkhook library is built with `-fPIC -shared -fvisibility=hidden`. |
| Size | Added content under `/customer/gk` must be less than 400 KB. `libgkhook.so` must be less than 60 KB. `fw/verify-image.sh` (`PHASE1=1`) fails at 400 KB or more (`customer_fits`). Nothing enforces the 60 KB guideline for `libgkhook.so`. |
| Root file system | Read-only. Only `/customer` (UBIFS) and `/tmp` (RAM) are writable. |
| RAM | Approximately 54 MB for everything. Keep threads, stacks and buffers small. gksdcp gets the first OOM kill. |
| Files | Print files are in `/media/emmc` (internal eMMC). |
| Clock | The printer clock starts at 1970. Use a monotonic clock for time spans. |
| Vendor code | `chitu` is not patched on disk. Do not add such a change. |

## Releases

### Version

The file `VERSION` holds the version, for example `0.1.0`. `gksdcp/Makefile` reads it into `BRIDGE_VERSION`, so the printer reports `FirmwareVersion` `V1.3.4-bridge-<version>`. The tests read the same file.

Versions follow [Semantic Versioning](https://semver.org/). Before 1.0.0, increase the minor version for a new feature or a change that is not compatible (an SDCP message, a build option, the layout of the build folder), and the patch version for a fix. A pre-release has a suffix, for example `0.2.0-rc.1`.

`CHANGELOG.md` follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Add each change that a user can see to the section `[Unreleased]` in the same pull request.

### Workflows

| Workflow | Trigger | Steps |
|---|---|---|
| `.github/workflows/ci.yml` | Push to `main`, pull request | Builds `fw`, runs the gkhook, gksdcp, pytest and page script tests, builds `bridge` (ARM build and glibc check) |
| `.github/workflows/release.yml` | Tag `v*` | Checks that the tag is `v` + `VERSION` and that `CHANGELOG.md` has a section for it, runs the host tests, pushes `ghcr.io/sufyanaslam198/gktwo-bridge`, and creates the GitHub release with the archive `gktwo-bridge-<version>-armhf.tar.gz` and its SHA-256 value |

The image tags are `<version>`, `<major>.<minor>` and `latest`. A pre-release gets only `<version>` and is marked as a pre-release on GitHub.
CI has no stock firmware, so it cannot run `VERIFY` and `E2E`. Run them on your computer before each release.

### Make a release

1. On `main`, move the entries of `[Unreleased]` in `CHANGELOG.md` to a new section `## [<version>] - <YYYY-MM-DD>`. Update the compare links at the end of the file.
2. Write the version to `VERSION`.
3. Run the tests (see [Tests](#tests)) and `tools/build-all.sh`. The output must contain `VERIFY OK` and `E2E OK`.
4. Build the `bridge` image and run it on a build folder with the stock firmware. The output must contain `VERIFY OK` and `E2E OK`:

   ```
   docker build -t gktwo-bridge .
   MSYS_NO_PATHCONV=1 docker run --rm --privileged -v "<build-folder>:/data" gktwo-bridge
   ```

5. Commit, tag and push:

   ```
   git commit -am "chore(release): <version>"
   git tag -a v<version> -m "GKtwo Bridge <version>"
   git push origin main v<version>
   ```

6. Make sure that the workflow `Release` passes and that the release and the image exist.

Do not move or delete a published tag. Fix a bad release with a new patch version.

## Repository layout

| Path | Content |
|---|---|
| `Dockerfile`, `.dockerignore` | The build images (see [Build environment](#build-environment)) |
| `VERSION`, `CHANGELOG.md` | The version and the changes of each version (see [Releases](#releases)) |
| `.github/workflows/` | `ci.yml` and `release.yml` |
| `deps/dropbear/` | `build.sh` for Dropbear (source is downloaded, not committed) |
| `docs/` | Documentation. `docs/reverse-engineering/` holds the findings about `chitu`. `gkhook-protocol.md` holds the hook protocol. `sdcp.md` holds the SDCP reference. |
| `fw/` | Firmware pipeline: `build-firmware.sh`, `verify-image.sh`, `e2e-ssh-test.sh`, `fix-header-version.py`, `rcS.hook`, `expected-added.txt`, and `image-entrypoint.sh` (the entrypoint of the `bridge` image) |
| `gkhook/` | The gkhook library: `src/`, `tests/`, `Makefile` |
| `gksdcp/` | The SDCP server and the web page: `src/`, `www/index.html`, `tests/`, `Makefile` |
| `overlay/` | Files copied to `/customer`: `app0.sh` and `gk/` (boot scripts) |
| `tests/gksdcp/` | pytest tests, the mock hook, the SDCP test client, the web page harness |
| `tools/` | `build-all.sh`, `dock.sh`, `check-glibc.sh`, and `re/` (reverse-engineering tools) |
| `out/` | Build output (not committed) |
| `secrets/` | Your SSH key pair, `root-password` and the Dropbear host keys (not committed). Only `README.md` and the `.example` files are committed. |
| `ChituUpgrade.bin` | The stock firmware that you supply (not committed) |
