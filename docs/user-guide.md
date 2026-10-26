# User guide

This guide describes the GKtwo Bridge web page.
It also describes SSH access and fault isolation.
The web page runs on the printer (gksdcp, port 80). Open it at `http://<printer-ip>/`.
The web page is a client of SDCP, the protocol that [`sdcp.md`](sdcp.md) describes. You can also control the printer from a slicer. Refer to the section "Clients" in [`sdcp.md`](sdcp.md).
The image builder can remove the web page (build option `WEBUI=0`). Then port 80 is closed, and SDCP clients still work.

## 1. Safety

**WARNING: Make sure that there are no hands, tools or objects near the build plate before you start a print or a movement. The build plate moves with force and can cause injury.**

**WARNING: Do not start a print on the touchscreen while the build plate moves from a web command. The printer does not prevent this condition. A collision can occur.**

**WARNING: Do not connect the printer to the internet. The web page and SDCP have no authentication. Anyone on your network can control the printer.**

**CAUTION: Use an empty, dry vat for the first test of any function. A fault can damage the vat film and the screen.**

**CAUTION: Do not use the web page when another person uses the touchscreen. The printer can do commands from the two controls at the same time.**

If the web page does not respond, stop the print or the movement from the touchscreen.
If the build plate continues to move, set the printer power switch to OFF.

## 2. Layout

The web page has these sections from top to bottom.

| Section | Content |
|---|---|
| Banner | A red line `Printer app not reachable` when the web page cannot connect to gksdcp. |
| Status | State, file, layer, elapsed time, remaining time, a message line, a progress bar, and the buttons **Pause**, **Resume** and **Stop**. |
| Message line | The result of your last action, or the reason for a refusal. |
| Move Z | The position of the build plate and the movement buttons. |
| Upload | A file chooser, the free space and a **Cancel** button. |
| Files | The files in the internal storage and on a USB flash drive, each with **Print** and **Delete** buttons. |

The web page keeps one WebSocket connection to gksdcp. gksdcp sends each change of the status to the web page. If the connection closes, the web page connects again after 3 seconds. The banner shows after two failed attempts.

## 3. Status

### 3.1 States

The line State shows one of these texts.

| State text | Description |
|---|---|
| `Idle` | The printer does not print, and no print has ended since gksdcp started. |
| `Starting` | The printer accepted a start but does not print yet. It checks the file, it heats, it copies the file from a USB flash drive, or the build plate goes to the home position. This can take minutes for a large file. |
| `Homing` | The build plate goes to the home position at the start of a print that was started on the touchscreen. |
| `Printing` | The printer prints. |
| `Pausing` | The printer pauses at the end of the layer. |
| `Paused` | A print is paused. |
| `Stopping` | The printer stops the print. |
| `Stopped` | The last print was stopped, or it ended with an error. The state stays until the next print starts. |
| `Complete` | The last print is finished. The state stays until the next print starts, also after you dismiss the finished print on the touchscreen. |
| `Receiving file` | An upload is in progress. |

The fields layer, elapsed and remaining show a dash when the value is not known. In the state `Starting`, elapsed shows `0:00:00`.
The remaining time shows a dash until the printer has an estimate.
The progress bar shows the layer divided by the number of layers.

### 3.2 Status line messages

The message line under the status shows the reason when a start ends. It is bold red text with no frame. The line is empty when no message exists.

| Message | Description |
|---|---|
| `start cancelled` | You stopped the start from the web page. |
| `print stopped on the printer` | The print window closed after it opened, for example after a stop on the touchscreen. |
| `start failed` | The print window did not open, or the print ended before it began. |
| `file check failed` | The file check found a problem in the file. |
| `file check timed out` | The file check did not finish in time. |
| `printer busy` | The printer was not ready to start the print. |
| `hook: wrong chitu version` | chitu is not V1.3.3. gkhook does not act. |
| `hook: hook not running` | gksdcp cannot talk to gkhook. The printer starts, or gkhook is not active. |
| `hook: hook starting` | gkhook started a moment ago. Wait a few seconds. |
| `hook: bad status reply` | gkhook sent a status that gksdcp cannot read. |
| `hook: hook error` | gkhook reported an error without a text. |

