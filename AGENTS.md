# Trophy Bootstrap Agent Guide

Initialization & Documentation Date: 2026-10-11. Version: 0.1.0. User's explicit instructions take precedence.

## Project Scope

This project is a resident trophy warm-up / bootstrap plugin running within PS Vita SceShell (`*main`). The host submits a Title ID / NPWR queue, and the plugin sequentially launches installed games. Each game's native executable initializes its own personal trophy DAT files (`TRPTITLE.DAT` and `TRPTRANS.DAT`). The plugin then safely terminates the claimed target process and verifies the presence of the DAT pair.

Only source code, control tooling, and tests belonging to this project are maintained here. The plugin does not install games, does not unlock trophies, does not modify DAT files, does not wipe trophy databases, and does not automatically delete generated records. It does not provide a standalone bubble, device key bindings, or boot-time auto-execution.

Always verify the working directory, Git root, and existing diffs before proceeding. Preserve existing modifications; do not commit, push, or release without explicit authorization. Proceed with operations that the user has already explicitly authorized.

## Device Operation Constraints

Reading and organizing documentation does not authorize connecting to devices, deploying, modifying configurations, or executing queues. When device operations are authorized, verify the active device address, mount points, sources, and targets; do not rely on historical environments.

Before deployment, inspect the actual `tai` configuration file on the device, maintain accompanying backups, and review candidate diffs. Add `ur0:tai/trophy_bootstrap.suprx` under the existing `*main` section; do not add duplicates if already present. Never delete other plugin entries or replace the entire tai directory. Loading the plugin requires a device reboot, executed strictly within the authorized scope.

The plugin defaults to an idle standby state; reboots do not automatically resume unfinished batches. On permission, identity, storage, logging, or process termination errors, halt immediately and preserve state. Resumption must use the current session and cannot inherit stale PID ownership. Never rewrite immutable queues of active batches or forge journals.

The existence of a DAT pair only proves that two non-empty files were observed; it does not substitute for plaintext structure verification, zero-progress checks, or trophy qualification validation. Batch completion must be inspected item-by-item; exit codes or `error=0` cannot substitute for per-item acceptance verification.

## Source Code Responsibilities

| File | Responsibility |
| --- | --- |
| [src/bootstrap.c](src/bootstrap.c), [include/bootstrap.h](include/bootstrap.h) | Queue parsing, state machine, capacity checking, process ownership, pause/resume, and result codes |
| [src/plugin.c](src/plugin.c) | SceShell resident thread, AppMgr process control, directory queries, control files, and diagnostics log |
| [src/app_list.c](src/app_list.c), [include/app_list.h](include/app_list.h) | Progressive running application list queries, rejecting truncated results and contract mismatches |
| [src/app_status.c](src/app_status.c), [include/app_status.h](include/app_status.h) | Classify startup contexts, running games, and known system background services |
| [src/sha256.c](src/sha256.c), [include/sha256.h](include/sha256.h) | SHA-256 digest calculations for queues and state journals |
| [tools/control.py](tools/control.py) | Host-side queue preparation, control command publication, and status inspection; no FTP |
| [tools/build.sh](tools/build.sh), [tools/verify_plugin.py](tools/verify_plugin.py) | Pinned VitaSDK container build and SELF envelope static validation |

The original game executable invokes its own trophy initialization. The plugin does not create trophy contexts on behalf of the game, does not bypass communication signatures, and does not modify or replace eboots. Each queue entry provides only a Title ID and NPWR; waiting, stopping, and result verification logic are shared across all entries.

## State Machine

```text
PREFLIGHT → LAUNCH → WAIT_PROCESS → RUN_WINDOW
→ STOP → WAIT_EXIT → CHECK_PAIR → COOLDOWN → Next Item
```

| Phase | Conditions & Limits |
| --- | --- |
| PREFLIGHT / LAUNCH | Skip if DAT pair already exists; wait if external applications are active; verify installed resources and real-time capacity; flush LAUNCH journal to disk before process invocation |
| WAIT_PROCESS | AppMgr status has non-zero `started`, `application_type=1`, name/PID/App ID match Shell list; same PID remains stable for 1 second; launch wait timeout is 30 seconds |
| RUN_WINDOW | Retain claimed PID ownership; stop early if pair is stable for 500 ms and running time reaches 5 seconds, otherwise run until queue limit (10s or 30s); PID changes never rebind |
| STOP | Re-verify target PID and App ID, flush STOP journal to disk, then terminate the claimed target process |
| WAIT_EXIT | Up to 10 seconds to confirm process exit; unknown or changing PIDs cannot be treated as exited |
| CHECK_PAIR | Wait 1.5 seconds, verify both DAT files exist and are non-empty, record item result |
| COOLDOWN | Wait 1.5 seconds cooldown, then advance to next item |
| RECOVER_WAIT | Do not inherit stale PID ownership; target must be absent; observe for 3 seconds, check if pair has emerged, then skip or proceed |

