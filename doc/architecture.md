# Architecture

The parts and how a host consumes them are in the [README](../README.md); authoring rules in
[coding-style.md](coding-style.md).

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
8. A transfer plans before it executes. Planning inspects and scans every root, and runs the destination resolvers
   with the context recording: a conflict (collision, type mismatch, unsupported entry, read-only source of a
   copy-based move) becomes a plan row instead of a prompt, and the walk descends as if every merge were accepted.
   The plan is presented once, re-planned only while answers rename something, and its answers are stored per
   (source path, issue kind) as a queue. Execution then resolves every node again as if unplanned; the context
   answers a question from the stored queue when the same key comes up, and prompts live otherwise. Errors
   (`ActionFailed`) are never planned. Planning writes only what a move writes before its first question: the
   destination parent chain and the optimistic whole-root rename.
   - Two items planned onto one final name: a rename that causes it is refused in the plan; two unrenamed names that
     fold to one, and same-name roots from different folders, are left to execution's live prompt.
   - Whether a move crosses volumes is known only from a failed rename. Where the whole-root rename hit an existing
     destination, planning predicts it from the volume identities; a wrong prediction costs unused plan rows or one
     live prompt, never a wrong write.

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

## Paths

`paths/pathsyntax.h` is the path text grammar the libraries and their hosts share: which spellings are absolute, where
the root ends, lexical normalization.

The file-operation engine uses `CEntryPath`: an absolute `/`-separated path without a trailing separator except at
roots. Keep conversions at the type's boundaries. The parser adds operation rules to the grammar: it rejects embedded
NUL and `.`/`..` components. Win32 namespace prefixes are not absolute paths to the grammar; native-path prefixing
belongs to the native bridge. See
file-commander's `doc/planned-refactors/native-paths.md` for the remaining POSIX filename limitation.

## Job threading

`CFileOperationJob` owns one worker and a mutex/condition-variable state machine for pause, cancellation, decisions,
and the event queue. The worker emits progress, decision requests, and one summary; the UI drains and dispatches
them after releasing the mutex because presenting a decision can enter a nested event loop.

Progress may coalesce, while decisions and the final summary preserve ordering. State mutations used by wait
predicates occur under the same mutex before notification. Cancellation releases every wait and invalidates an
undrained decision. Job destruction cancels, wakes, and joins; owning UI objects must outlive that sequence.

## Dialogs (`fileoperationsui/`)

`CFileOperationDialog` owns one `CFileOperationJob`, drains typed events, presents only the decisions offered by the
core, and formats the outcome. It may remain hidden for quick success, appear for a decision or background
operation, and keep a completed summary visible only when it needs attention. Closing an active dialog suppresses
later display but does not weaken job/listener lifetime requirements.

`CConflictListDialog` presents the conflict plan. A fresh plan with existing files or folders opens compact: counts by
kind and comparison, and Replace all / Replace older / Skip all, which merge every folder, answer the file replacements,
and start. Rows a button cannot answer (other kinds, and for Replace older, times that do not decide) open the list with
those rows selected. The list: `CConflictListModel` shows the rows as a two-level tree (a row under an earlier directory
row's source is its child), action buttons apply to the selected rows that allow the action, a Select menu picks rows by
kind or comparison, and Start is enabled only once every applicable row is answered. A row under a root answered Skip
or Rename is disabled and resolves to nullopt.

`CFileOperationPrompt` renders one decision request: only the actions the engine allows, the "remaining matching
issues" scope where permitted, and a rename row when renaming is offered. `fileoperationtexts` holds the wording the
dialogs and hosts share, request validation errors included.

## Directory scanning (`directoryscan/`)

`scanDirectory()` walks a tree depth-first and reports every entry below the root, never the root itself, as the
containing folder's path plus thin_io's `directory_entry`. It can follow directory links and breaks cycles with resolved
native identity, not path text. The engine builds its manifests with a traversal of its own.

`thinioqstring` is the `QString`-path access to thin_io the scanner uses; hosts use it too. Its name decoding is lossy
for POSIX names that are not valid in the locale's encoding.

## Folder comparison (`foldercomparison/`)

`compareFolders()` enumerates both trees, pairs entries by relative path (exactly, or folding case and Unicode
normalization), and reads file contents only where type and size cannot already tell a pair apart. Directory links are
followed, so a link is judged by the contents a transfer would copy. `compareFileContents()` is the byte comparison
underneath.