The web page shows `Z control unavailable` and disables the buttons **Print**, **Pause**, **Resume**, **Stop** and **Delete** while gksdcp cannot talk to gkhook.

## 4. Files

The web page shows the files in the internal storage `/media/emmc` on the tab **Internal**. When a USB flash drive is in the printer, the tab **USB** shows. Each folder has an **Open** button. A row `..` goes up one folder.
The page shows only folders and `.ctb` files.

### 4.1 Which actions the states allow

| Action | Allowed states |
|---|---|
| Upload a new file name | All states except `Homing`, `Printing`, `Pausing`, `Paused` and `Stopping`, and except `Starting` during a USB copy |
| Replace a file | `Idle`, `Complete`, `Stopped` |
| Delete a file | `Idle`, `Complete`, `Stopped` |
| Start a print | `Idle`, `Complete`, `Stopped` |

gksdcp refuses to delete the file that the printer has open, also in the state `Complete`. Dismiss the finished print on the touchscreen first.

### 4.2 Upload a file

1. Click the file chooser in the Upload section.
2. Select a file with the extension `.ctb`.
3. Wait until the message line shows `Uploaded <name>`.
4. Make sure that the file shows in the list.

The web page sends a large file in parts of 1 MB. The upload keeps 16 MB of the free space in reserve.
Click **Cancel** to stop an upload. The message line shows `Upload cancelled`.

File names must follow these rules:

- Use letters, digits, space and the characters `. _ ( ) + -`.
- Use a maximum of 128 characters.
- Do not start the name with a dot.
- End the name with `.ctb`.

If the name is not valid, the message line shows `Invalid file name. Use letters, digits, space . _ ( ) + - only (max 128 chars, not starting with '.'), ending in .ctb. Rename the file and try again.` Rename the file and upload it again.


If an upload fails, the message line shows the reason that gksdcp gives. Refer to the table in section 4.5.

### 4.3 Replace a file

1. Select a file with the same name as a file in the list.
2. Click **OK** in the question `Replace existing file <name>?`.

### 4.4 Delete a file

1. Click **Delete** next to the file.
2. Click **OK** in the question `Delete <name>?`.

The message line shows `Deleted <name>`, or `Could not delete <name>`.

### 4.5 Messages for files

| Message | Cause | Action |
|---|---|---|
| `file is empty` | The file has no content. | Select a different file. |
| `Not enough free space: <name> is <n> MB but only <n> MB can be used (16 MB stay in reserve). Delete a file first.` | The storage has too little free space. | Delete a file. |
| `printer busy` | A print, a start or a USB copy is in progress, so the printer refuses the upload. | Wait until the state is `Idle`, `Complete` or `Stopped`. |
| `printer state unknown` | gksdcp cannot read the state of the printer, so it does not replace a file. | Wait until the status shows the state. Then try again. |
| `Could not delete <name>` | The printer refused the deletion: a print or a start is in progress, the printer has the file open, or gksdcp cannot read the state of the printer. | Wait until the state is `Idle`, `Complete` or `Stopped`. Dismiss a finished print on the touchscreen. Then try again. |
| `another upload is running` | A different upload is in progress. | Wait, or cancel it. |
| `storage not mounted` | The printer storage is not mounted yet. | Wait one minute. Reload the web page. |
| `not enough space` | The storage has too little free space. | Delete a file. |
| `upload failed (code <n>)` or `upload failed (HTTP <n>)` | The printer refused the upload and gave no text. | Upload the file again. |
| `Upload failed: network error` | The connection to the printer failed. | Upload the file again. |

## 5. Print control

### 5.1 Start a print

**WARNING: Make sure that the vat and the build plate are ready before you start. The build plate moves down when the print starts.**

1. Make sure that the state is `Idle`, `Complete` or `Stopped`.
2. If the touchscreen shows a print error, close it there first. A finished print closes on the touchscreen when the new print starts.
3. Click **Print** next to the file.
4. Make sure that the state changes to `Starting`, and then to `Printing`.

The message line shows `Start <name> sent`. The state shows `Starting` until the printer prints.