`process=0` denotes absent; `1` denotes claimable running game; `2` denotes present but starting (cannot be treated as absent or claimed). Negative errors must never be interpreted as "process absent". Actual execution latency is subject to Shell scheduling; logs do not record granular per-phase microsecond timestamps, so exact generation durations should not be inferred.

## Capacity and External Applications

Maximum 100 queue items, 100 personal registered directories. Capacity checks count actual personal directories, reserving multiple slots for games where multiple NPWRs are visible across `ux0:app`, `ux0:patch`, and `ux0:rePatch`. Directory query failures are not treated as empty directories; SFO or resource conflicts are not arbitrarily resolved. Concurrency races with other registration components remain unverified.

The running application list query starts at capacity 1, expanding through 2 / 4 / 8 / 16 / 32 when full; higher-capacity query failures never fall back to partial lists. Saturated maximum capacity or disagreement between return value and buffer contents trigger errors.

The plugin treats `NPXS10063` and `NPXS10079` as background services only when full name, PID, App ID verification passes and `application_type=0`; it neither waits for nor terminates them. All other system applications and VitaShell block execution. During PREFLIGHT, ordinary foreground occupancy results in waiting; during RUN, encountering external applications causes the plugin to stop its owned target, verify results, and pause the batch. Startup confirmation prompts may cause WAIT_PROCESS to time out; existing DAT files do not imply the game reached the main menu or achieved trophy eligibility.

## Queue and Control Protocol

Queue files must not exceed 8191 bytes, use LF line endings, and reject NUL bytes, CR characters, duplicate Title IDs, or duplicate NPWRs. Only two window configurations are accepted; Title IDs must be 9 alphanumeric characters not starting with `NPXS`. The following block illustrates the format:

```text
TROPHY_BOOTSTRAP_QUEUE_V1
batch=0123456789abcdef0123456789abcdef
user=00
window_ms=5000,30000
PCSE00001 NPWR00001_00
```

The alternative window setting is `window_ms=5000,10000`. New batches must use a distinct 32-hex `batch` identifier. Execution and recovery strictly use the immutable copy at `jobs/<batch>/queue.txt`, never the mutable root `queue.txt`.

```text
TROPHY_BOOTSTRAP_COMMAND_V1
<current_32_hex_session> <START|RESUME|CANCEL> <queue_sha256>
```

| Command | Purpose |
| --- | --- |
| START | Start a new batch; rejected if an active or unfinished batch exists |
| RESUME | Resume an unfinished batch matching saved queue; clears stale ownership and pause errors, verifies target absence |
| CANCEL | Record cancellation, attempt to terminate the current target under ownership checks, preserve generated files |

`control.py` inspects `ready.json` and `status.json`; RESUME and CANCEL also require reading the local `jobs/<batch>/queue.txt`. Downloading only `ready.json`/`status.json` to the PC is insufficient to construct a recovery command. Unconsumed commands or client-pending files refuse overwriting. If FTP lacks atomic rename-overwrite, retain existing files and switch via verified candidates; never delete active batch files to perform replacements.

## Persistence and Results

The device control directory is `ux0:data/trophy_bootstrap/`.

| File | Purpose |
| --- | --- |
| ready.json | Current boot session and plugin version; reboots generate a new session |
| status.json | Observed phase, active flag, needs_explicit_resume, index/count, error, operation, waiting_for, storage_error |
| state-0.bin / state-1.bin | Alternating state journals with sequence numbers and SHA-256 checksums; recovery selects the latest valid slot |
| jobs/<batch>/queue.txt | Immutable copy of the batch queue |
| jobs/<batch>/result.json | Per-item results published upon reaching terminal state; may not exist while paused |
| diagnostic.log | State transitions, process identities, and error locations; never logs credentials or DAT contents |

Each state transition writes to a temporary file, calls `fsync`, reads back for verification, and then replaces the target slot. If neither slot contains valid data, the plugin refuses to proceed. Journals follow the V1 layout. `status.error` prioritizes reporting negative control/runtime errors, falling back to persisted batch errors.

| Item Result | Meaning |
| --- | --- |
| existing_skipped | Non-empty pair already existed prior to launch; skipped |
| pair_generated_waiting_decrypted_export | Non-empty pair observed after game exit |
| pair_recovered_waiting_decrypted_export | Existing pair observed during pause recovery |
| missing_pair / incomplete_pair | Pair missing or incomplete; cannot be treated as success |
| launch_failed / cancelled / pending | Launch failed / cancelled / pending processing |

