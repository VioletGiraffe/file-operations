#pragma once

// QString-path access to thin_io.

#include "compiler/compiler_warnings_control.h"
#include "filesystem_error.hpp" // thin_io
#include "filesystem_types.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <vector>

// Unique identity of the filesystem entry the path resolves to (links are followed).
// Empty both when the path cannot be resolved (e. g. a broken link) and when the filesystem exposes no stable
// identity, so two empty results are never the same entry.
[[nodiscard]] std::optional<thin_io::entry_identity> resolvedObjectId(const QString& path);

// True for a directory the process may enumerate
[[nodiscard]] bool canListDirectory(const QString& dirPath);
// thin_io::list_directory() with listing_detail::full
[[nodiscard]] thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectoryWithDetails(const QString& dirPath);
[[nodiscard]] thin_io::filesystem_result<thin_io::directory_entry> getDirectoryEntry(const QString& path);
// Lossy for a POSIX name that is not valid in the locale's encoding
[[nodiscard]] QString nativeNameToQString(const thin_io::native_string& name);

// CFileSystemObject normalizes a directory's path to always end with a slash; native APIs are less accommodating,
// CreateFileW especially, once the \\?\ prefix has turned path normalization off. Strip it before handing a path to
// one. A root keeps its slash: "C:" names the drive's current directory rather than its root.
// Either separator is accepted.
[[nodiscard]] QString withoutTrailingSeparator(QString path);
