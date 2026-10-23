# gkhook protocol reference

## Purpose and scope

gkhook is a library that the printer loads into chitu. It serves a control protocol on a Unix socket. Clients use the protocol to read the print status, to control a print and to move the build plate.

gksdcp is the reference client. It turns the protocol into SDCP, which [`sdcp.md`](sdcp.md) describes. You can write your own client for the hook socket. You can also add commands to the protocol.

This document describes the protocol as the code implements it. The sources are in `gkhook/src`:

| File | Content |
|---|---|
| `proto.c` | Request parser and reply builder |
| `server.c` | Socket server and the `start` command |
| `queue.c` | Queue between the socket thread and the UI tick |
| `tick.c` | The UI tick: status snapshot, print control |
| `zjog.c` | Z commands |
| `validate.c` | File checks for `start` |
| `hook.c` | Version gate and start of the server |

The UI tick is a function that gkhook installs in chitu UI loop. It runs on chitu UI thread. It is the only code that acts on the printer.

## Transport

| Item | Value |
|---|---|
| Socket type | Unix stream socket |
| Path | `/tmp/gkhook.sock` |
| Path override | Environment variable `GKHOOK_SOCK` (read by chitu at boot) |
| Socket permissions | `0600` (only root can connect) |
| Requests per connection | One |
| Request | One JSON object that ends with `\n` |
| Reply | One JSON object that ends with `\n` |
| Reply size | Less than 2048 bytes, with the `\n` |
| Reply content | No NUL byte |
| Reply time | Within 3 seconds |

The client connects, writes the request line, and reads until the `\n` of the reply. The server closes the connection after the reply.

The server handles up to 8 connections at the same time. Each connection has its own thread. A command that waits for the UI tick does not delay a `status` request. When 8 connections are open, the server answers a new connection with `hook busy` and closes it.

Two other environment variables change the behavior of gkhook. `GKHOOK_FILESDIR` sets the folder for print files (default `/media/emmc`). `GKHOOK_LOG` sets the log file (default `/tmp/gkhook.log`).

## Request format

A request is one flat JSON object.

| Rule | Detail |
|---|---|
| Line end | The request ends at the first `\n`. The server accepts the JSON and the `\n` in separate writes. |
| Maximum length | 2048 bytes. If there is no `\n` in 2048 bytes, the server closes the connection without a reply. |
| Time limit | The whole line must arrive within 1 second. If not, the server closes the connection without a reply. |
| `cmd` | Required string. The name of the command. Maximum 15 bytes. |
| `path` | String. Required for `start`. Maximum 1099 bytes after JSON decoding. A string that is empty counts as absent. |
| `mm` | Integer. Required for `z_move`. |
| Unknown keys | The server ignores them. |
| String escapes | The server decodes `\" \\ \/ \n \r \t \b \f` and `\uXXXX`. `\u0000` and surrogate values are refused. |

The server refuses a request that does not parse. This includes a request that is not an object, an empty object `{}`, a request with no `cmd`, and a `start` with no `path`. The error is `bad request`.

If `cmd` has a value that is not in the command list, the error is `unknown command`.

A reply of the error form carries a short, readable message:

```json
{"ok":false,"error":"<message>"}
```

