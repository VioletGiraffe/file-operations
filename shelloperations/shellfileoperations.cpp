#include "shellfileoperations.h"

#ifdef _WIN32
// Submodule includes
#include "compiler/compiler_warnings_control.h"
#include "system/shellcontextmenu.hpp"


DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <filesystem>

bool pasteFromClipboardWithShell(const QString& destinationFolder, void* const ownerWindow)
{
	return invokeShellVerb({ std::filesystem::path{ destinationFolder.toStdWString() } }, static_cast<HWND>(ownerWindow), "paste");
}
#endif
