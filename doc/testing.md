# Tests

The project files are authoritative for the current test set; this is the map. Every suite uses Catch2.

## Layout

`tests/tests.pro` builds the suites together with their dependencies, which are checked out beside this repository.
Each suite compiles the module sources it tests from their `.pri` manifests. A host can build the same suites from its
own test tree.

| Executable | Component |
|------------|-----------|
| `fileoperations_test` | The engine: copy, move, delete, staged copy, destination and name resolution, cross-volume, hostile names |
| `foldercomparison_test` | File content and folder comparison |
| `fileoperationsui_test` | The dialog and the decision prompt |

`test-utils/` holds the shared fixtures: temporary folder generation, random data, link creation, Qt/Catch2 glue. Every
suite links `catch2_runner` (`cpp-template-utils/3rdparty/catch2/`), Catch2's implementation compiled once.

## Running them

`scripts/run_tests.bat` and `scripts/run_tests.sh` build the suites and run every executable, then report which failed.
The Windows logic lives in `run_tests.ps1`, which the batch file starts; it takes named parameters, while the shell
script takes the same options as positional keywords, in the order listed here:

| | Windows | macOS, Linux |
|---|---|---|
| Debug configuration | `-Configuration debug` | `debug` |
| Build without running, run without building | `-BuildOnly`, `-NoBuild` | `build`, `nobuild` |
| Include the slow cases | `-All` | `all` |
| One suite, plus arguments for it | `-Suite fileoperations_test "[moveexecutor]"` | `fileoperations_test "[moveexecutor]"` |

Every suite and its default filter live in one table at the top of each script. Each suite runs with Catch2's
`--warn NoTests`, so a filter that matches nothing fails instead of reporting success.

The scripts take the Qt kit from `QT_ROOT_DIR`, or from a git-ignored `local-env.ps1`/`local-env.sh` beside them;
the shell script also falls back to a `qmake` already on PATH.

Without `all`, the tests that work on generated data are excluded: `[executor]` and `[deleteexecutor]` in
`fileoperations_test`, which build trees of thousands of files, and `[CFileComparator]` in `foldercomparison_test`,
which writes about a thousand files of up to 3 MB each. Every other case in both suites still runs.

To run on FAT32 or exFAT, point `TEMP` and `TMP` at the volume: every test works in a `QTemporaryDir`. Tests that need
what those filesystems lack skip themselves there: links everywhere, and on POSIX also FIFOs, sockets and permission bits.

## Kinds

| Kind | Where |
|------|-------|
| Integration tests on the real filesystem, in generated temporary trees | engine, comparison |
| Randomized runs, reproducible by `--std-seed <seed>` | `fileoperations_test`, `foldercomparison_test` |
| Fault injection through `operationtesthooks`, compiled in only with `FILE_OPERATIONS_TEST_HOOKS` | engine, dialogs |
| Headless widget tests, runnable under `QT_QPA_PLATFORM=offscreen` | `fileoperationsui_test` |

Coverage that needs a filesystem capability is skipped where it is absent, and required where its variable is set:

| Variable | Meaning |
|----------|---------|
| `FILE_OPERATIONS_TEST_SECOND_VOLUME` | A writable directory on another volume: real cross-device transfers |
| `FILE_OPERATIONS_TEST_CASE_SENSITIVE_VOLUME` | A writable directory on a case-sensitive filesystem |
| `FILE_OPERATIONS_TEST_SYMLINKS` | Symbolic link creation must succeed |

## CI

CI clones the dependencies beside the checkout, builds with `-BuildOnly`/`build`, and runs with `-NoBuild`/`nobuild`.
On every platform it provides a second volume, symbolic links, and exFAT and FAT32 volumes for a further full run;
macOS and Linux also provide the case-sensitive volume. The generated-data cases run with fresh seeds, those of
`fileoperations_test` 20 times.
