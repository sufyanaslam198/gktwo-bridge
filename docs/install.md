# Install guide

This guide shows how to build a GKtwo Bridge image and flash it to the printer.
An image is a `ChituUpgrade.bin` file that this project builds.
To flash is to install an image from a USB flash drive.

**WARNING: Do not use the web page or SSH on a printer with resin in the vat for the first tests. The build plate moves with force. Use an empty, dry vat.**

**CAUTION: Do not set the printer power switch to OFF when the printer installs an image. A power loss during the flash can damage the printer.**

## 1. Requirements

| Item | Requirement |
|---|---|
| Printer | Uniformation GKtwo with stock firmware V1.3.3 |
| Stock firmware | The vendor `ChituUpgrade.bin`, version V1.3.3 (you supply it) |
| Build computer | Windows, Linux or macOS on x86-64 (the build image is linux/amd64). The project is tested on Windows 11. |
| Software | Docker. Git only for a build from the source. |
| Disk space | Approximately 8 GB free for the build image and the build |
| Network | The build downloads tools and source code, so the computer needs internet access |
| USB flash drive | Formatted as FAT32 |
| Printer network | The printer is connected to your local network |

The project does not distribute vendor firmware.
Other firmware versions are not tested.
The build does not check the stock firmware version. Do the checks in section 2 before you build.
At run time, gkhook refuses a different chitu version (status `wrong chitu version`).

## 2. Get the stock firmware

1. Make sure that the printer runs V1.3.3. Read the version on the touchscreen, on the device information screen.
2. Get the stock `ChituUpgrade.bin` V1.3.3 from your printer vendor.
3. Make sure that the file is V1.3.3. Run one of these commands in the folder that has the file.

   PowerShell:

   ```
   (Select-String -Path ChituUpgrade.bin -Pattern '#upgrade_bin_version=\S+' -List).Matches.Value
   ```

   Linux, macOS or Git Bash:

   ```
   head -c 4096 ChituUpgrade.bin | tr -d '\0' | grep -a -m1 '^#upgrade_bin_version='
   ```

   The line near the top of the file holds the version. It must show V1.3.3.
4. Copy the file to a safe place, for example a different folder or a second USB flash drive.
5. Rename the copy `ChituUpgrade-STOCK-V1.3.3.bin`.
6. Keep this copy. You need it to restore the stock firmware.

The image that the build makes reports itself as `GKtwoV1.3.4`.

## 3. Install Docker

