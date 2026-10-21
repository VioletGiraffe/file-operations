#include "pathsyntax.h"


DISABLE_COMPILER_WARNINGS
#include <QList>
RESTORE_COMPILER_WARNINGS

#include <vector>

namespace
{

#ifdef _WIN32
[[nodiscard]] bool isSeparator(const QChar c) noexcept
{
	return c == u'/' || c == u'\\';
}

[[nodiscard]] bool isAsciiLetter(const QChar c) noexcept
{
	return (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z');
}

[[nodiscard]] qsizetype indexOfSeparator(const QStringView path, const qsizetype from) noexcept
{
	for (qsizetype i = from; i < path.size(); ++i)
	{
		if (isSeparator(path[i]))
			return i;
	}
	return -1;
}
#else
[[nodiscard]] bool isSeparator(const QChar c) noexcept
{
	return c == u'/';
}
#endif

} // namespace

qsizetype absoluteRootLength(const QStringView path)
{
#ifdef _WIN32
	if (path.size() >= 3 && isAsciiLetter(path[0]) && path[1] == u':' && isSeparator(path[2]))
		return 3;

	// UNC: exactly two leading separators, then the server and the share
	if (path.size() < 3 || !isSeparator(path[0]) || !isSeparator(path[1]) || isSeparator(path[2]))
		return 0;

	const qsizetype serverEnd = indexOfSeparator(path, 2);
	if (serverEnd < 0)
		return 0;

	qsizetype shareEnd = indexOfSeparator(path, serverEnd + 1);
	if (shareEnd < 0)
		shareEnd = path.size();

	const QStringView server = path.sliced(2, serverEnd - 2);
	const QStringView share = path.sliced(serverEnd + 1, shareEnd - serverEnd - 1);
	// "?" and "." servers are the Win32 extended and device namespaces; a dot component is a traversal, never a name
	if (share.isEmpty() || server == u"?" || server == u"." || server == u".." || share == u"." || share == u"..")
		return 0;

	return shareEnd;
#else
	return !path.isEmpty() && path[0] == u'/' ? 1 : 0;
#endif
}

bool isRootPath(const QStringView path)
{
	const qsizetype rootLength = absoluteRootLength(path);
	return rootLength > 0 && (path.size() == rootLength || (path.size() == rootLength + 1 && isSeparator(path.back())));
}

QString normalizedAbsolutePath(const QStringView path)
{
	const qsizetype rootLength = absoluteRootLength(path);
	if (rootLength == 0)
		return {};

	QString text = path.toString();
#ifdef _WIN32
	text.replace(u'\\', u'/');
	if (text[1] == u':')
		text[0] = text[0].toUpper();
#endif

	std::vector<QStringView> components;
	for (const QStringView component : QStringView{ text }.sliced(rootLength).split(u'/', Qt::SkipEmptyParts))
	{
		if (component == u".")
			continue;

		if (component == u"..")
		{
			if (!components.empty())
				components.pop_back();
			continue;
		}

		components.push_back(component);
	}

	QString result = text.first(rootLength);
	for (const QStringView component : components)
	{
		if (!result.endsWith(u'/'))
			result += u'/';
		result += component;
	}

	return result;
}