If the printer heats first, the touchscreen shows the heat dialog.
You can not close this dialog from the web page.

For a file on a USB flash drive, the printer copies the file to the internal storage first. The state is `Starting` during the copy. Click **Stop** to cancel the copy. You can remove the USB flash drive after the copy.

If the copy fails, the state changes to `Stopped` and the message line shows the reason.

### 5.2 Pause and resume

1. Click **Pause**. The pause starts at the end of the current layer.
2. Make sure that the state changes to `Paused`.
3. Click **Resume** to continue the print.

**Pause** is available only in the state `Printing`.

### 5.3 Stop a print

1. Click **Stop**.
2. Click **OK** in the question `Stop the print?`.
3. Make sure that the state changes.

**Stop** is also available in the state `Starting`:

| Condition | Result |
|---|---|
| The file check runs, or the print window is not open | The start is cancelled. The message is `start cancelled`. The printer does not move. |
| The printer homes | The printer stops. The start ends with `start cancelled`. |
| The printer heats | The printer refuses with `printer is heating - cancel it on the touchscreen`. Cancel the heat on the touchscreen. |

### 5.4 Messages for print control

After a command, the message line shows `<Action> sent` when the printer accepts it. If the printer refuses it, the message line shows the reason from this table. If no reason arrives within 1 second, it shows `<Action> refused`.

| Message | Cause | Action |
|---|---|---|
| `printer busy` | The state does not allow the action. | Wait until the state is `Idle`, `Complete` or `Stopped`. |
| `dismiss the finished print on the printer first` | A failed print is open on the touchscreen, or a dialog is open over a finished print. | Close it on the touchscreen. |
| `Z is moving` | The build plate moves. | Wait, or click **Stop** in the Move Z panel. |
| `not printing` | You use pause or stop when no print runs. | None. |
| `printer is homing - pause when printing starts` | You use pause while the printer homes. | Wait until the state is `Printing`, then pause. |
| `not paused` | You use resume when the print is not paused. | None. |
| `printer is heating - cancel it on the touchscreen` | You use stop while the printer heats. | Cancel the heat on the touchscreen. |
| `only .ctb files can be printed` | The file is not a `.ctb` file. | Select a `.ctb` file. |
| `file is not in the files folder` | The path of the file is not in the files folder. | Select a file from the list. |
| `bad file path` | The path of the file is not valid. | Rename the file. |
| `move the file to the top folder to print it` | The file is in a folder of the internal storage. | Move the file to the top folder. |
| `USB drive not mounted` | The file is on a USB flash drive that the printer does not see. | Insert the drive again. |
| `file not found` | The file does not exist. | Reload the web page. |
| `not a ctb file` | The file does not have the `.ctb` format. | Select a valid file. |
| `cannot read file` | The printer cannot read the file. | Upload the file again. |
| `cannot open file` | chitu cannot open the file. | Upload the file again. |
| `unsupported file format` | The format of the file is not supported. | Select a valid file. |
| `unsupported ctb version` | The version of the `.ctb` file is not supported. | Prepare the file again for the printer. |
| `file resolution does not match printer` | The file is for a different screen resolution. | Prepare the file for the GKtwo. |
| `file check failed` | The check found a problem in the file. | Upload the file again, or prepare it again. |
| `cannot start file check` | The printer has too little resources. | Try again. |
| `not enough space on internal storage` | The copy of a USB file does not fit. | Delete a file on the internal storage. |
| `cannot write to internal storage`, `USB drive removed during copy`, `cannot read from USB drive`, `USB file changed during copy` | The copy of a USB file failed. | Try again. |
| `printer not responding` | The printer did not do the command in time. | Try again. |
| `bad reply from printer` | gkhook sent a reply that gksdcp cannot read. | Try again. If the message shows again, refer to section 7. |
| `printer refused` | gkhook refused the command and gave no reason. | Try again. If the message shows again, refer to section 7. |

## 6. Move the build plate (Move Z)

Use the Move Z panel to move the build plate up and down.
You do not touch the touchscreen.

The panel can do these operations:

- Move the build plate to the home position.
- Move the build plate up or down by 1 mm, 10 mm or 100 mm.
- Move the build plate to the top position.
- Stop a movement.

