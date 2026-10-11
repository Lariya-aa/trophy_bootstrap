# Trophy Bootstrap

[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-PS%20Vita%20(FW%203.60%2B)-blueviolet.svg)](https://vitasdk.org)

**Trophy Bootstrap** is a resident background plugin running within PlayStation Vita's `SceShell` (`*main`). It automates the safe, batch initialization ("warm-up") of personal trophy database files (`TRPTITLE.DAT` and `TRPTRANS.DAT`) for installed games without tampering with system databases or unlocking trophies.

For comprehensive operational guidelines, state machine specifications, error code tables, and recovery protocols, refer to [AGENTS.md](AGENTS.md).

---

## Overview

When games are installed or backed up on a PS Vita, personal trophy DAT structures (`ur0:user/<user>/trophy/data/<NPWR>/`) are only initialized when each game is booted for the first time by its native executable.

Trophy Bootstrap coordinates this process safely and automatically:
1. The host prepares a queue file containing pairs of Title IDs and NPWR IDs.
2. The resident plugin sequentially launches each title via `SceAppMgr`.
3. The game's native binary initializes its own personal trophy context.
4. The plugin monitors process state, verifies DAT pair creation, terminates the target process under strict ownership checks, and advances to the next queue item.

---

## Core Principles & Safety

- **No Trophy Modification**: Does not unlock trophies, does not alter trophy progress, does not bypass communication signatures, and never modifies game eboots.
- **Strict Process Ownership**: The plugin verifies target process validity via AppMgr status records (`app_id`, `pid`, `title`, `application_type`) and Shell running application lists. Transient startup contexts cannot be claimed or terminated.
- **Dual-Slot Atomic Journaling**: State transitions are protected against unexpected power loss or crashes using alternating slots (`state-0.bin` and `state-1.bin`) with sequence numbers and SHA-256 checksums.
- **Controlled Concurrency**: Whitelists verified system background services (`NPXS10063`, `NPXS10079`) while waiting for or yielding to user foreground apps and VitaShell.
- **Capacity Safe**: Enforces the 100-title personal trophy capacity limit, accounting for multi-NPWR titles visible across `ux0:app`, `ux0:patch`, and `ux0:rePatch`.

---

## Architecture

```
trophy_bootstrap/
├── include/
│   ├── app_list.h         # Running application stepped query interface
│   ├── app_status.h       # AppMgr status ABI contract definitions
│   ├── bootstrap.h        # Core state machine, queue types, and error codes
│   └── sha256.h           # SHA-256 hash interface
├── src/
│   ├── app_list.c         # Capacity-expanding running application queries
│   ├── app_status.c       # Process classification & background service detection
│   ├── bootstrap.c        # State machine implementation & queue parser
│   ├── plugin.c           # SceShell resident thread & I/O controller
│   └── sha256.c           # SHA-256 implementation
├── tools/
│   ├── build.sh           # Reproducible containerized VitaSDK build script
│   ├── control.py         # Host-side queue preparation & command CLI
│   ├── test.sh            # Local host unit test runner with ASan/UBSan
│   └── verify_plugin.py   # SELF envelope inspection & firmware verification
├── tests/
│   ├── test_app_list.c    # Application list contract tests
│   ├── test_app_status.c  # Status classification unit tests
│   ├── test_control.py    # Host control tool tests
│   ├── test_scheduler.c   # State machine synthetic simulation tests
│   └── test_vita_format.py# ARM format string ABI regression tests
├── AGENTS.md              # Complete technical agent & architecture guide
└── README.md              # Project overview
```

---

## State Machine Lifecycle

```text
PREFLIGHT → LAUNCH → WAIT_PROCESS → RUN_WINDOW
→ STOP → WAIT_EXIT → CHECK_PAIR → COOLDOWN → Next Item
```

- **PREFLIGHT**: Checks if DAT pair already exists; verifies installed resources and system capacity.
- **LAUNCH**: Flushes state journal to disk before requesting application launch via URI.
- **WAIT_PROCESS**: Waits up to 30 seconds for the target process to become active and stable for 1 second.
- **RUN_WINDOW**: Maintains process execution until the DAT pair emerges (min 5 seconds) or window timeout.
- **STOP**: Re-validates PID and App ID ownership, flushes journal, and terminates the target process.
- **WAIT_EXIT**: Confirms process termination within 10 seconds.
- **CHECK_PAIR**: Verifies `TRPTITLE.DAT` and `TRPTRANS.DAT` exist and are non-empty.
- **COOLDOWN**: 1.5-second cooldown delay before proceeding to the next entry.

---

## Host Control CLI

The host tool [tools/control.py](tools/control.py) prepares batches, publishes control files, and inspects status over local directories or mounts (e.g. FTP mounts or USB drives):

```sh
# Prepare a new queue from an existing catalog
python3 tools/control.py prepare --catalog /path/to/catalog.json --output /path/to/batch --limit 10 --user 00 --max-run-ms 10000

# Issue START command for an idle session
python3 tools/control.py command START --root /path/to/ux0/data/trophy_bootstrap

# Check current execution status
python3 tools/control.py status --root /path/to/ux0/data/trophy_bootstrap

# Resume an interrupted batch
python3 tools/control.py command RESUME --root /path/to/ux0/data/trophy_bootstrap

# Cancel an active batch safely
python3 tools/control.py command CANCEL --root /path/to/ux0/data/trophy_bootstrap
```

---

## Building & Verification

### Local Synthetic Tests
Run host C tests (compiled with clang/gcc `-fsanitize=address,undefined`) and Python test suites:
```sh
sh tools/test.sh
python3 tests/test_control.py
```

### Containerized VitaSDK Build
Build `trophy_bootstrap.suprx` using the pinned VitaSDK Docker image:
```sh
sh tools/build.sh
python3 tools/verify_plugin.py /path/to/trophy_bootstrap.suprx
```

---

## Documentation

For full operational constraints, device deployment rules, journal structures, error codes, and recovery state handling, please read the complete guide in [AGENTS.md](AGENTS.md).

---

## License

This project is licensed under the **GNU General Public License v3.0 or later** (GPL-3.0-or-later). See the [LICENSE](LICENSE) file for details.
