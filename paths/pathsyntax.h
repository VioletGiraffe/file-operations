#pragma once

// Path text grammar: '/' separators, and '\' as well on Windows. Pure text: nothing here touches the filesystem.

// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QString>
#include <QStringView>
RESTORE_COMPILER_WARNINGS

// Length of an absolute path's root: "/" -> 1, "C:/" -> 3, "//server/share" -> through the share name.
// 0 when the path is not absolute: relative, drive-relative ("C:", "C:dir"), rooted without a drive ("/dir" on Windows),
// a UNC spelling without a share, or a Win32 extended or device namespace prefix.
[[nodiscard]] qsizetype absoluteRootLength(QStringView path);

// A root alone, with or without a trailing separator
[[nodiscard]] bool isRootPath(QStringView path);

// '/'-separated, repeated separators collapsed, "." and ".." resolved lexically without climbing above the root, the drive
// letter uppercase, a trailing separator only on "/" and "C:/". Names are kept verbatim, trailing dots and spaces included.
// Empty when the path is not absolute.
[[nodiscard]] QString normalizedAbsolutePath(QStringView path);
