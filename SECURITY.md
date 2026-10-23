# Security policy

## Supported versions

| Item | Support |
|---|---|
| GKtwo Bridge | The latest commit on the `main` branch |
| Printer | Uniformation GKtwo |
| Stock firmware | V1.3.3 only |

GKtwo Bridge does not support any other printer or firmware version.
gkhook refuses to act on a version of chitu that it does not recognize.

## Security model

GKtwo Bridge is for a trusted local network.
It trades security for convenience. Read this section before you install it.

| Component | Access control |
|---|---|
| SDCP discovery (gksdcp, port 3000/UDP) | None. Any device on the network gets the name and the address of the printer. |
| SDCP WebSocket and upload (gksdcp, port 3030/TCP) | None. No user name, no password, no encryption. |
| Web page (gksdcp, port 80/TCP) | None. No user name, no password, no encryption. It is optional (build option `WEBUI`). |
| SSH server (Dropbear, port 22) | The root password from `secrets/root-password` or the SSH key that you supply when you build the image. |
| Hook socket (`/tmp/gkhook.sock`) | File mode `0600`. Only root on the printer can use it. |

What this means:

- Ports 3000/UDP, 3030/TCP and 80/TCP have no authentication. They must stay on a trusted local network.
- Anyone who can reach port 3030 can upload and delete print files, start and stop prints, and move the build plate. Port 80 serves only the web page, which uses port 3030.
- A print can start while no person is near the printer. The printer has parts that move and UV light.
- Each build has its own root password. The password is in `secrets/root-password`. Keep the folder `secrets/` private. A person who has the password or the SSH key and can reach port 22 gets full control of the printer.
- SDCP and the web page do not use encryption. Other people on the network can read the traffic.
- gksdcp refuses cross-site browser requests. It accepts a request that has an `Origin` header only when the origin is the web page of the printer. It also refuses to show the web page in a frame. This does not replace authentication. A program that sends no `Origin` header, such as a slicer or a script, is always accepted.
- To remove the web page, build the image with `WEBUI=0`. Ports 3000 and 3030 stay open.

## Protective measures

1. Connect the printer only to a network that you trust.
2. Never forward port 80, port 3000, port 3030 or port 22 to the internet.
3. Put the printer in a separate network (VLAN) or a guest network that guests cannot reach, if your router supports it.
4. Use a firewall rule to allow only your own computers to reach the printer.
5. Do not let untrusted devices join the Wi-Fi network of the printer.
6. Use the SSH key to connect. Do not share the key file `secrets/gktwo_ed25519`. Keep the Dropbear host keys in `secrets/dropbear/` and the file `secrets/root-password` private too. On Windows, the file permissions depend on the host folder, so keep the repository in a private user folder.
7. Remove `/customer/gk/lib/libgkhook.so` over SSH, and restart the printer, to disable gkhook. gkhook is the part that controls prints and movement.
8. Flash the stock firmware to remove all GKtwo Bridge components. Refer to `docs/install.md`.

To set your own root password, write it to `secrets/root-password` before the build.

## Report a vulnerability

Use the private vulnerability reporting of the repository on GitHub.

1. Open <https://github.com/sufyanaslam198/gktwo-bridge/security>.
2. Click **Report a vulnerability**.
3. Write the problem, the steps to repeat it and the version you tested.

Do not open a public issue for a vulnerability.
Do not publish details before the project has had time to fix the problem.
Expect a first answer within a few weeks. The project is maintained by volunteers.

Problems that the security model above already states are not vulnerabilities.
Example: no authentication on SDCP or on the web page.
