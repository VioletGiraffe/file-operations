#include "thinioqstring.h"

#include "paths/pathsyntax.h"

#include "fs.hpp" // thin_io
#include "qt_paths.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <QDir>
RESTORE_COMPILER_WARNINGS

std::optional<thin_io::entry_identity> resolvedObjectId(const QString& path)
{
	// thin_io prefixes with \\?\, which turns off the normalization that would otherwise absorb a trailing separator.
	const auto metadata = thin_io::get_entry_metadata(thin_io::native_path(withoutTrailingSeparator(path)).data(), thin_io::link_behavior::follow);
	if (!metadata)
		return {};

	return metadata->identity;
}

bool canListDirectory(const QString& dirPath)
{
	return thin_io::can_list_directory(thin_io::native_path(dirPath).data()).has_value();
}

thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectoryWithDetails(const QString& dirPath)
{
	return thin_io::list_directory(thin_io::native_path(dirPath).data(), thin_io::listing_detail::full);
}

thin_io::filesystem_result<thin_io::directory_entry> getDirectoryEntry(const QString& path)
{
	return thin_io::get_directory_entry(thin_io::native_path(path).data());
}

QString withoutTrailingSeparator(QString path)
{
	if ((path.endsWith('/') || path.endsWith(QDir::separator())) && !isRootPath(path))
		path.chop(1);

	return path;
}
