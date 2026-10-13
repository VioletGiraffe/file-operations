# file-operations

File operations for Qt applications: a copy / move / permanent-delete engine, its progress and decision dialogs,
directory scanning and folder comparison.

| Folder | Contents | Manifest |
|--------|----------|----------|
| `fileoperations/` | The engine. QtCore only. | `fileoperations.pri` |
| `fileoperationsui/` | `CFileOperationDialog`, `CFileOperationPrompt`, user-facing wording. QtWidgets. | `fileoperationsui.pri` |
| `directoryscan/` | `scanDirectory()` and `QString`-path access to thin_io. | `directoryscan.pri` |
| `foldercomparison/` | `compareFolders()`, `compareFileContents()`. Needs `directoryscan`. | `foldercomparison.pri` |
| `test-utils/` | Test fixtures: folder generation, random data, link creation. | `test-utils.pro` |
| `tests/` | The suites; `tests.pro` builds them with their dependencies. | |

A host project includes the manifests it needs and puts this repository's root on its include path.

## Dependencies

`cpputils`, `cpp-template-utils`, `qtutils` and `thin_io`, checked out beside this repository; `config.pri` holds the
paths. A host repository's `../global.pri` supplies the build settings when present.

## Tests

`scripts/run_tests.bat` and `scripts/run_tests.sh` build `tests/tests.pro` and run every suite. They take the Qt kit
from `QT_ROOT_DIR`, or from a git-ignored `local-env` file beside them.

| Executable | Component |
|------------|-----------|
| `fileoperations_test` | The engine: copy, move, delete, staged copy, destination and name resolution, cross-volume, hostile names |
| `foldercomparison_test` | File content and folder comparison |
| `fileoperationsui_test` | The dialog and the decision prompt |

## Engine (`fileoperations/`)

The module separates synchronous policy and filesystem mechanics from its threaded UI wrapper:

```text
CFileOperationJob
  `- CTransferExecutor or CDeleteExecutor
       |- COperationExecutionContext
       |- destination resolution: resolveFileDestination(), resolveDirectoryDestination() in destinationresolver.h
       |- source manifest scan: buildSourceTree() in sourcetreebuilder.h
       |- CStagedFileCopy
       `- CFileSystemMutator
```

`fileoperationtypes.h` defines requests, issues, decisions, progress, and summaries. `centrypath.h` owns operation
paths; `newnamecheck.h` validates user-proposed names. The UI chooses `DestinationIntent` once when building a
request; lower layers do not reinterpret it.

Important boundaries:

1. Executors are synchronous. `CFileOperationJob` is the cross-thread owner and the sole pause, cancel, decision,
   and event-queue boundary.
2. Move attempts native rename first. Only a classified cross-device result selects staged copy and owned-source
   cleanup.
3. Staged copy publishes by rename after contents and required metadata are ready. Cancellation is honored before
   publication; publication and committed source cleanup are not interruptible.
4. Delete and same-filesystem move act on link entries. Copy materializes link targets, detects active traversal
   cycles, and never removes content merely borrowed through a directory link.
5. Manifest scanning builds the complete operation-specific tree before aggregate totals become available. Fixed
   depth and node limits bound hostile or cyclic inputs.
6. Native error classification is conservative. Narrow a platform error only when the operation's local context
   proves the meaning; unknown errors remain ordinary I/O failures.
7. Namespace decisions are path-bound. External processes can replace a source or destination between inspection,
   authorization, and mutation; eliminating that limitation would require stable identities plus handle-bound or
   conditional native operations, not another path recheck.

Exclusive publication degrades to a check followed by rename when the filesystem lacks an exclusive-rename
primitive. This preserves compatibility but admits a narrow external race. Publication also relies on normal
filesystem namespace persistence rather than promising power-loss transactional durability.

A case-only rename goes through a temporary sibling name where the filesystem cannot do it in one step: where the
new spelling collides with the entry itself (POSIX), or where the rename reports success and keeps the old spelling
(FAT32 and exFAT on Windows). That path is two renames with a rollback, not one atomic operation.

Known defects:

- Same-entry detection (`checkSameEntry`) compares a volume serial number plus a file ID. Two volumes cloned from one
  image share both, so an entry and its counterpart on the other volume compare as one entry. A copy or move between
  them reports such an item as already satisfied and transfers nothing.
- On FAT32, a copied file can keep the creation time of a file recently deleted under the same name. Windows
  filesystem tunnelling applies it when the staged copy is renamed to its final name, and on FAT32 it also matches a
  name deleted in another, since removed directory.
- On macOS, a file copied to FAT32 gets the current modification time: the driver restamps a written file after the
  source's time was applied. Only flushing every file before applying it would prevent that.

Inline rename is a separate synchronous command that reuses name, inspection, identity, and native rename
primitives. Test-only fault injection lives in `operationtesthooks` and compiles out of production builds.
