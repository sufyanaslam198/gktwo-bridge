# secrets

This folder holds the private files of your build. Git ignores every file in this folder except this file and the `.example` files.
Do not add your files to a commit or a fork. Any person with these files can get full control of your printer.

| File | Content | How you get it |
|---|---|---|
| `gktwo_ed25519` | The private SSH key | You create it. Refer to "Create the SSH key" in `docs/install.md`. |
| `gktwo_ed25519.pub` | The public SSH key. The build installs it on the printer. | `ssh-keygen` creates it with the private key. |
| `root-password` | The root password, on one line, 8 or more characters | You write it, or the build makes a random password. |
| `dropbear/` | The host keys of the SSH server on the printer | The build creates them one time and uses them again in each build. |

## Create the files

1. Create the SSH key:

   ```
   ssh-keygen -t ed25519 -f secrets/gktwo_ed25519 -N ""
   ```

2. Optional: choose the root password. Copy the example and replace the text with your password (one line, 8 or more characters):

   ```
   cp secrets/root-password.example secrets/root-password
   chmod 600 secrets/root-password
   ```

   Do not create or edit the file with PowerShell `>` or with Notepad. These tools can add a byte order mark or use UTF-16, and the password does not match. Use an editor that saves UTF-8 without a byte order mark, for example `nano`.
   If you do not create the file, the build makes a random password.
   The build stops if the file still contains the example text.

3. Do not create `dropbear/`. The build creates it.

The repository has no example keys. Each build must use its own keys.