`BATCH_GENERATED` merely indicates all queue entries were processed; it may contain `missing_pair` items. `PAUSED` preserves the current progress and context; resumption may restart the current entry or advance to the next, depending on the index and results. `error=0` cannot substitute for individual success verification.

| Internal Error | Decimal | Meaning |
| --- | --- | --- |
| TB_ERROR | -29697 | Invalid input or contract error |
| TB_BUSY | -29698 | Target or external application occupancy |
| TB_CAPACITY | -29699 | Insufficient capacity |
| TB_IDENTITY | -29700 | Process, queue, or identity mismatch |
| TB_TIMEOUT | -29701 | Launch or exit observation timeout |
| TB_STORAGE | -29702 | Persistence / I/O failure |

System API errors may yield other negative values; refer to `operation` for diagnostics rather than mapping them to internal codes.

## Host Control Tooling

`tools/control.py` operates exclusively on local directories or mounts accessible to the PC; it does not connect to FTP, upload files, or install games.

1. Verify complete catalog, actual installed SFO / trophy resource mappings, existing personal pairs, and available capacity before preparing a new batch.
2. Place the generated queue into `ux0:data/trophy_bootstrap/queue.txt`, and issue `START` based on the active ready/status session.
3. Confirm the command was consumed and `active=true`; terminate FTP/USB connections, and completely close VitaShell from LiveArea so the plugin can process the queue.
4. After completion or pause, reconnect, read `status.json`, journals, immutable queue, and `result.json`, then verify actual DAT pairs. Inspect current position and target execution status before resuming.

Example commands (paths are placeholders; verify targets and side effects before execution):

```sh
python3 tools/control.py prepare --catalog /path/to/catalog.json --output /path/to/new_batch --limit 10 --user 00 --max-run-ms 30000
python3 tools/control.py command START --root /path/to/device/data/trophy_bootstrap
python3 tools/control.py status --root /path/to/device/data/trophy_bootstrap
python3 tools/control.py command RESUME --root /path/to/device/data/trophy_bootstrap
python3 tools/control.py command CANCEL --root /path/to/device/data/trophy_bootstrap
```

## Build and Verification

```sh
sh tools/test.sh
python3 tests/test_control.py
python3 tests/test_vita_format.py
sh tools/build.sh
python3 tools/verify_plugin.py /absolute/path/to/trophy_bootstrap.suprx
```

Select checks according to actual modifications. Documentation edits only verify links, content, and diffs without running device tasks or builds. Changes to queues, process ownership, capacity, cancellation, or recovery must run relevant synthetic checks; VitaSDK builds and SELF envelope checks cannot substitute for on-device validation.

`tools/build.sh` uses a pinned-digest VitaSDK Docker image: read-only source mount, disabled container networking, output directed to host temporary directories. Build artifacts belong in ignored `artifacts/`.

Pinned SDK libk `va_arg` exhibits ARM 64-bit word-alignment discrepancies; 64-bit integers in log formatting are split into two 32-bit hexadecimal values and must never be reintroduced as mixed `%llu` format strings. Format regressions are verified by `tests/test_vita_format.py`.

No build or on-device validation records exist prior to repository initialization. Completion reports must explicitly distinguish between static, synthetic, build, and hardware evidence, clearly detailing actual changes, completed checks, and unverified boundaries.

## Private Data

Actual accounts, credentials, DAT files, private queues, device configurations, and research logs must never enter version control. Working artifacts and verification evidence belong in the ignored `local/` directory, categorized by NPWR into generated, pre-existing, and missing. New device results must be backed by corresponding readback evidence. Never overwrite historical backups or clean device data during routine tasks.

## License and Provenance

This project is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE) for the full text. SPDX identifiers in source files must be preserved.

`src/sha256.c` and `include/sha256.h` are adapted from GPL-3.0-or-later baseline `63acbc8d8051ad1407fc88e8790edf4632f56618`, modified for resident plugin compilation against libk headers.

Pinned VitaSDK builds link against libk (PDCLib public domain C library) and GCC compiler support routines. This repository does not distribute the full toolchain; upstream licenses governing toolchain dependencies remain in effect.

AppMgr status contracts in `include/app_status.h` are independently represented based on VitaSDK FW 3.60 API layouts (<https://github.com/vitasdk/vita-headers/blob/master/include/psp2/appmgr.h>). The pinned SDK supplies import stubs but lacks typed declarations. No firmware binaries, game code, or cryptographic signing materials are reproduced.
