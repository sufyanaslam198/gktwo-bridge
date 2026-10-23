# GKtwo Bridge

GKtwo Bridge adds remote access to the Uniformation GKtwo resin printer.
It is a set of firmware add-ons. You build one firmware image from your own copy of the stock firmware, then flash it from a USB flash drive.

The image gives the printer an SSH server, a network interface that speaks SDCP (Smart Device Control Protocol), a web page and remote movement of the build plate.
You can then upload print files, start and stop prints, and move the build plate. Use a slicer, a browser or a command line on your local network.
The touchscreen and chitu work as before.
The project does not patch chitu on disk.

GKtwo Bridge does not include any vendor firmware.
You must supply the stock firmware yourself.

## Safety and security notice

**WARNING: Do not use the web page when other people are near the printer. The build plate moves with force and the printer has UV light. Parts that move can cause injury.**

**WARNING: Do not connect the printer to the internet through GKtwo Bridge. SDCP and the web page have no authentication. Anyone on your network can start a print or move the build plate.**

- SDCP (ports 3000 and 3030) and the web page (port 80) have no user name and no password.
- Each build sets its own root password. The password is in `secrets/root-password`. Keep the folder `secrets/` private. SSH also accepts the key that you supply when you build.
- Use GKtwo Bridge only on a trusted local network.
- Never forward port 80, port 3000, port 3030 or port 22 to the internet.
- Use an empty, dry vat for the first test of every function.
- You modify the firmware of your printer at your own risk. The authors give no warranty. A fault in a modified image can damage the printer. The project can restore the stock firmware only if you keep a copy of it.
- Stop a print or a build plate movement from the touchscreen if the web page or the slicer does not respond.

Refer to `SECURITY.md` for the full security model.

## Features

- SSH server (Dropbear) with root access, and `scp -O` file copy.
- SDCP V3.0.0 server (gksdcp): discovery on UDP port 3000, and WebSocket and file upload on TCP port 3030. It works with DragonFruit. Refer to `docs/sdcp.md`.
- Web page on port 80 (an SDCP client, optional at build time):
  - Status: state, file name, layer, progress, elapsed time and remaining time.
  - Files: list, upload (also large `.ctb` files), replace and delete in the internal storage.
  - Print control: start, pause, resume and stop a print.
  - Move Z panel: home, up and down by 1 mm, 10 mm or 100 mm, move to the top, and stop. The panel keeps the build plate between 0 mm and the top position.
- The image does not change chitu on disk. If the gkhook library is missing, the printer starts chitu without it.
- One-command build in Docker, with automatic checks of the image.

The printer prints only `.ctb` files in this build.

## Known limitations

- SDCP and the web page have no authentication.
- Only the DragonFruit slicer has been tested with a printer: upload, print and stop work.
- Only `.ctb` files are tested and supported.

## Supported hardware and firmware

| Item | Support |
|---|---|
| Printer | Uniformation GKtwo only |
| Stock firmware | V1.3.3 only |
| Other versions | Refused. gkhook shows `wrong chitu version` and does not act. |

The project is tested on one GKtwo with stock firmware V1.3.3.
The image reports itself as `GKtwoV1.3.4`, so that the printer accepts it as an upgrade.

## Not affiliated

GKtwo Bridge is an independent project.
It is not affiliated with, approved by or supported by Uniformation or ChiTu Systems.
All trademarks belong to their owners.

## Quick start

1. Get the stock firmware V1.3.3 (`ChituUpgrade.bin`). Keep a safe copy of it.
2. Install Docker and start it.
3. Create an empty folder and copy the stock `ChituUpgrade.bin` to it.
4. In that folder, run the build (PowerShell, Linux or macOS):

   ```
   docker run --rm --privileged -v "${PWD}:/data" ghcr.io/sufyanaslam198/gktwo-bridge
   ```

   Wait for `VERIFY OK` and `E2E OK`. The build writes the image to `out/` and your SSH key and root password to `secrets/`.
5. Copy `out/ChituUpgrade.bin` to a FAT32 USB flash drive and flash the printer.
6. Open `http://<printer-ip>/` in a browser, or add the printer to your slicer (refer to `docs/sdcp.md`).

The install guide gives the full procedure, the Git Bash command and the build from the source: `docs/install.md`.

## Releases

Versions follow [Semantic Versioning](https://semver.org/). The changes of each version are in `CHANGELOG.md`.
Each release on GitHub has the build image `ghcr.io/sufyanaslam198/gktwo-bridge:<version>` and an archive of the ARM programs (Dropbear, gksdcp, gkhook) with SHA-256 values.
Printers report the version in SDCP as `FirmwareVersion`, for example `V1.3.4-bridge-0.1.0`.

## Files that stay on your computer

`.gitignore` keeps these files out of Git. Do not add them to a commit or a fork.

| Path | Content | Reason |
|---|---|---|
| `secrets/` (except `README.md` and the `.example` files) | The root password, your SSH key and the Dropbear host keys | A person with these files can get full control of your printer. |
| `ChituUpgrade.bin` | The stock firmware | The vendor owns the firmware. The project does not distribute it. |
| `UltraFirmwareToolkit/` | A local clone of the firmware toolkit, if you make one | The build clones its own copy in Docker. |
| `out/`, `gkhook/build/`, `gksdcp/build/`, `gksdcp/build-noweb/`, `deps/*/src/` | Build outputs and downloaded sources | The build makes them again. |
| `archive/`, `*.bundle` | Local backups of Git history | They are for your use only. |

The image in `out/ChituUpgrade.bin` contains your root password hash, your SSH public key and the private Dropbear host keys. Do not share your built image.

## Documentation

| Document | Content |
|---|---|
| `docs/install.md` | Build, flash, first boot checks and recovery |
| `docs/user-guide.md` | The web page, print control, Move Z, fault isolation and SSH |
| `docs/sdcp.md` | The SDCP reference: ports, commands, messages, upload rules and client notes |
| `docs/gkhook-protocol.md` | The hook socket protocol between gksdcp and gkhook |
| `docs/development.md` | Source layout, build, tests, hot-swap and releases for developers |
| `CHANGELOG.md` | The changes in each version |
| `CONTRIBUTING.md` | How to contribute |
| `SECURITY.md` | Security model, protective measures and how to report a vulnerability |
| `docs/reverse-engineering/` | Notes on how chitu works |

## Credits

- [Dropbear](https://matt.ucc.asn.au/dropbear/dropbear.html) 2022.83 by Matt Johnston: the SSH server. The build compiles it from source. Dropbear uses an MIT-style licence.
- [jsmn](https://github.com/zserge/jsmn) by Serge Zaitsev: the JSON tokenizer in gksdcp. jsmn uses the MIT licence.
- [weashadow/UltraFirmwareToolkit](https://github.com/weashadow/UltraFirmwareToolkit): the tool that unpacks and repacks the firmware image. The build uses commit `6286345080449c6b59015a897aafd75eb8479542`. See the project's repository for its licence.

## License

GKtwo Bridge is free software under the GNU General Public License, version 3 (GPL-3.0).
Refer to the `LICENSE` file.
