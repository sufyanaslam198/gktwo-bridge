# How to contribute to GKtwo Bridge

Thank you for your interest. This file explains how to propose a change and which checks a change must pass. Read [docs/development.md](docs/development.md) first. It explains the architecture and the build.

GKtwo Bridge moves a machine with a build plate that moves and UV light. Code review is strict for this reason.

## Propose a change

1. Open an issue that describes the problem or the feature. Do this before you write a large change.
2. State your printer model and the stock firmware version. The project supports only the Uniformation GKtwo with stock firmware V1.3.3.
3. Fork the repository and create a branch from `main`.
4. Make a small, focused change. One topic for each pull request.
5. Run the required checks (below).
6. Open a pull request. Describe what changed, why, and how you tested it. If you tested on a printer, say so and describe the test.

## Required checks before a pull request

Run each check and make sure that it passes. The commands are in [docs/development.md](docs/development.md).

| Check | Command |
|---|---|
| gkhook host tests | `tools/dock.sh gktwo-fw 'make -C gkhook host && make -C gkhook test'` |
| gksdcp tests | `tools/dock.sh gktwo-fw 'make -C gksdcp host && make -C gksdcp test && python3 -m pytest -q -p no:cacheprovider tests/gksdcp'` |
| Page script tests | The node container command in the development guide |
| ARM build and glibc check | `make -C gkhook arm`, `make -C gksdcp arm` and `tools/check-glibc.sh` in `gktwo-cc` |
| Full build | `tools/build-all.sh` must end with `VERIFY OK` and `E2E OK` |

A change that touches the web page, gksdcp or gkhook needs all checks. A change that only touches documents needs no build.

## Safety rules for code that moves the printer or calls chitu

These rules are mandatory. A pull request that breaks one is not accepted.

- Only the UI tick calls `printer_*`, `axis_*` and `ui_set_window_index`, and reads the `ui` struct and the printer pointer. The socket threads use only the queue and the status snapshot. The exceptions are the cheap file check on the connection thread (`print_file_get_format`, `print_file_open`, `print_file_close` and the resolution globals) and `print_file_open`, `print_file_check` and `print_file_close` on the checker thread.
- Every new call into `chitu` needs the full procedure in the development guide.
  The procedure has these parts: a findings entry, a fingerprint window in the code and in the test copy, a typed wrapper, guards and mock tests.
- Every motion command needs guards. Refuse it during a print, during a print start, during a firmware update and while the axis moves.
- Clamp every value that goes to `chitu`. Validate every value that comes from `chitu` memory.
- gkhook must never abort or block the UI loop. Every code path of the tick chains the stock callback.
- Do not patch `chitu` on disk.
- Test on hardware with an empty, dry vat and with your hands clear of the build plate. Do not test motion without guards.
- State in the pull request which hardware tests you ran.

## Code style

- C11. Compile with `-Wall -Wextra -Werror`. The build must have no warnings.
- LF line endings. UTF-8 text. The `.gitattributes` file enforces LF.
- Keep files small and each file to one task.
- Write a comment where the reason for the code is not obvious. Comment the reason, not the syntax. Give the evidence (a findings section or an address) for every `chitu` address.
- Keep the code inside the glibc 2.28 and size limits.
- Keep the new text in the documents short and exact. Use the terms that the existing documents use.

## Commit messages

Use a conventional prefix, a short imperative summary and, if needed, a body that explains why. Read `git log` for examples.

| Prefix | Use |
|---|---|
| `feat(scope):` | A new feature |
| `fix(scope):` | A bug fix |
| `docs:` | Documents only |
| `test(scope):` | Tests only |
| `build:` | `Dockerfile`, workflows and build scripts |
| `chore(release):` | A release commit (see "Releases" in the development guide) |

Add each change that a user can see to the section `[Unreleased]` in `CHANGELOG.md`.

Example: `fix(gkhook): refuse z_stop during firmware update`. Use the component as the scope: `gkhook`, `gksdcp`, `fw`, `zjog`.

## License

GKtwo Bridge uses the GPL-3.0 license (see `LICENSE`). When you submit a contribution, you agree to license it under GPL-3.0. Make sure that you have the right to do this.

## Never commit

- The vendor firmware: `ChituUpgrade.bin`, or any file that you extracted from it. The project does not distribute vendor firmware.
- Your files in `secrets/`: SSH keys, `root-password` and Dropbear host keys. Only `secrets/README.md` and the `.example` files are committed.
- Build output: `out/`, `gkhook/build/`, `gksdcp/build/`, `gksdcp/build-noweb/`.
- Real IP addresses of your network. Use `<printer-ip>` in documents.