The panel uses the same movement functions as the touchscreen.
The Move Z panel is below the Status section.

**WARNING: Make sure that there are no hands, tools or objects near the build plate before you move it. The build plate moves with force and can cause injury.**

**WARNING: Do not start a print on the touchscreen while the build plate moves from a web command. A collision can occur.**

**CAUTION: Make sure that there is no cured resin or object in the vat before you move the build plate to the home position. The build plate moves down to the bottom limit. An object in the vat can cause damage to the vat film and to the screen.**

### 6.1 Position line

| Text on the panel | Description |
|---|---|
| `Z: 12.35 mm` | The build plate is at 12.35 mm above the home position. |
| `Z: not homed` | The printer does not know the position. Move the build plate to the home position first. |
| `Z: moving…` | The build plate moves now. |
| `Z control unavailable` | The panel cannot control the build plate. Refer to section 6.5. |

The text next to the position line gives a hint:

| Hint | Description |
|---|---|
| `Home Z first` | Click **Home** before you use the other movement buttons. |
| `Printer busy` | A print runs or starts, or an upload runs. The movement buttons are not available. |

### 6.2 Buttons

| Button | Result |
|---|---|
| **Up 1**, **Up 10**, **Up 100** | Moves the build plate up by 1 mm, 10 mm or 100 mm. |
| **Down 1**, **Down 10**, **Down 100** | Moves the build plate down by 1 mm, 10 mm or 100 mm. |
| **TOP** | Moves the build plate to the top position. |
| **Home** | Moves the build plate down to the bottom limit switch. This sets the home position (0 mm). |
| **Stop** | Stops the movement. |

A button is gray when it is not available.

### 6.3 Limits

- The printer must know the position before it moves the build plate up, down or to the top. After the printer starts, click **Home** one time.
- The build plate does not go below the home position (0 mm).
- The build plate does not go above the top position. On the tested printer, the top position is 250 mm.
- If a step is too large, the build plate stops at the limit. Example: the build plate is at 3 mm and you click **Down 10**. The build plate moves down 3 mm and stops at 0 mm.
- The printer does one movement at a time. When the build plate moves, only **Stop** is available.
- The panel does not operate when a print is in progress.
- On the tested printer, the build plate moves at approximately 2.5 mm per second. A 100 mm step takes approximately 40 seconds. A movement from the home position to the top position takes approximately 100 seconds. The printer's machine parameters set the speed, so these are measured values for the tested printer.

### 6.4 Procedures

#### Move the build plate to the home position

1. Make sure that there is no cured resin or object in the vat.
2. Make sure that no print is in progress.
3. Click **Home**.
4. Read the question. Click **OK**.
5. Make sure that the position line shows `Z: moving…`.
6. Make sure that the position line shows `Z: 0.00 mm` when the movement stops.

#### Move the build plate up or down

1. Make sure that the position line shows a position in mm. If it shows `Z: not homed`, move to the home position first.
2. Click the button for the direction and the distance. Example: click **Up 10** to move the build plate up 10 mm.
3. Make sure that the position line shows the new position when the movement stops.

#### Move the build plate to the top position

Use this procedure before you remove a print or clean the build plate.

1. Make sure that the position line shows a position in mm. If it shows `Z: not homed`, move to the home position first.
2. Make sure that there are no objects above the build plate.
3. Click **TOP**.
4. Read the question. Click **OK**.
5. Make sure that the position line shows `Z: 250.00 mm` when the movement stops.

#### Stop a movement

1. Click **Stop**.
2. Make sure that the build plate stops. The build plate can continue to move for a maximum of 2 seconds.
3. Make sure that the position line shows a position in mm.

If the build plate does not stop, push the stop control on the touchscreen.
If the build plate continues to move, set the printer power switch to OFF.

### 6.5 Messages for Move Z

The message line shows `Z <action> sent` when the printer accepts the command. If the printer refuses it, the message line shows one of these reasons.

