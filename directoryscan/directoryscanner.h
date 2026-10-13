#pragma once

#include "compiler/compiler_warnings_control.h"
#include "filesystem_types.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <functional>

// The target of a live link, otherwise the entry itself: a broken link keeps its own kind.
[[nodiscard]] const thin_io::entry_status& followedStatus(const thin_io::directory_entry& entry) noexcept;

// Depth-first scan of everything below rootPath, an absolute '/'-separated directory path. The root itself is not reported.
// parentPath is the directory holding the entry and ends with '/'.
// reachedThroughLink is true for entries found by traversing a directory link (symlink / junction). With followDirLinks = false,
// a directory link is still reported, but its contents are not.
// Siblings arrive in the filesystem's order, unsorted.
// Link cycles are detected and not descended into, so the scan always terminates.
void scanDirectory(const QString& rootPath,
	const std::function<void (const QString& parentPath, const thin_io::directory_entry& entry, bool reachedThroughLink)>& observer,
	const std::atomic<bool>& abort = std::atomic<bool>{false},
	bool followDirLinks = true);
