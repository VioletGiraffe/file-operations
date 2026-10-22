# file-operations project guidance

## Project essentials

- A Qt library of file operations: a copy / move / permanent-delete engine, its progress and decision dialogs, directory scanning and folder comparison. Hosts include it as a Git submodule; file-commander is the first.
- C++23, Qt 6, qmake. Two static libraries, built by the projects in `libraries/`: `fileoperations` (QtCore: the engine, scanning, comparison, path grammar) and `fileoperationsui` (QtWidgets: the dialogs). A host builds them and includes `libraries/fileoperations.pri` or `libraries/fileoperationsui.pri`, which link a library with its dependencies.
- Parts and dependency direction: `fileoperationsui/` (QtWidgets) -> `fileoperations/` (QtCore only); `foldercomparison/` -> `directoryscan/`; all three QtCore parts -> `paths/`. All native filesystem access goes through `thin_io`.
- Dependencies (`cpputils`, `cpp-template-utils`, `qtutils`, `thin_io`) are checked out beside this repository; `config.pri` holds the paths, and a host's `../global.pri` supplies the compiler settings when present.

## Documentation routing

- Writing code: `doc/coding-style.md`, including the mandatory review pass after any concurrency change.
- The engine, its threading and path model, the dialogs, scanning, comparison: `doc/architecture.md`.
- Tests, scripts, CI and the environment variables that enable volume-dependent coverage: `doc/testing.md`.
- The POSIX non-UTF-8 filename limitation and its planned fix: file-commander's `doc/planned-refactors/native-paths.md`.

## Verification

- `scripts/run_tests.bat` / `scripts/run_tests.sh` build and run the suites. Run the ones that compile what you changed.
- These suites build only this repository's libraries. A change to a public header or a manifest must also build in the host's full build, applications and plugins included, not only in the host's test tree.

## Invariants to preserve

- Module code never includes a host's headers; a dependency a host provides is passed in through the API.
- Executors are synchronous. `CFileOperationJob` is the only cross-thread boundary: pause, cancellation, decisions and the event queue.
- The UI chooses the `DestinationIntent` once, when building a request; nothing below reinterprets it from path spelling or filesystem state.
- `CEntryPath` comes only from `parseOperationPath()` or operations on valid paths. Its equality is exact spelling; same-entry decisions use filesystem identity (`checkSameEntry()`).
- Links are entries distinct from their targets: delete and same-filesystem move act on the link, copy materializes the target, and content reached through a directory link is never removed.
- Fault-injection hooks compile only with `FILE_OPERATIONS_TEST_HOOKS`, into the `fileoperations_testhooks` library variant the suites link; production builds see the no-op stubs.
