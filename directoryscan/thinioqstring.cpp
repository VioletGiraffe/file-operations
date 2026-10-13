#include "thinioqstring.h"

#include "fs.hpp" // thin_io

DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFile>
RESTORE_COMPILER_WARNINGS

#ifdef _WIN32
[[nodiscard]] static std::wstring nativePath(const QString& path)
{
	return path.toStdWString();
}
#else
[[nodiscard]] static QByteArray nativePath(const QString& path)
{
	return QFile::encodeName(path);
}
#endif

std::optional<thin_io::entry_identity> resolvedObjectId(const QString& path)
{
	// thin_io prefixes with \\?\, which turns off the normalization that would otherwise absorb a trailing separator.
	const auto metadata = thin_io::get_entry_metadata(nativePath(withoutTrailingSeparator(path)).data(), thin_io::link_behavior::follow);
	if (!metadata)
		return {};

	return metadata->identity;
}

bool canListDirectory(const QString& dirPath)
{
	return thin_io::can_list_directory(nativePath(dirPath).data()).has_value();
}

thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectoryWithDetails(const QString& dirPath)
{
	return thin_io::list_directory(nativePath(dirPath).data(), thin_io::listing_detail::full);
}

thin_io::filesystem_result<thin_io::directory_entry> getDirectoryEntry(const QString& path)
{
	return thin_io::get_directory_entry(nativePath(path).data());
}

QString nativeNameToQString(const thin_io::native_string& name)
{
#ifdef _WIN32
	return QString::fromStdWString(name);
#else
	return QFile::decodeName(QByteArray::fromRawData(name.data(), static_cast<qsizetype>(name.size())));
#endif
}

QString withoutTrailingSeparator(QString path)
{
	if ((path.endsWith('/') || path.endsWith(QDir::separator())) && !QDir{ path }.isRoot())
		path.chop(1);

	return path;
}
