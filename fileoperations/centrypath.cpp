#include "centrypath.h"

#include "paths/pathsyntax.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp"


DISABLE_COMPILER_WARNINGS
#include <QList>
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

#include <algorithm>

CEntryPath::CEntryPath(QString normalizedPath) noexcept :
	_path{ mv(normalizedPath) }
{
}

const QString& CEntryPath::value() const noexcept
{
	return _path;
}

bool CEntryPath::isRoot() const
{
	return _path.length() == absoluteRootLength(_path);
}

CEntryPath CEntryPath::parent() const
{
	assert_r(!isRoot());
	const qsizetype lastSeparator = _path.lastIndexOf(QLatin1Char('/'));
	// The root's own separator (if any) is part of the root spelling, not a component boundary to cut at.
	return CEntryPath{ _path.left(std::max(lastSeparator, absoluteRootLength(_path))) };
}

CEntryPath CEntryPath::child(const QString& name) const
{
	assert_r(isSingleComponentName(name));
	return _path.endsWith(QLatin1Char('/')) ? CEntryPath{ _path % name } : CEntryPath{ _path % QLatin1Char('/') % name };
}

QString CEntryPath::name() const
{
	if (isRoot())
		return _path;
	return _path.mid(_path.lastIndexOf(QLatin1Char('/')) + 1);
}

bool CEntryPath::isStrictDescendantOf(const CEntryPath& ancestor) const
{
	if (isRoot())
		return false;

	CEntryPath candidate = parent();
	for (;;)
	{
		if (candidate == ancestor)
			return true;
		if (candidate.isRoot())
			return false;
		candidate = candidate.parent();
	}
}

std::optional<CEntryPath> parseOperationPath(QString path)
{
	if (path.contains(QChar{ 0 }))
		return {};

#ifdef _WIN32
	path.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif

	const qsizetype rootLength = absoluteRootLength(path);
	if (rootLength == 0)
		return {};

	for (const QStringView component : QStringView{ path }.sliced(rootLength).split(QLatin1Char('/'), Qt::SkipEmptyParts))
	{
		if (component == QLatin1String(".") || component == QLatin1String(".."))
			return {};
	}

	return CEntryPath{ normalizedAbsolutePath(path) };
}

bool isSingleComponentName(const QString& name)
{
	if (name.contains(QChar{ 0 }))
		return false;

#ifdef _WIN32
	if (name.contains(QLatin1Char('\\'))) // A separator here; on POSIX an ordinary character a name may contain
		return false;
#endif

	return !name.isEmpty() && !name.contains(QLatin1Char('/')) && name != QLatin1String(".") && name != QLatin1String("..");
}
