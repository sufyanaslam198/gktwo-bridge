# Changelog

All notable changes to GKtwo Bridge are in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project uses [Semantic Versioning](https://semver.org/). Before 1.0.0, a minor version can contain changes that are not compatible.

## [Unreleased]

### Fixed

- A start now closes a finished print that is still on the touchscreen. Before, gkhook refused the start with `dismiss the finished print on the printer first` until the user closed the print-complete dialog on the touchscreen. A print that ended with an error must still be closed on the touchscreen.

## [0.1.1] - 2026-10-23

### Fixed

- The ARM programs are now reproducible: gksdcp and gkhook link their source files in sorted order, so the image build and `tools/build-all.sh` make byte-identical files. Before, the link order depended on the file system, and the two builds gave different `libgkhook.so` and `gksdcp` files from the same source.

### Changed

- Workflows use `actions/checkout@v7`, `docker/setup-buildx-action@v4`, `docker/build-push-action@v7`, `docker/login-action@v4` and `docker/metadata-action@v6` (Node 24). The page script tests run on Node 24.

## [0.1.0] - 2026-10-23

The first release. Supports the Uniformation GKtwo with stock firmware V1.3.3 only.

### Added

- SSH server: Dropbear 2022.83 with root access by key or password, and `scp -O`.
- gksdcp: an SDCP V3.0.0 server. Discovery on UDP port 3000, WebSocket and file upload on TCP port 3030. Tested with DragonFruit: upload, print and stop.
- Web page on port 80, served by gksdcp (build option `WEBUI`, on by default): status, file list, upload, replace and delete, print control, and the Move Z panel.
- gkhook: a library that the printer loads into chitu at start (`LD_PRELOAD`). It gives gksdcp print control and Z movement, and does not change chitu on disk. It refuses to act on a chitu version other than V1.3.3.
- Firmware pipeline: builds the image from the stock `ChituUpgrade.bin`, compares it with the stock image (`VERIFY OK`) and tests it in an emulator (`E2E OK`).
- The `gktwo-bridge` image on GHCR: one `docker run` command builds, verifies and tests an image from a folder with the stock firmware.
- A root password and SSH host keys for each build, kept in `secrets/`.

[Unreleased]: https://github.com/sufyanaslam198/gktwo-bridge/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/sufyanaslam198/gktwo-bridge/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/sufyanaslam198/gktwo-bridge/releases/tag/v0.1.0