The server removes control characters from the message and escapes `"` and `\`.

## Order of checks

The server examines a request in this order. The first check that fails gives the reply.

1. The server reads the request line. On a failure it closes the connection with no reply.
2. If chitu is not the supported version, the reply is `wrong chitu version`. This applies to every complete request line, also to a request that does not parse.
3. If the line is not a valid flat JSON object with a string `cmd`, the reply is `bad request`.
4. If the command is unknown, the reply is `unknown command`.
5. If a field that the command needs (`path`, `mm`) is absent or not valid, the reply is `bad request`.
6. The server runs the command.

## Command reference

### Common errors

Every command, also `status`, can return these errors.

| Error | When |
|---|---|
| `wrong chitu version` | The version gate failed (see Version gate). |
| `bad request` | The request is not valid for the command. |
| `unknown command` | `cmd` is not in this reference. |
| `hook starting` | The first status snapshot does not exist yet. Only `status` and `start` return this error. |
| `hook busy` | 8 connections are open already, or the server cannot start a thread for the connection. The server sends this reply before it reads the request. Retry later. |

Commands `pause`, `resume`, `stop`, `z_move`, `z_top`, `z_home` and `z_stop` go through the queue to the UI tick. They return these errors in addition:

| Error | When |
|---|---|
| `printer busy` | The queue has no free place (16 commands or 16 slots for a wait). |
| `printer not responding` | The UI tick did not act within 2 seconds. The server removes the command from the queue. |

A command that the UI tick accepts gets `{"ok":true}` after the tick acts. The tick runs often, so the wait is usually short. The maximum wait is 2 seconds.

### `status`

Request:

```json
{"cmd":"status"}
```

Success reply: see Status reply reference.

Errors: `hook starting` (no snapshot yet), `status unavailable` (the reply is too long for 2047 bytes), and the common errors.

Preconditions: none. `status` is valid in every state.

### `start`

Request:

```json
{"cmd":"start","path":"/media/emmc/x.ctb"}
```

Success reply: `{"ok":true}`

The reply means that the server accepted the start. It does not mean that the print runs. Read `status` to follow the start (see Start flow).

Preconditions:

- The state is `idle`, `complete` or `error`.
- The print window is not open (see below).
- Z does not move.

The server checks in this order and returns the first error:

| Error | When |
|---|---|
| `hook starting` | No status snapshot exists yet. |
| `printer busy` | The state is `starting`, `printing` or `paused`. Also when a start is already in progress. Also when the state is `idle` but a print window is open. |
| `dismiss the finished print on the printer first` | The state is `complete` or `error` and the print dialog is still open on the touchscreen. |
| `Z is moving` | The status shows `"moving":true`. |
| `file is not in the files folder` | `path` does not start with the files folder and `/`. |
| `bad file path` | The file name is empty, has a `/`, or is `.` or `..`. A name that only contains `..` inside (for example `a..b.ctb`) is valid. |
| `only .ctb files can be printed` | The extension is not `.ctb` (the check ignores case). |
| `file not found` | The path does not name a regular file. |
| `cannot read file` | The server cannot read the first 4 bytes of the file. |
| `not a ctb file` | The first 4 bytes are not a known CTB signature. |
| `unsupported file format` | chitu does not recognize the file format. |
| `cannot open file` | chitu cannot open the file. |
| `file resolution does not match printer` | The file resolution differs from the screen resolution of the printer. |
| `unsupported ctb version` | The format field that chitu sets when it opens the file is not 1 or 6. The touchscreen makes the same check. This field is not the version number in the CTB header. |
| `cannot start file check` | The server cannot start the thread that checks the file. |

### `pause`

Request: `{"cmd":"pause"}`

Success reply: `{"ok":true}`

The pause takes effect at the end of the current layer. The status shows `printing` until then.

Preconditions: chitu prints a layer (state `printing` after the homing step), or already pauses.

| Error | When |
|---|---|
| `not printing` | There is no print window, the printer is idle or stops, or the print is complete or in error. |
| `printer is homing - pause when printing starts` | The printer is in the homing step. |

If the printer already pauses or is paused, the reply is `{"ok":true}` and nothing changes.

### `resume`

Request: `{"cmd":"resume"}`

Success reply: `{"ok":true}`

Precondition: the state is `paused`.

| Error | When |
|---|---|
| `not printing` | There is no print window with a printer. |
| `not paused` | There is a printer, and it is not paused. |

### `stop`

Request: `{"cmd":"stop"}`

Success reply: `{"ok":true}`

Preconditions: a print runs, or a start is in progress (state `starting`).

| Error | When |
|---|---|
| `not printing` | The printer is idle, complete or in error, or there is no print window and no start in progress. |
| `printer is heating - cancel it on the touchscreen` | The print window is open and the printer is idle. The stock heat dialog is open. |

If a print runs, gkhook sends the stop command to the printer. When the printer stops, gkhook closes the print window. If the printer already stops, the reply is `{"ok":true}`.

The command also cancels a start. See Start flow.

### `z_move`

Request:

```json
{"cmd":"z_move","mm":10}
```

`mm` is a bare JSON integer. It must be one of `1`, `10`, `100`, `-1`, `-10`, `-100`. A positive value moves the build plate up. A negative value moves it down. A value in any other form (`5`, `1.5`, `"10"`, `1e2`, a missing key) gives `bad request`. gkhook queues nothing in that case.

Success reply: `{"ok":true}`. The reply comes when gkhook issues the move, not when the move ends. Read `z.moving` in the status to see the end.

gkhook limits the move to the range 0 mm to the travel. If the move crosses a limit, gkhook shortens it to the limit.

Preconditions:

- Z control is available.
- The printer is idle with no print window.
- No start is in progress and no firmware update runs.
- The axis is idle and homed.

gkhook checks in this order and returns the first error:

| Error | When |
|---|---|
| `Z control unavailable` | `z` in the status is `null`. |
| `printer busy` | A printer object exists, the print window is current or requested, or a start is in progress. |
| `firmware update in progress` | The firmware window is current or requested. |
| `Z speed setting invalid` | The speed value that chitu holds is 0 or less, or more than 30 mm/s. |
| `Z is moving` | The axis moves. |
| `home Z first` | The axis is not homed. |
| `Z travel limit unknown` | The move is up, and the travel value is not between 0 and 300 mm. |
| `Z position unknown` | The position is not a finite number. |
| `at top limit` | The move is up, and less than 0.01 mm remains to the top. |
| `at bottom limit` | The move is down, and less than 0.01 mm remains to Z = 0. |

### `z_top`

Request: `{"cmd":"z_top"}`

Success reply: `{"ok":true}`

The build plate moves to the top position (the travel value). The preconditions and errors are the same as for `z_move` with a positive step. `at top limit` applies when less than 0.01 mm remains to the top. `at bottom limit` does not apply.

### `z_home`

Request: `{"cmd":"z_home"}`

Success reply: `{"ok":true}`

gkhook halts the axis and starts the homing move. The axis does not need to be homed. The checks are `Z control unavailable`, `printer busy`, `firmware update in progress`, `Z speed setting invalid` and `Z is moving`, in this order.

### `z_stop`

Request: `{"cmd":"z_stop"}`

Success reply: `{"ok":true}`

gkhook halts the axis. The command is not limited to an idle axis. It also stops a moving axis.
The guards are `Z control unavailable`, `printer busy` and `firmware update in progress`, in this order.
`z_stop` returns `printer busy` when a print runs or a start is in progress.
It returns `firmware update in progress` when a firmware update runs.

The common queue errors `printer busy` and `printer not responding` apply to all Z commands.

## Status reply reference

Example:

```json
{"ok":true,"state":"printing","file":"/media/emmc/x.ctb","layer":12,"total_layers":400,"progress":3.00,"elapsed_s":95,"remaining_s":3010,"z":{"pos":null,"homed":false,"moving":false},"phase":"exposing","screen":[7680,4320]}
```

The keys have this order. The optional `message` comes last. gksdcp reads these fields and builds the SDCP status from them.

| Field | Type | Description |
|---|---|---|
| `ok` | boolean | Always `true` in a status reply. |
| `state` | string | The print state (see the next table). |
| `file` | string or `null` | The full path of the current job. `null` when there is no print window with a printer, or the path is empty. gkhook truncates the path to 400 bytes. |
| `layer` | integer or `null` | The current layer. `null` when it is not known. |
| `total_layers` | integer or `null` | The number of layers. `null` when the file is not open or the number is not valid. |
| `progress` | number or `null` | Layer divided by total layers, from 0 to 100, with 2 decimals. `null` when the layer or the total is not known. |
| `elapsed_s` | integer or `null` | Print time in seconds. `null` when there is no print window with a printer. |
| `remaining_s` | integer or `null` | Estimated time to the end, in seconds. `null` until chitu has an estimate. |
| `z` | object or `null` | The Z state. `null` when Z control is unavailable. |
| `phase` | string or `null` | The step of the print. One of `homing`, `exposing`, `pausing` or `stopping`. `null` in all other cases. |
| `screen` | array or `null` | The resolution of the printer screen in pixels, as `[width,height]`. `null` when chitu does not report a valid size. |
| `message` | string | Optional. The reason that a start ended. Absent when there is no reason. |

The `z` object has these fields:

| Field | Type | Description |
|---|---|---|
| `pos` | number or `null` | The Z position in mm, with 2 decimals. `null` unless the axis is homed. |
| `homed` | boolean | `true` when the axis is homed. |
| `moving` | boolean | `true` when the axis moves. |

When Z control is available but the axis is not homed, `z` is `{"pos":null,"homed":false,"moving":false}`.

### State values

| `state` | Description | gksdcp allows start, replace, delete |
|---|---|---|
| `idle` | No print and no start in progress. | Yes |
| `starting` | A start was accepted, or the printer prepares a print (for example the heat dialog). For a start through gkhook, this includes the homing step. chitu does not print yet. | No |
| `printing` | The printer prints, pauses or stops. For a print started on the touchscreen, this includes the homing step. | No |
| `paused` | The printer is paused. | No |
| `complete` | The print is finished. The finished-print dialog can still be open. | Yes |
| `error` | The print ended in an error. The dialog can still be open. | Yes |

gksdcp refuses to replace or delete a file in any state that is not `idle`, `complete` or `error`. gksdcp refuses to upload a file when the state is `printing` or `paused`. A state that a future version adds counts as busy. When gksdcp cannot read the state, it refuses to replace or delete a file, and to print from a USB flash drive, with the message `printer state unknown`.

The numeric fields are `null` until a print runs. The gkhook snapshot has a new state at most 250 ms after a change, and at once after a command.

### `message`

`message` is present after a start ended without a print. It stays until the next start is accepted, or until the UI tick sees that chitu prints. The state then returns to the state that chitu reports, normally `idle`, or `error` when the print window ended in an error.

| Message | Cause |
|---|---|
| `file check failed` | The full file check failed. |
| `cannot open file` | The full file check cannot open the file. |
| `cannot read file` | The checker cannot read the file. |
| `not a ctb file` | The signature check failed. |
| `printer busy` | The printer was not ready when the UI tick took the start. |
| `Z is moving` | The Z axis moved when the UI tick took the start. |
| `bad file path` | The path is 1024 bytes or longer. |
| `cannot start file check` | gkhook cannot start the file check. |
| `file check timed out` | The file check did not end within 600 seconds. The heat dialog has no time limit. |
| `start failed` | The print window did not open within 5 seconds. Or the print ended in `complete` or `error` before it printed. Or the checker finished and the start did not arrive within 10 seconds. |
| `print stopped on the printer` | The print window closed after it opened, and gkhook did not send a stop (for example a stop on the touchscreen). |
| `start cancelled` | A client sent `stop` during the start. |

## Start flow

1. gkhook examines the state and the file with cheap checks. It runs them before it replies. A failure gives `{"ok":false,"error":"..."}`.
2. On success gkhook records the start and replies `{"ok":true}`. The next status shows `"state":"starting"`.
3. A separate thread runs the full file check. The check can take minutes for a large file.
4. If the check fails, the start ends. The state returns to chitu state. The status carries the reason in `message`.
5. If the check passes, the thread queues the start for the UI tick. The tick checks the printer again, then asks chitu to open the print window.
6. When chitu reports a state of print, the start is complete. The state is `printing`. The tick clears `message`.

gkhook refuses a second `start` with `printer busy` until the first start ends.

### Cancel

A client can send `stop` while the state is `starting`.

| Situation | Result |
|---|---|
| The file check still runs, or the print window is not open yet | gkhook cancels the start and does not touch the printer. The reply is `{"ok":true}`. `message` is `start cancelled`. gkhook drops the later start request of the checker. |
| The print window is open and the printer homes | gkhook stops the printer. The start ends with `start cancelled` when the window closes. |
| The print window is open and the printer is idle (heat dialog) | The reply is `{"ok":false,"error":"printer is heating - cancel it on the touchscreen"}`. gkhook cannot close the stock heat dialog. |

### Finished print dialog

A print in state `complete` or `error` leaves its dialog open on the touchscreen. A new start cannot proceed until the user closes the dialog there. While the dialog is open, `start` returns `dismiss the finished print on the printer first`.

## Version gate

At boot gkhook compares chitu with the supported version (V1.3.3). The check uses the file size of `/proc/self/exe` and byte fingerprints that it reads from the memory of the process (`/proc/self/mem`).

If the version does not match, gkhook does not install the UI tick. It still serves the socket. Every request that has a complete line then gets:

```json
{"ok":false,"error":"wrong chitu version"}
```

A client must treat this reply as "the printer cannot be controlled". gksdcp shows the text in the field `GKBridge.Message` of the status, with the prefix `hook: `.

If the library loads into a process that is not chitu, gkhook does nothing and serves no socket.

## Rules for the server

1. Read until `\n`. The client can send the JSON and the `\n` in separate writes. A single `recv` can return a partial request.
2. End each reply with `\n`. Keep each reply shorter than 2048 bytes. Do not put a NUL byte in a reply.
3. Keep the status reply compatible. gksdcp reads it on every poll. Keep messages short and readable, because gksdcp sends them to SDCP clients as notices.
4. Reply to every command within 3 seconds.
5. Reply to `start` when the server accepts the start, not when the print begins. Validate the file before the reply. An invalid file crashes chitu.
6. Accept start paths of the form `<files folder>/<name>`. The files folder is `/media/emmc` by default.
7. Do not block chitu UI thread. Accept and answer connections on the server thread. Build the status reply from a snapshot that the UI tick publishes. Queue commands for the tick.
8. Return exactly one result for each queued command.
9. Send a Z command or a print command to the printer only from the UI tick.

## Rules for the client

1. Send one request for each connection. End the line with `\n`. Send the whole line within 1 second.
2. Keep the request line shorter than 2048 bytes.
3. Wait at most 3 seconds for the reply. gksdcp uses 2.3 seconds for a command.
4. Read until the first `\n`. Treat each of these as "hook unavailable":
   - The connection closes before the `\n`.
   - The reply has a NUL byte.
   - The reply is 2048 bytes or longer.
   - The reply is empty.
   - The connection fails or times out.
5. The server serves up to 8 connections at the same time and answers more with `hook busy`. A command that waits for the tick (up to 2 seconds) does not delay a `status` request.
6. Poll `status` at most once every 250 ms. gksdcp polls once every 250 ms, for all its clients together.
7. Check `ok` before you read other fields. Show `error` to the user as it is.
8. Do not send `start` unless the state is `idle`, `complete` or `error`. gkhook refuses the other states.
9. Send `z_move` only with the six integer values that this document lists.

## Compatibility notes

- A client must ignore unknown fields in a reply. The status can get new fields in a later version.
- A client must treat an unknown `state` value as busy.
- If `z` is absent or `null`, the client must treat Z control as unavailable. Do not send Z commands.
- The server ignores unknown keys in a request.
- Error strings are for people. A client can compare them, but must show a string that it does not know.
- gksdcp sends only `.ctb` files to `start`, and gkhook prints only `.ctb` files. gkhook refuses the other extensions with `only .ctb files can be printed`.