| Message | Cause | Action |
|---|---|---|
| `home Z first` | The printer does not know the position. | Move the build plate to the home position. |
| `Z is moving` | A movement is in progress. | Let the movement stop. Then click the button again. |
| `printer busy` | A print is in progress, a print starts, an upload runs, or a print is on the screen. | Let the print or the upload stop. On the touchscreen, close the print screen. |
| `at top limit` | The build plate is at the top position. | Move the build plate down. |
| `at bottom limit` | The build plate is at the home position. | Move the build plate up. |
| `firmware update in progress` | The printer installs new software. | Do not use the panel until the update stops. |
| `Z travel limit unknown` | The printer has an incorrect value for the top position. | The build plate can only move down or home. Tell the person who maintains the printer. |
| `Z speed setting invalid` | The printer has an incorrect value for the movement speed. | Tell the person who maintains the printer. |
| `Z position unknown` | The printer cannot read the position. | Move the build plate to the home position. If the message shows again, tell the person who maintains the printer. |
| `bad request` | The step is not 1, 10 or 100 mm. | Use the buttons of the panel. |
| `printer not responding` | The printer did not do the command in 2 seconds. | Click the button again. If the message shows again, set the printer power switch to OFF and then to ON. |
| `Z control unavailable` | The panel cannot control the build plate. | Refer to the fault isolation table below. |

## 7. Fault isolation

| Problem | Possible cause | Action |
|---|---|---|
| The web page does not load. | The printer started less than 60 seconds ago, the IP address is wrong, or the image has no web page (`WEBUI=0`). | Wait one minute. Make sure of the IP address. |
| The banner shows `Printer app not reachable`. | gksdcp is not active, or the printer starts. | Wait for the main screen on the touchscreen. Open the web page again. |
| The status line shows `hook: wrong chitu version`. | chitu is not V1.3.3. | Flash the stock firmware V1.3.3. Refer to `docs/install.md`. |
| The status line shows `hook: hook not running` or `hook: hook starting`. | gkhook is not active, or it started a moment ago. | Wait one minute. If the message stays, read the log `/tmp/gkhook.log`. |
| Move Z shows `Z control unavailable`. | The printer has a different software version, or gkhook is not active. | Use the touchscreen to move the build plate. |
| All movement buttons are gray, or all except **Stop**. | A print is in progress, the build plate moves, or the web page has no connection to the printer. | Read the hint and the position line. |
| The **Print** button is gray. | The state is not `Idle`, `Complete` or `Stopped`, or gksdcp cannot talk to gkhook. | Read the state and the status line. |
| A print does not start from the state `Complete`. | The finished print is open on the touchscreen. | Close it on the touchscreen. |
| An upload fails. | The storage has too little space, or a print is in progress. | Read the message line. Delete a file, or wait until the print stops. |
| A slicer does not find the printer. | The slicer and the printer are on different networks, or a firewall blocks UDP port 3000. | Add the printer by its IP address. Refer to `docs/sdcp.md`. |
| The build plate moves in the wrong direction. | The printer settings are incorrect. | Click **Stop**. Do not use the panel. Tell the person who maintains the printer. |

To find the cause of a problem, connect with SSH and read the log `/tmp/gksdcp.log` or `/tmp/gkhook.log`.
The logs are in RAM. The printer removes them at restart.

## 8. SSH access

1. Connect with the key that you made at build time:

   ```
   ssh -i secrets/gktwo_ed25519 root@<printer-ip>
   ```

2. Or connect with the password in `secrets/root-password`:

   ```
   ssh root@<printer-ip>
   ```

3. To copy a file, use `scp -O`. The SSH server has no SFTP.

   ```
   scp -O somefile.txt root@<printer-ip>:/tmp/
   ```

Useful files on the printer:

| File | Content |
|---|---|
| `/tmp/gkhook.log` | The log of gkhook |
| `/tmp/gksdcp.log` | The log of gksdcp |
| `/customer/gk/lib/libgkhook.so` | The gkhook library |
| `/customer/gk/bin/gksdcp` | The SDCP server and the web server |
| `/customer/gk/var` | The printer name and the print history |
| `/media/emmc` | The print files |

The root password is in the file `secrets/root-password` on the computer that built the image. Keep this file private. A changed password takes effect only after a rebuild and a flash. Use the printer only on a trusted network. Refer to `SECURITY.md`.