1. Install Docker:
   - Windows and macOS: Docker Desktop (<https://www.docker.com/products/docker-desktop/>). On Windows, use the WSL 2 back end.
   - Linux: Docker Engine (<https://docs.docker.com/engine/install/>). Do the post-installation steps so that your user can run Docker without `sudo`.
2. Start Docker. Docker must be running for each build.
3. Make sure that Docker can start a privileged container. The end-to-end test of the build needs this:

   ```
   docker run --rm --privileged hello-world
   ```

   The output must contain `Hello from Docker!`. Rootless Docker and Podman can refuse `--privileged`.

Docker Desktop is free for personal use and for small companies. Larger companies need a paid subscription.

The build image is `ghcr.io/sufyanaslam198/gktwo-bridge` (linux/amd64). It contains the build tools and the GKtwo Bridge programs, and no vendor firmware. Each release has a tag with its version, for example `:0.1.0`. The tag `latest` is the newest release. To build the image from the source instead, refer to section 6.2.

## 4. Prepare the build folder

The build folder holds your input and output files. Docker mounts it at `/data` in the container.

1. Create an empty folder, for example `gktwo-build`, in a private user folder.
2. Copy the stock `ChituUpgrade.bin` V1.3.3 to the folder.

After the build, the folder holds these items:

| Path | Content |
|---|---|
| `ChituUpgrade.bin` | The stock firmware (your copy) |
| `out/ChituUpgrade.bin` | The GKtwo Bridge image to flash |
| `out/ChituUpgrade.bin.sha256` | The SHA-256 value of the image |
| `secrets/` | The SSH key, the root password and the Dropbear host keys |

Keep the folder for the next build. The build uses the files in `secrets/` again, so the password and the host key of the printer do not change between builds.
Do not share `secrets/` or `out/ChituUpgrade.bin`. The image contains your root password hash, your SSH public key and the private host keys.

## 5. SSH key and root password

### SSH key

The build copies the public key to the file `authorized_keys` of the root user.
You use the private key to connect without a password.

If `secrets/gktwo_ed25519` does not exist, the build creates a key pair with no passphrase.
To use your own key, create it before the first build. In the build folder:

```
mkdir secrets
ssh-keygen -t ed25519 -f secrets/gktwo_ed25519 -N ""
```

To add a passphrase, run `ssh-keygen -p -f secrets/gktwo_ed25519`.

The build also creates the Dropbear host keys one time in `secrets/dropbear/`.
The host keys must stay private, like the SSH key.

### Root password

The build sets a root password. The password is for SSH login. You can use the SSH key instead.

1. To let the build choose the password, do nothing. If the file `secrets/root-password` does not exist, the build makes a random password and writes it to the file.
2. To choose the password, write it to `secrets/root-password` before the build. Use one line with 8 or more characters. Replace `your-password` with your password.

   PowerShell, in the build folder:

   ```
   New-Item -ItemType Directory -Force secrets
   [IO.File]::WriteAllText("$PWD\secrets\root-password", "your-password`n")
   ```

   Linux, macOS or Git Bash, in the build folder:

   ```
   mkdir -p secrets
   printf '%s\n' 'your-password' > secrets/root-password
   chmod 600 secrets/root-password
   ```

   Do not create or edit the file with PowerShell `>` or with Notepad. These tools can add a byte order mark or use UTF-16, and the password does not match. Use the commands above, or an editor that saves UTF-8 without a byte order mark.

3. To read the password after the build, open the file `secrets/root-password`.

Each build uses a new salt for the hash.
The build stops if the file is empty, has fewer than 8 characters or still contains the text of `secrets/root-password.example` from the repository.
On Windows, the file permissions depend on the host folder. Keep the build folder in a private user folder.
A changed password takes effect only after you build the image again and flash it.

## 6. Build the image

### 6.1 With the published image

1. Make sure that Docker is running.
2. Go to the build folder and run the build.

   PowerShell:

   ```
   docker run --rm --privileged -v "${PWD}:/data" ghcr.io/sufyanaslam198/gktwo-bridge
   ```

   Linux or macOS:

   ```
   docker run --rm --privileged -v "$PWD:/data" ghcr.io/sufyanaslam198/gktwo-bridge
   ```

   Git Bash:

   ```
   MSYS_NO_PATHCONV=1 docker run --rm --privileged -v "$(pwd -W):/data" ghcr.io/sufyanaslam198/gktwo-bridge
   ```

   To build an image with no web page, add `-e WEBUI=0` before the image name.
   To use a fixed release, add its tag to the image name, for example `ghcr.io/sufyanaslam198/gktwo-bridge:0.1.0`.
   To get the newest release again, run `docker pull ghcr.io/sufyanaslam198/gktwo-bridge` first.

3. Wait until the command stops. The first run downloads the image (approximately 2 GB) and takes several minutes.
4. Make sure that the output contains `VERIFY OK` and `E2E OK`.
5. Make sure that the command prints a SHA-256 value and that these files exist:
   - `out/ChituUpgrade.bin`
   - `out/ChituUpgrade.bin.sha256`

If the command stops with an error, do not flash the image. Refer to section 9.

The build does these steps:

1. It creates the SSH key, the root password and the host keys in `secrets/` if they do not exist.
2. It unpacks the stock firmware and adds the GKtwo Bridge files. The image contains Dropbear, gksdcp and gkhook already built. The web page is part of gksdcp, unless you set `WEBUI=0`.
3. It sets the root password from `secrets/root-password` and the SSH key.
4. It packs the new image and sets the version to `GKtwoV1.3.4`.
5. It compares the new image with the stock firmware (`VERIFY OK`).
6. It starts the new system in an emulator and tests it (`E2E OK`).

The build downloads the firmware toolkit (`weashadow/UltraFirmwareToolkit`, a pinned commit) each time, so the computer needs internet access.

### 6.2 From the source

Use this procedure to build a version that has no published image, or a change of your own.

1. Install Git. On Windows, Git for Windows (<https://git-scm.com/download/win>) includes Git Bash.
2. Clone the repository and build the image:

   ```
   git clone https://github.com/sufyanaslam198/gktwo-bridge.git
   cd gktwo-bridge
   docker build -t gktwo-bridge .
   ```

   The build of the image downloads packages and can take several minutes.

3. Do section 6.1 with the image name `gktwo-bridge` instead of `ghcr.io/sufyanaslam198/gktwo-bridge`.

For development, `tools/build-all.sh` builds from the repository folder instead of a build folder. Refer to `docs/development.md`.

## 7. Flash the printer

**CAUTION: Do not set the printer power switch to OFF during the flash. The printer can become unusable.**

1. Format a USB flash drive as FAT32.
2. Copy `out/ChituUpgrade.bin` to the top level of the drive. Do not put it in a folder.
3. Make sure that the file name is exactly `ChituUpgrade.bin`.
4. Make sure that no other `ChituUpgrade*` file is on the drive.
5. Set the printer power switch to OFF.
6. Insert the USB flash drive in the printer.
7. Set the printer power switch to ON.
8. Wait until the flash ends and the touchscreen shows the normal screen.
9. Remove the USB flash drive.

If the touchscreen does not change for more than 10 minutes, do not set the power switch to OFF.
Read the text on the touchscreen and refer to section 9.

## 8. First boot checks

Wait at least 60 seconds after the normal touchscreen screen shows.
The SSH server and gksdcp (SDCP and the web page) start approximately 35 seconds after the boot scripts run.

### 8.1 Touchscreen

1. Make sure that the touchscreen responds.
2. Make sure that the printer connects to your network again.
3. Make sure that the internal file list shows your files.

Find the IP address of the printer in the touchscreen network settings or in your router.
The text below uses `<printer-ip>` for this address.

### 8.2 Web page

1. Open `http://<printer-ip>/` in a browser on the same network.
2. Make sure that the web page loads and shows the file list.
3. Make sure that the status shows `Idle`.

Refer to `docs/user-guide.md` for the use of the web page.

If the image has no web page (`WEBUI=0`), port 80 is closed. Use a slicer or another SDCP client instead.

### 8.3 SDCP

1. Make sure that your computer can reach the printer's IP address. The printer can be on a different subnet if a router connects the two.
2. Add the printer to your slicer by its IP address. Refer to the section "Clients" in `docs/sdcp.md`. Automatic discovery finds the printer only on the same subnet, so use the IP address in other cases.
3. Make sure that the client shows the status `idle`.

SDCP uses UDP port 3000 and TCP port 3030. A firewall between the client and the printer must allow them.

### 8.4 SSH

1. Connect with the key:

   ```
   ssh -i secrets/gktwo_ed25519 root@<printer-ip>
   ```

   Or connect with the password in `secrets/root-password`:

   ```
   ssh root@<printer-ip>
   ```

2. Answer `yes` to the question about the host key. This question shows the first time only.
3. To copy a file, use `scp -O`. The SSH server of the printer has no SFTP.

   ```
   scp -O somefile.txt root@<printer-ip>:/tmp/
   ```

## 9. Fault isolation

| Problem | Possible cause | Action |
|---|---|---|
| The build stops with `no ChituUpgrade.bin in the mounted folder` | The stock file is not in the build folder, or the command ran in a different folder | Copy the stock file to the build folder (section 4). Run the command in that folder. |
| The build stops with `no folder mounted at /data` | The command has no `-v` option | Use the command in section 6.1. |
| The build stops with `the end-to-end test needs a privileged container` | The command has no `--privileged` option, or Docker refuses it | Use the command in section 6.1. Do step 3 of section 3. |
| The build stops with `secrets/gktwo_ed25519.pub exists without secrets/gktwo_ed25519` | The private key is missing | Put the private key back. Or remove `secrets/gktwo_ed25519.pub` to get a new key pair; the printer accepts the new key after the next flash. |
| The build stops at the unpack step | The stock file is damaged | Get the stock file again. |
| The status line of the web page shows `hook: wrong chitu version` | The stock file was not V1.3.3. The build does not check the version. | Flash the stock firmware V1.3.3. Do the checks in section 2. Build again with the correct file. |
| The output ends with `VERIFY FAILED` | The image differs from the stock firmware in a way that the checks do not allow | Read the lines that start with `FAIL`. Do not flash the image. Make sure that the stock file is V1.3.3 and build again. |
| The output ends with `E2E FAILED` | A test of the new system failed in the emulator | Read the lines that start with `FAIL`. Do not flash the image. Make sure that Docker can start `--privileged` containers. |
| A development build stops with a `FAIL` line from `check-glibc` (`needs newer than GLIBC_2.28` or `not an ARM ELF`) | A program was not built in the `gktwo-cc` image | Build the `gktwo-cc` image again (`docs/development.md`). Run the build again. |
| `docker: command not found` | Docker is not installed, or the terminal was open before you installed Docker | Install Docker (section 3). Open a new terminal. |
| The build stops with `Cannot connect to the Docker daemon` or a similar Docker error | Docker is not running, or your user cannot use it | Start Docker Desktop and wait until the Docker Engine is running. On Linux, do the post-installation steps for Docker. Make sure that `docker ps` works. |
| `Unable to find image` with `not found` or `denied` | The image name or the tag is wrong | Copy the command from section 6.1. Make sure that the release tag exists. |
| The build stops with `No space left on device` | The disk is full | Free disk space. Refer to section 11 to remove old Docker data. |
| The build stops during a download (the image, the clone of the toolkit, or the Dropbear source in a build from the source) | No internet access, a proxy, or the server is not available | Make sure that the computer has internet access. Run the build again. |
| The printer ignores the USB flash drive | Wrong file name, a folder, or a drive that is not FAT32 | Do the procedure in section 7 again. |
| The web page does not load | The printer started less than 60 seconds ago, or the IP address is wrong | Wait one minute. Make sure of the IP address. |
| The web page shows `Printer app not reachable` | gksdcp is not active | Refer to the fault isolation table in `docs/user-guide.md`. |
| SSH does not answer | The printer started less than 60 seconds ago | Wait two minutes. Make sure that the touchscreen works. |

If the touchscreen does not work after the flash, flash the stock firmware (section 10).

## 10. Recovery

### 10.1 Restore the stock firmware

This procedure removes all GKtwo Bridge components.

1. Copy the stock `ChituUpgrade.bin` V1.3.3 to the top level of a FAT32 USB flash drive.
2. Rename the file to `ChituUpgrade.bin` if you use the copy with `-STOCK` in the name.
3. Do the procedure in section 7 with this file.
4. After 60 seconds, make sure that SSH does not connect.

To install GKtwo Bridge again, flash `out/ChituUpgrade.bin`.

### 10.2 Disable gkhook only

Use this procedure if the web page or gkhook causes a problem and you want the stock behavior.
The SSH server stays active.

1. Connect to the printer with SSH.
2. Remove the gkhook library:

   ```
   rm /customer/gk/lib/libgkhook.so
   ```

3. Restart the printer.

The file `/customer/app0.sh` starts chitu without gkhook if the library is missing.
To get gkhook again, flash the image again.

After the restart, without gkhook:

- The status line of the web page shows `hook: hook not running`, and `GKBridge.HookOk` is `false`.
- Print control and Move Z do not work.
- The file list works. An upload with a new file name works.
- gksdcp cannot read the state of the printer. For this reason, it refuses to delete or replace a file and to print from a USB flash drive. The message is `printer state unknown`.

## 11. Remove the build tools

The build image uses several GB of disk space. Remove it if you do not build again.

1. Remove the build image:

   ```
   docker image rm ghcr.io/sufyanaslam198/gktwo-bridge
   ```

   Add the tag if you used one, for example `ghcr.io/sufyanaslam198/gktwo-bridge:0.1.0`.

2. If you built the image from the source (section 6.2), remove it and the Docker build cache:

   ```
   docker image rm gktwo-bridge
   docker builder prune
   ```

   Answer `y` to the question. This command also removes the build cache of other Docker projects on the computer.

To build again, do section 6 again.
Keep the folder `secrets/` if you build again. A new SSH host key makes SSH clients show a warning.
