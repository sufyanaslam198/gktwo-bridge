# SDCP reference

gksdcp is the server program on the printer. It speaks SDCP V3.0.0 (Smart Device Control Protocol), a printer control protocol that uses WebSocket and JSON. Slicers know this protocol. gksdcp also serves the web page.

This document describes what gksdcp does, as the code implements it. It does not repeat the protocol text. It gives the commands that the printer supports, the exact replies and the places where gksdcp is different from the protocol.

gksdcp reads and controls the printer through the hook socket (see [`gkhook-protocol.md`](gkhook-protocol.md)).

**WARNING: Do not connect the printer to the internet. SDCP has no authentication. Anyone who can reach the ports can start a print or move the build plate.**

Contents:

1. [Ports](#1-ports)
2. [Discovery](#2-discovery)
3. [WebSocket connection](#3-websocket-connection)
4. [Message format](#4-message-format)
5. [Supported commands](#5-supported-commands)
6. [Print control](#6-print-control)
7. [Files](#7-files)
8. [Upload](#8-upload)
9. [Status](#9-status)
10. [Attributes](#10-attributes)
11. [Print history](#11-print-history)
12. [Project extensions](#12-project-extensions)
13. [Differences from the protocol](#13-differences-from-the-protocol)
14. [Options and build options](#14-options-and-build-options)
15. [Clients](#15-clients)

`<printer-ip>` is the IP address of the printer.

## 1. Ports

| Port | Protocol | Use |
|---|---|---|
| 3000 | UDP | Discovery |
| 3030 | TCP | WebSocket at `/websocket`, and the upload at `/uploadFile/upload` |
| 80 | TCP | The web page. It is optional (see section 14). |

None of these ports has authentication or encryption. Use them only on a trusted local network.

Port 80 serves one resource: `GET /`. Every other request to port 80 gets HTTP 404. A request with a malformed request line or header gets HTTP 400 on both ports. Port 3030 gives HTTP 400 for a request to `/websocket` that is not a `GET` with a WebSocket upgrade. It accepts `POST` and `HEAD` on `/uploadFile/upload`. Every other method on that path, and every other path, gets HTTP 404.

## 2. Discovery

A client sends the text `M99999` in a UDP datagram to port 3000. The text can have a line break at the end. The client can use broadcast or the address of the printer. gksdcp ignores every other datagram.

gksdcp replies to the address and port of the sender. The reply comes from the address that the client used. Example:

```json
{"Id":"da6371eab4f602742504df5e03abd18b","Data":{"Name":"GKtwo","MachineName":"GKtwo","BrandName":"Uniformation","MainboardIP":"<printer-ip>","MainboardID":"0000020000aabbcc","ProtocolVersion":"V3.0.0","FirmwareVersion":"V1.3.4-bridge-0.1.0"}}
```

| Field | Value |
|---|---|
| `Id` | A fixed identifier of the project: `da6371eab4f602742504df5e03abd18b`. |
| `Name` | The name of the printer. The default is `GKtwo`. Command 192 changes it. |
| `MachineName` | `GKtwo`. A build option changes it. |
| `BrandName` | `Uniformation`. A build option changes it. |
| `MainboardIP` | The address of the printer on the network that the probe used. |
| `MainboardID` | `0000` and the 12 hex digits of the MAC address of the interface `wlan0`, in lower case. |
| `ProtocolVersion` | `V3.0.0`. |
| `FirmwareVersion` | The image version, then `-bridge-`, then the GKtwo Bridge version (the file `VERSION`). |

If gksdcp cannot read the MAC address, it uses the `MainboardID` `0000000000000001`. It reads the MAC address again, at most once each second, when it next needs the `MainboardID`, and then changes to the real value. The option `-m` sets a fixed value.

## 3. WebSocket connection

A client connects to `ws://<printer-ip>:3030/websocket`. The server accepts text frames that contain JSON. It answers a ping frame with a pong frame. It answers the text frame `ping` with the text frame `pong`.

Immediately after the handshake, the server sends the attributes message and then the status message to the new connection. A client that cannot use UDP discovery, for example from another subnet, gets the `MainboardID` from these messages.

| Item | Value |
|---|---|
| Connections | Up to 32 at the same time (WebSocket, upload and web page together). More connections get HTTP 503. |
| Largest message | 65536 bytes. |
| Idle time | The server closes a WebSocket connection that is silent for 120 seconds. Send `ping` at intervals to keep it open. |
| Slow client | The server closes a connection when more than 512 KB wait to be sent. |
| Fragmented text | The server closes the connection. |

### Origin rule

A browser sends the header `Origin`. gksdcp accepts a request in these cases:

- The request has no `Origin` header. Slicers send none.
- The `Origin` is `http://<host>` or `http://<host>:80`, and `<host>` is the host name that the client used to reach the printer. This is the web page of the printer.

gksdcp refuses every other `Origin`. The WebSocket handshake gets HTTP 403. The upload gets HTTP 403 and the body that section 8 describes, with the message `origin not allowed`.

gksdcp adds the header `Access-Control-Allow-Origin` to an HTTP reply only when it accepts the `Origin`. It sets the header to that `Origin`, and adds `Vary: Origin`. It never sends `*`.

Every HTTP reply has the headers `X-Frame-Options: DENY` and `X-Content-Type-Options: nosniff`, except the WebSocket handshake and the HTTP 503 reply when there are too many connections. A browser does not show the web page in a frame.

## 4. Message format

A request has this form:

```json
{"Id":"<any>","Data":{"Cmd":0,"Data":{},"RequestID":"<id>","MainboardID":"<id>","TimeStamp":1700000000,"From":0},"Topic":"sdcp/request/<MainboardID>"}
```

gksdcp reads `Data.Cmd` (an integer) and `Data.RequestID` (a string of 256 bytes or less). If a request has no readable `Cmd`, or no `RequestID`, or a `RequestID` longer than 256 bytes, gksdcp sends no response. The connection stays open.

gksdcp never refuses a request for a wrong `MainboardID` or `Topic`. It writes the first occurrence of each wrong value to the log.

A request with a readable `Cmd` and `RequestID` gets exactly one response:

```json
{"Id":"da6371eab4f602742504df5e03abd18b","Data":{"Cmd":0,"Data":{"Ack":0},"RequestID":"<id>","MainboardID":"0000020000aabbcc","TimeStamp":1700000000},"Topic":"sdcp/response/0000020000aabbcc"}
```

The response has the same `Cmd` and `RequestID` as the request. `Ack` is the result. `Ack` 0 means success. A command that gksdcp does not know gets `Ack` 1.

The server sends four other kinds of frame. A client can receive them at any time.

| Topic | Content |
|---|---|
| `sdcp/status/<MainboardID>` | The status (section 9). |
| `sdcp/attributes/<MainboardID>` | The attributes (section 10). |
| `sdcp/notice/<MainboardID>` | A message text, as `{"Id":...,"Data":{"Data":{"Message":"<text>","Type":100},"MainboardID":...,"TimeStamp":...},"Topic":"sdcp/notice/<MainboardID>"}`. `Type` 100 is a project extension. |
| `sdcp/error/<MainboardID>` | An error code, as `{"Id":...,"Data":{"Data":{"ErrorCode":<n>},"MainboardID":...,"TimeStamp":...},"Topic":"sdcp/error/<MainboardID>"}`. gksdcp sends `ErrorCode` 1 when the check of an upload fails (checksum mismatch). |

The server sends notices and errors to all connections.

### Time

gksdcp does not set the clock of the printer. The clock of the printer starts at 1970. gksdcp learns the real time from the `TimeStamp` field of requests, when it is a value in seconds or in milliseconds between the years 2024 and 2100. It then adds an offset to the monotonic clock. Until then it uses the clock of the printer.

## 5. Supported commands

| Cmd | Name | Result |
|---|---|---|
| 0 | Status | `Ack` 0, then a status frame to the requester. |
| 1 | Attributes | `Ack` 0, then an attributes frame to the requester. |
| 128 | Start print | Section 6. |
| 129 | Pause print | Section 6. |
| 130 | Stop print | Section 6. |
| 131 | Resume print | Section 6. |
| 132, 133 | Not applicable | `Ack` 1. |
| 192 | Rename the printer | Section 7. |
| 255 | Cancel an upload | Section 8. |
| 258 | List files | Section 7. |
| 259 | Delete files | Section 7. |
| 320 | List print tasks | Section 11. |
| 321 | Task details | Section 11. |
| 386 | Camera | `Ack` 2, and `"VideoUrl":""`. The printer has no camera. |
| 387 | Time-lapse | `Ack` 1. |
| 4096 to 4099 | Move the build plate | Section 12. |

The server sends a status frame to all connections when the status changes. It sends at most one such frame each 500 ms.

## 6. Print control

### Start (Cmd 128)

```json
{"Cmd":128,"Data":{"Filename":"/local/part.ctb","StartLayer":0}}
```

| Field | Rule |
|---|---|
| `Filename` | `/local/<name>` for a file in the top level of the internal storage. `/usb/<path>` for a file on a USB flash drive (see below). |
| `StartLayer` | Optional. It must be 0. |

The printer prints only `.ctb` files. A file in a folder of `/local/` is refused. Move it to the top level first.

gksdcp answers when the hook accepts or refuses the start. It does not wait for the print to begin. After `Ack` 0, the state is `starting` until the printer prints.

| `Ack` | Description |
|---|---|
| 0 | The printer accepted the start. |
| 1 | The printer is busy, or the request is not valid for another reason. |
| 2 | The file does not exist, or the path is not valid. |
| 4 | The printer cannot read the file, it cannot write to the internal storage, or it has too little space. |
| 5 | The resolution of the file is not the resolution of the printer. |
| 6 | The file format is not supported. |

Each refusal also sends a notice, after the response. These are the texts that gksdcp produces itself:

| Notice | Cause |
|---|---|
| `printer busy` | A print, a start, an upload or a USB copy is in progress, or the printer is not ready. |
| `printer state unknown` | The path starts with `/usb/` and gksdcp cannot read the state of the printer (`HookOk` is `false`). |
| `storage not mounted` | The path starts with `/usb/` and the internal storage is not mounted yet. |
| `bad file path` | `Filename` is absent, is not a string, or is not a valid path. |
| `USB drive not mounted` | The path starts with `/usb/` and no USB flash drive is present. |
| `only files in /local/ can be printed` | The path points to a storage that is not `/local/`. |
| `move the file to the top folder to print it` | The file is in a folder. |
| `start layer is not supported` | `StartLayer` is not 0. |
| `only .ctb files can be printed` | The file name does not end in `.ctb`. |
| `file not found` | The file does not exist. |
| `cannot read file` | The printer cannot read the file. |
| `not a ctb file` | The file does not have the CTB format. |
| `not enough space on internal storage` | The copy of a USB file does not fit. |
| `printer not responding` | gkhook did not answer within 2.3 seconds. `Ack` is 1. |
| `bad reply from printer` | The answer of gkhook was not valid JSON. `Ack` is 1. |
| `printer refused` | gkhook refused the command without a text. `Ack` is 1. |

When the hook refuses the start, the notice is the text of the hook. The texts are in [`gkhook-protocol.md`](gkhook-protocol.md). The table converts these texts to `Ack`:

| Hook text | `Ack` |
|---|---|
| `file not found`, `file is not in the files folder`, `bad file path` | 2 |
| `cannot read file`, `cannot open file`, `cannot start file check` | 4 |
| `file resolution does not match printer` | 5 |
| `not a ctb file`, `unsupported file format`, `unsupported ctb version`, `only .ctb files can be printed` | 6 |
| Any other text (for example `printer busy`, `hook starting`, `Z is moving`) | 1 |

If a start ends without a print, gksdcp sends the reason as a notice once. Examples: `start cancelled`, `file check failed`, `file check timed out`. It also sets `ErrorNumber` in the status: 2 for `file check failed`, `cannot open file` or `cannot read file`, 4 for `not a ctb file`, and 0 for every other reason. `PrintInfo.Status` becomes 8. The task of the start ends with `TaskStatus` 3 when the state returns to `idle`, or with `TaskStatus` 2 when the state becomes `error`. This is also true when the file check fails before the status shows `starting`, and when the same start fails again. One exception: if the state was already `error` before the start and no status shows `starting`, the state does not change, so the task ends with `TaskStatus` 3.

### Print from a USB flash drive

The printer cannot print a file directly from the USB flash drive. gksdcp copies the file to the internal storage first, with the same name. It then starts the print with the copy. The touchscreen does the same.

1. gksdcp checks that the file is a `.ctb` file and that the internal storage has enough space.
2. gksdcp answers `Ack` 0. The status shows `CurrentStatus` `[1]` and `PrintInfo.Status` 10.
3. gksdcp copies the file in small steps. The WebSocket stays available.
4. When the copy is complete, gksdcp starts the print.

Command 130 during the copy cancels the copy and removes the partial file. You can remove the USB flash drive after the copy. The print is not affected. gksdcp refuses to replace a file of the same name that the printer has open.

A copy that fails sends one of these notices: `cannot write to internal storage`, `USB drive removed during copy`, `cannot read from USB drive`, `USB file changed during copy`, `printer busy`, `printer state unknown`. If the hook refuses the start after the copy, the notice is the hook text, for example `file resolution does not match printer`. The client already has `Ack` 0 in this case.

### Pause, stop and resume (Cmd 129, 130, 131)

These commands have no fields. gksdcp sends them to the hook. `Ack` is 0 when the hook accepts the command. Otherwise `Ack` is 1 and a notice gives the hook text, for example `not printing`, `not paused` or `printer is heating - cancel it on the touchscreen`. If gkhook does not answer in time, or its answer cannot be used, the notice is one of the texts that gksdcp produces itself: `printer not responding`, `bad reply from printer` or `printer refused`. The commands 4096 to 4099 (section 12) do the same.

Command 130 also cancels a start that has not begun.

## 7. Files

### List files (Cmd 258)

```json
{"Cmd":258,"Data":{"Url":"/local/"}}
```

`Url` is `/local/<folder>` or `/usb/<folder>`. The default is `/local/`. The response has `FileList`:

```json
{"Ack":0,"FileList":[{"name":"/local/part.ctb","usedSize":123456789,"totalSize":7000000000,"storageType":0,"type":1}]}
```

| Field | Description |
|---|---|
| `name` | The full path, with the prefix `/local/` or `/usb/`. |
| `usedSize`, `totalSize` | Bytes used and bytes in total on that storage. |
| `storageType` | 0 for the internal storage, 1 for the USB flash drive. |
| `type` | 0 for a folder, 1 for a file. |

The list has folders first. It then has the newest files first. It shows only folders and files with the extension `.ctb`. It does not show names that are not valid (see File names), for example names that start with a dot. The list has at most 500 entries. If more exist, it keeps the folders and the newest files.

If no USB flash drive is present, the list for `/usb/` is empty with `Ack` 0. For a path that is not valid, `FileList` is empty and `Ack` is 1.

### Delete files (Cmd 259)

```json
{"Cmd":259,"Data":{"FileList":["/local/old.ctb"],"FolderList":["/local/folder"]}}
```

`Ack` is 0 when gksdcp processed the request. `ErrData` lists each path that gksdcp did not delete. When you delete the file that the printer has open, `Ack` is 0 and the path is in `ErrData`. A folder is deleted with all its content, to a depth of 8 levels.

gksdcp refuses every deletion, with `Ack` 1 and the notice `printer busy`, in these cases:

- The state is not `idle`, `complete` or `error`.
- An upload, a USB copy or a start is in progress.

When gksdcp cannot read the state of the printer (`HookOk` is `false`), it refuses every deletion with `Ack` 1 and the notice `printer state unknown`. A print can run on the touchscreen.

gksdcp always refuses to delete the file that the printer has open, or a folder that contains it. This includes the state `complete`, because the finished-print dialog can still hold the file. Dismiss the finished print on the touchscreen, then delete the file. gksdcp never deletes a storage root.

A request with neither `FileList` nor `FolderList`, or with a value that is not an array, gets `Ack` 1.

### File names

A name is valid when it has 1 to 200 bytes, does not start with a dot, has valid UTF-8, has no control characters (bytes below 0x20, and 0x7f), and has none of the characters `/ \ : * ? " < > |`. The web page is stricter. It accepts letters, digits, space and the characters `. _ ( ) + -`, with at most 128 characters.

### Rename the printer (Cmd 192)

```json
{"Cmd":192,"Data":{"Name":"Shop printer"}}
```

`Name` has 1 to 64 bytes of valid UTF-8, with no control character. gksdcp saves it in the folder `/customer/gk/var`, so it stays after a restart. `Ack` is 0 and an attributes frame goes to all connections. For a name that is not valid, `Ack` is 1.

## 8. Upload

A client sends a file in chunks. Each chunk is one `POST` with `multipart/form-data` to `http://<printer-ip>:3030/uploadFile/upload`.

| Part | Content |
|---|---|
| `S-File-MD5` | The MD5 value of the whole file, in hex. |
| `Check` | `1` to check the MD5 value when the last chunk arrives. gksdcp checks only when `Check` is `1` and `S-File-MD5` has 32 hex digits. Otherwise there is no check. |
| `Offset` | The byte position of the chunk in the file. |
| `Uuid` | An identifier of the upload. Use the same value for all chunks. |
| `TotalSize` | The size of the whole file, in bytes. The largest value is 64 GiB. |
| `File` | The chunk. The file name is the `filename` of this part. |

All other parts must come before `File`. The body can be large. gksdcp writes it to the storage while it arrives and never holds a whole file in RAM.

`HEAD /uploadFile/upload` gives HTTP 200 with no body, for every `Origin`. Clients use it to test that the port is reachable.

### Rules

- The file name must be valid (section 7) and end in `.ctb`.
- The chunks must come in order. The `Offset` of a chunk is the end of the previous chunk.
- One upload can run at one time. A request with a different `Uuid` gets HTTP 409, and the message `another upload is running`. If the active upload has had no chunk for more than 5 seconds, a valid request with a new `Uuid` replaces it. gksdcp removes the partial file of the old upload. The old upload ends with an error.
- An upload with no chunk for more than 5 seconds does not block other commands. Command 128, 259 or 4096 to 4098 ends it and removes its partial file. `CurrentStatus` is then no longer `[2]`.
- An upload with no new chunk for 60 seconds ends and its partial file is removed. The option `-S` changes this time.
- gksdcp refuses every upload while the state is `printing` or `paused`, and during a USB copy.
- To replace a file of the same name, the state must be `idle`, `complete` or `error`, and no start must be in progress. gksdcp checks this when each chunk arrives, and again after the last chunk, before it replaces the file.
- When gksdcp cannot read the state of the printer, it refuses to replace a file, with HTTP 409 and the message `printer state unknown`. An upload with a new file name is allowed.
- gksdcp keeps 16 MiB of the free space. The option `-R` changes this value.
- The internal storage must be mounted. gksdcp refuses to store a file on the temporary storage that the printer uses before it mounts the storage.
- The status shows `CurrentStatus` `[2]` while the upload runs, only when the state is idle. In the states `complete` and `error`, `CurrentStatus` stays `[0]`.
- The partial file is a hidden file in the files folder, with a name that ends in `.part`. gksdcp renames it to the final name after the last chunk. When gksdcp starts, it removes old `.part` files. If the internal storage is not mounted at that time, gksdcp removes them when the storage is mounted.

### Replies

Success has HTTP 200 and this body:

```json
{"code":"000000","messages":null,"data":{},"success":true}
```

A failure has this body. `<n>` is the error code, and `<text>` is the message. The entry with the text is present only when gksdcp has a message:

```json
{"code":"111111","messages":[{"field":"common_field","message":<n>},{"field":"common_field","message":"<text>"}],"data":null,"success":false}
```

Some clients, such as DragonFruit, look only at the HTTP status. For this reason a failure never has HTTP 200.

| HTTP status | Cause | Messages |
|---|---|---|
| 400 | The request is not valid. | `invalid file name` (field `filename`), `invalid Offset`, `invalid TotalSize`, `invalid Uuid`, `missing field before File`, `file name changed` (field `filename`), `total size changed`, `more data than TotalSize`, `field too long`, `malformed upload request`, `multipart/form-data required`, `invalid boundary`, `Transfer-Encoding is not supported`, `File part missing`, `incomplete multipart request`. An offset that is negative, or that does not follow the previous chunk (error code -1 or -2), also gets 400, with only the numeric entry and no text. |
| 403 | The `Origin` is not allowed. | `origin not allowed` |
| 409 | The printer cannot accept the file now. | `printer busy`, `printer state unknown`, `another upload is running`, `upload cancelled` |
| 500 | The storage write failed, or the MD5 value does not match. | `md5 mismatch` (the error code is -4), or no message (the error code is -3) |
| 503 | The storage is not mounted. | `storage not mounted` |
| 507 | The storage has too little free space. | `not enough space` |

### Cancel an upload (Cmd 255)

```json
{"Cmd":255,"Data":{"Uuid":"<uuid>","FileName":"part.ctb"}}
```

| `Ack` | Description |
|---|---|
| 0 | gksdcp cancelled the upload and removed the partial file. |
| 1 | No upload runs. |
| 3 | The `Uuid` or the file name does not match the active upload. |

## 9. Status

A status frame has this form (shortened):

```json
{"Status":{"CurrentStatus":[0],"PreviousStatus":0,"PrintScreen":0,"ReleaseFilm":0,"TempOfUVLED":0,"TimeLapseStatus":0,"TempOfBox":0,"TempTargetBox":0,"PrintInfo":{"Status":0,"CurrentLayer":0,"TotalLayer":0,"CurrentTicks":0,"TotalTicks":0,"Filename":"","ErrorNumber":0,"TaskId":""},"GKBridge":{"HookOk":true,"Z":{"Pos":12.35,"Homed":true,"Moving":false},"Message":""}},"MainboardID":"0000020000aabbcc","TimeStamp":1700000000,"Topic":"sdcp/status/0000020000aabbcc"}
```

### Fixed values

The printer has no sensors for these fields. They are always 0, never `null`: `PrintScreen`, `ReleaseFilm`, `TempOfUVLED`, `TimeLapseStatus`, `TempOfBox`, `TempTargetBox`.

### `CurrentStatus`

`CurrentStatus` is an array with one number.

| Value | Description |
|---|---|
| 0 | Idle. |
| 1 | A print, a start or a USB copy is in progress. |
| 2 | An upload is in progress, and the state is idle. In the states `complete` and `error` the value stays 0. |

`PreviousStatus` is the earlier value of `CurrentStatus`.

### `PrintInfo`

| Field | Description |
|---|---|
| `Status` | The step of the print (table below). |
| `CurrentLayer`, `TotalLayer` | The layer and the number of layers. 0 when not known. |
| `CurrentTicks` | The elapsed time of the print, in ms. |
| `TotalTicks` | The elapsed time plus the remaining time, in ms. 0 until the printer has an estimate. |
| `Filename` | The file as the client names it, for example `/local/part.ctb`. For a print from a USB flash drive, it is the `/usb/` path. |
| `ErrorNumber` | 0, or the error number of a start that ended without a print (see section 6). |
| `TaskId` | The identifier of the print task. Empty when no print runs. |

| `PrintInfo.Status` | Description |
|---|---|
| 0 | Idle. |
| 1 | The build plate goes to the home position, for a print started on the touchscreen. |
| 3 | The printer prints. |
| 5 | The printer pauses. |
| 6 | Paused. |
| 7 | The printer stops. |
| 8 | Stopped or ended with an error. |
| 9 | The print is complete. |
| 10 | A start is in progress: the file check, the heat dialog, a USB copy, or the homing step of a start through gksdcp. |

After a print ends, `Status` keeps 8 or 9 until the next print. The values 2 and 4 do not occur.

### `GKBridge`

`GKBridge` is a project extension. Other clients ignore it. The web page uses it.

| Field | Description |
|---|---|
| `HookOk` | `true` when gksdcp can talk to gkhook, and gkhook accepts the version of chitu. |
| `Z` | `null` when Z control is not available. Otherwise an object. |
| `Z.Pos` | The Z position in mm, with 2 decimals. `null` when the axis is not homed. |
| `Z.Homed` | `true` when the axis is homed. |
| `Z.Moving` | `true` when the axis moves. |
| `Message` | The reason that a start ended, or the hook text. When `HookOk` is `false` and no other text exists, it is `hook: ` and the reason, for example `hook: hook not running`. Empty when there is no message. |

## 10. Attributes

An attributes frame has the topic `sdcp/attributes/<MainboardID>`. gksdcp sends it after Cmd 1, and when the name, the USB state or the IP address changes.

| Field | Value |
|---|---|
| `Name`, `MachineName`, `BrandName` | As in section 2. |
| `ProtocolVersion` | `V3.0.0`. |
| `FirmwareVersion` | As in section 2. |
| `Resolution` | The screen resolution that chitu reports, as `<width>x<height>`. `7680x4320` if chitu does not report it. |
| `XYZsize` | `228.1x128.3x245`. A build option changes it. |
| `MainboardIP`, `MainboardID` | The address and the identifier of the printer. |
| `NetworkStatus` | `wlan`. |
| `UsbDiskStatus` | 1 when a USB flash drive is present, otherwise 0. |
| `Capabilities` | `["FILE_TRANSFER","PRINT_CONTROL"]`. |
| `SupportFileType` | `["CTB"]`. |
| `RemainingMemory` | The free space of the internal storage, in bytes. |
| `CameraStatus`, `NumberOfVideoStreamConnected`, `MaximumVideoStreamAllowed` | 0. |
| `DevicesStatus` | `LCDStatus` is 1. `ZMotorStatus` is 1 when Z control is available. All other fields are 0. |

The printer has no UV LED temperature sensor, no heated chamber and no release film. The related fields are 0.

## 11. Print history

gksdcp keeps a record of each print. It also records a print that you start on the touchscreen. It keeps the 50 latest records in the file `/customer/gk/var/history.json`.

| Cmd | Request | Response |
|---|---|---|
| 320 | No fields. | `HistoryData`: the task identifiers, newest first. It can be `[]`. |
| 321 | `Id` or `TaskIdList`: an array of task identifiers. | `HistoryDetailList`: one object for each known identifier. |

Each object has these fields: `Thumbnail` (always empty), `TaskName` (the file name), `BeginTime`, `EndTime`, `TaskStatus`, `SliceInformation` (always `{}`), `AlreadyPrintLayer`, `TaskId`, `MD5` (always empty), `CurrentLayerTalVolume`, `TimeLapseVideoStatus`, `TimeLapseVideoUrl` and `ErrorStatusReason`.

`TaskStatus` is 0 while the task runs, 1 for a complete print, 2 for an error and 3 for a stopped print. `EndTime` is 0 while the task runs, and never 0 for a finished task.

## 12. Project extensions

These parts are not in SDCP. Other clients do not use them.

### Commands 4096 to 4099

The commands move the build plate. They use the same hook commands and the same safety rules as the touchscreen. gkhook decides. The response is `Ack` 0 when the hook accepts the command. Otherwise it is `Ack` 1, and a notice gives the reason.

| Cmd | Name | Data | Hook command |
|---|---|---|---|
| 4096 | Home | None | `z_home` |
| 4097 | Move | `{"Mm":<n>}` with `<n>` equal to 1, 10, 100, -1, -10 or -100 | `z_move` |
| 4098 | Top | None | `z_top` |
| 4099 | Stop | None | `z_stop` |

gksdcp refuses commands 4096 to 4098 with `Ack` 1 and the notice `printer busy` when an upload, a USB copy or a start is in progress. It refuses command 4097 with another value of `Mm`, and the notice `bad request`. It always sends command 4099 to the hook.

The notices of the hook for these commands are in [`gkhook-protocol.md`](gkhook-protocol.md). Examples: `home Z first`, `Z is moving`, `at top limit`, `at bottom limit`.

**WARNING: Make sure that there are no hands, tools or objects near the build plate before you send a movement command. The build plate moves with force.**

### Notice type 100

A notice from gksdcp has `"Type":100`. The SDCP specification does not define this field for notices. The web page shows only notices of type 100: while it waits for the result of an action that it sent, and after it sent a start that was accepted, until the print runs or the start ends.

### The field `GKBridge`

See section 9.

## 13. Differences from the protocol

| Subject | Behavior of gksdcp |
|---|---|
| Upload failures | HTTP status 400, 403, 409, 500, 503 or 507 with the protocol failure body. The protocol text uses HTTP 200 with `success` set to `false`. |
| Upload sessions | A new `Uuid` replaces an upload that has had no chunk for 5 seconds. Some clients never send Cmd 255 when the user cancels. |
| Unreadable `RequestID` | No response, when the `RequestID` is absent, is not a string, or is longer than 256 bytes. |
| Unknown command | `Ack` 1. Never silence. |
| Cmd 259 | gksdcp always refuses the file that the printer has open, also in the state `complete`. |
| Print from USB | gksdcp copies the file to the internal storage first. |
| `StartLayer` | Only 0. |
| Print files | Only `.ctb`. The `SupportFileType` is `["CTB"]`. |
| Camera, time-lapse, video | Not available. |
| Notices | `Type` 100 is a project extension. |
| Z commands | 4096 to 4099 are a project extension. |
| Status | `GKBridge` is a project extension. |
| Clock | gksdcp does not set the clock of the printer. It learns the time from requests. |
| `MachineName` | `GKtwo`, the model of this printer. |

## 14. Options and build options

### Options of gksdcp

The script `/customer/gk/init.d/R97gksdcp` starts gksdcp with the options `-d /media/emmc -s /tmp/gkhook.sock -v /customer/gk/var`. All other options have their default value.

| Option | Default | Description |
|---|---|---|
| `-u <port>` | 3000 | The UDP port for discovery. |
| `-p <port>` | 3030 | The TCP port for WebSocket and upload. |
| `-w <port>` | 80 | The TCP port for the web page. 0 turns it off. |
| `-d <folder>` | `/media/emmc` | The folder for print files. |
| `-U <folder>` | `/media/usb` | The mount point of the USB flash drive. |
| `-M <file>` | `/proc/mounts` | The file that lists the mounts. |
| `-s <socket>` | `/tmp/gkhook.sock` | The hook socket. |
| `-v <folder>` | `/customer/gk/var` | The folder for the name and the print history. |
| `-i <interface>` | `wlan0` | The network interface for `MainboardID` and `MainboardIP`. |
| `-m <id>` | MAC address | A fixed `MainboardID`. It must have 16 hex digits. |
| `-S <seconds>` | 60 | The time after which an upload with no chunk ends (1 to 86400). |
| `-R <bytes>` | 16777216 | The free space that uploads and copies keep. |
| `-T` | Off | Allow files on a temporary storage. For tests only. |

### Build options

These make variables are build options for `make -C gksdcp arm`. `tools/build-all.sh` passes `WEBUI` from the environment.

| Variable | Default | Description |
|---|---|---|
| `WEBUI` | 1 | 1 builds the web page into gksdcp and serves it on port 80. 0 builds no web page, and gksdcp opens no port 80. |
| `MACHINE_NAME` | `GKtwo` | The value of `MachineName`. |
| `BRAND_NAME` | `Uniformation` | The value of `BrandName`. |
| `FW_VERSION` | `V1.3.4` | The first part of `FirmwareVersion`. |
| `BRIDGE_VERSION` | The content of `VERSION` | The last part of `FirmwareVersion`. |
| `XYZ_SIZE` | `228.1x128.3x245` | The value of `XYZsize`. |

Example, a build without the web page: `WEBUI=0 tools/build-all.sh`.

If gksdcp cannot open port 80, it writes a message to the log and continues without the web page.

## 15. Clients

The DragonFruit notes are tested on a printer: upload, print and stop work.

### DragonFruit

DragonFruit is the slicer of the Open Resin Alliance.

1. Duplicate the official GKtwo profile. A later update of the preset can reset a changed profile, but not a duplicate.
2. Open the duplicate. Open Profile settings, then Output.
3. Set Network Support to `SDCP 3.0.0`.
4. Enter the IP address of the printer.

DragonFruit exports encrypted CTB version 5 files. The touchscreen prints them, and a start through gksdcp prints them too. gkhook makes the same file checks as the touchscreen. The text `unsupported ctb version` comes from a format field that chitu sets when it opens the file. This field is not the version number in the CTB header. DragonFruit uploads in chunks and reads only the HTTP status of each chunk. It does not send Cmd 255 when you cancel an upload. Section 8 describes how gksdcp handles this.

### Other clients

A client that follows SDCP V3.0.0 can work with gksdcp. The commands in section 5 are the full list. For a test without a client, open a WebSocket connection with any WebSocket tool and send the request of section 4.
