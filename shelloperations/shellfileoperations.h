#pragma once

#ifdef _WIN32
class QString;

// Pastes the clipboard's files into destinationFolder with the shell's own Paste command, which also accepts virtual files.
// Blocks until the shell's progress and prompt UI is dismissed, so it belongs on a CShellOperationRunner.
// ownerWindow is an HWND that outlives the call, or null.
// False when destinationFolder has no shell Paste command or the command fails.
[[nodiscard]] bool pasteFromClipboardWithShell(const QString& destinationFolder, void* ownerWindow);
#endif
