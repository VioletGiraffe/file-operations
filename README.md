# file-operations

File operations for Qt applications: a copy / move / permanent-delete engine, its progress and decision dialogs,
directory scanning and folder comparison.

## Capabilities

**Copy, move, delete** (`fileoperations/`)

- Copy, move and permanent delete of files and whole folder trees, each run as one background job with progress
  events, pause, cancellation (also in the middle of a large file) and a final summary.
- Move renames natively where it can, whole folders included. Across volumes it copies, then removes each source
  entry only after its copy is in place.
- Copies are staged: data goes to a hidden temporary file beside the destination, which becomes the destination by an
  atomic rename. An interrupted copy never leaves a half-written file under the real name.
- Timestamps and permissions are preserved; folders the operation creates keep their source folder's times.
- Collisions and failures arrive as decision requests the caller answers: replace, merge folders, rename, skip, retry,
  make writable, cancel. Replace, merge, skip and make writable can apply to all remaining matching cases.
- Progress in bytes for transfers and in items for delete, with speed and time remaining. Totals become exact once the
  source trees are scanned.
- Links are entries of their own: delete and same-volume move act on the link, copy brings over what it points to,
  link cycles end, and nothing reached through a directory link is ever deleted.
- Handles awkward input: unusual names, paths beyond Windows' `MAX_PATH`, FAT32 and exFAT volumes, case-only
  renames. A destination inside its own source is refused, judged by filesystem identity, not spelling.
- Requests are built from raw path text (`makeTransferRequest`, `makePermanentDeleteRequest`), validated once.
- Inline rename of a single entry, and validation of a name a user proposes for a new or renamed entry.
- Not included: moving to the Trash / Recycle Bin (delete is permanent), and clipboard integration.

**Dialogs** (`fileoperationsui/`)

- `CFileOperationDialog` runs one job and stays hidden when the operation finishes quickly. Otherwise it shows
  progress, speed and time remaining, with pause, cancel, and a background mode.
- Decisions are asked through `CFileOperationPrompt`, which offers only what the engine allows for that case.
- The completion summary stays on screen only when something needs the user's attention.

**Scanning and comparison** (`directoryscan/`, `foldercomparison/`)

- `scanDirectory()`: recursive, abortable traversal that follows directory links and detects link cycles.
- `compareFolders()`: compares two folder trees, matching names exactly or across letter case and Unicode
  normalization, and reading file contents only where type and size cannot already tell files apart. Reports progress
  and can be aborted.
- `compareFileContents()`: byte comparison of two files.

## Layout

| Folder | Contents | Library |
|--------|----------|---------|
| `fileoperations/` | The engine. QtCore only. | `fileoperations` |
| `fileoperationsui/` | `CFileOperationDialog`, `CFileOperationPrompt`, user-facing wording. QtWidgets. | `fileoperationsui` |
| `directoryscan/` | `scanDirectory()` and `QString`-path access to thin_io. | `fileoperations` |
| `foldercomparison/` | `compareFolders()`, `compareFileContents()`. Needs `directoryscan`. | `fileoperations` |
| `libraries/` | The library projects, and the `.pri` files a host includes to link them. | |
| `test-utils/` | Test fixtures: folder generation, random data, link creation. | `test_utils` |
| `tests/` | The suites; `tests.pro` builds them with their dependencies. | |

A host builds the projects in `libraries/` and includes `libraries/fileoperations.pri` or
`libraries/fileoperationsui.pri`, which add the library, its dependencies and their include paths.

## Dependencies

`cpputils`, `cpp-template-utils`, `qtutils` and `thin_io`, checked out beside this repository; `config.pri` holds the
paths. A host repository's `../global.pri` supplies the build settings when present.

## Tests

`scripts/run_tests.bat` and `scripts/run_tests.sh` build `tests/tests.pro` and run every suite; see
[doc/testing.md](doc/testing.md).

## Documentation

- [doc/architecture.md](doc/architecture.md): the engine's design, its threading and path model, the dialogs, scanning
  and comparison.
- [doc/testing.md](doc/testing.md): the suites, how to run them, and the coverage that needs special volumes.
- [doc/coding-style.md](doc/coding-style.md): authoring rules, including the mandatory review after concurrency changes.
