#include "newnamecheck.h"

#include "centrypath.h"


DISABLE_COMPILER_WARNINGS
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <algorithm>

namespace
{

#ifdef _WIN32
constexpr bool NATIVE_RULES_ARE_WINDOWS_RULES = true;
#else
constexpr bool NATIVE_RULES_ARE_WINDOWS_RULES = false;
#endif

bool isRejectedWhitespaceCharacter(const QChar c, const bool windowsRules)
{
	// Non-ASCII characters that Qt classifies as whitespace are ordinary filename text on POSIX.
	return c.isSpace() && (windowsRules || c.unicode() < 0x80);
}

bool isWindowsInvalidNameCharacter(const QChar c)
{
	if (c.unicode() < 0x20)
		return true;

	switch (c.unicode())
	{
	case '<':
	case '>':
	case ':':
	case '"':
	case '|':
	case '?':
	case '*': return true;
	default: return false;
	}
}

bool isWindowsReservedDeviceName(const QString& name)
{
	// Windows ignores the extension and the spaces ending the stem: "NUL.txt" and "CON .log" are devices too.
	const qsizetype dotPosition = name.indexOf(QLatin1Char('.'));
	QStringView stem = QStringView{ name }.left(dotPosition == -1 ? name.size() : dotPosition);
	while (stem.endsWith(QLatin1Char(' ')))
		stem.chop(1);

	if (stem.compare(QLatin1String("CON"), Qt::CaseInsensitive) == 0
		|| stem.compare(QLatin1String("PRN"), Qt::CaseInsensitive) == 0
		|| stem.compare(QLatin1String("AUX"), Qt::CaseInsensitive) == 0
		|| stem.compare(QLatin1String("NUL"), Qt::CaseInsensitive) == 0)
		return true;

	if (stem.length() != 4)
		return false;

	// The superscript digits are reserved as well.
	const char16_t digit = stem[3].unicode();
	if ((digit < u'1' || digit > u'9') && digit != u'¹' && digit != u'²' && digit != u'³')
		return false;

	const QStringView prefix = stem.left(3);
	return prefix.compare(QLatin1String("COM"), Qt::CaseInsensitive) == 0
		|| prefix.compare(QLatin1String("LPT"), Qt::CaseInsensitive) == 0;
}

} // namespace

NameRejection checkNewEntryName(const QString& name, const NameRules rules)
{
	const bool windowsRules = NATIVE_RULES_ARE_WINDOWS_RULES || rules == NameRules::Portable;

	if (name.isEmpty())
		return NameRejection::Empty;

	if (name.contains(QChar{ 0 }))
		return NameRejection::InvalidCharacter;

	if (windowsRules && std::any_of(name.cbegin(), name.cend(), isWindowsInvalidNameCharacter))
		return NameRejection::InvalidCharacter;

	if (std::all_of(name.cbegin(), name.cend(), [windowsRules](const QChar c) { return isRejectedWhitespaceCharacter(c, windowsRules); }))
		return NameRejection::WhitespaceOnly;

	// isSingleComponentName owns the structural rules; which of them failed matters only for the wording.
	if (!isSingleComponentName(name))
	{
		return name == QLatin1String(".") || name == QLatin1String("..")
			? NameRejection::DotComponent
			: NameRejection::Separator;
	}

	if (!windowsRules)
		return NameRejection::None;

	// A Windows separator that isSingleComponentName accepts as name text on POSIX.
	if (name.contains(QLatin1Char('\\')))
		return NameRejection::Separator;

	// Legal on NTFS and reachable through the extended-length prefix this app uses, but every tool that goes through
	// Win32 normalization - Explorer, cmd, most applications - would resolve the name without them and miss the entry.
	if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
		return NameRejection::TrailingDotOrSpace;

	if (isWindowsReservedDeviceName(name))
		return NameRejection::ReservedDeviceName;

	return NameRejection::None;
}

NameRejection checkNewEntryPath(const QString& relativePath)
{
	QString path = relativePath;
#ifdef _WIN32
	path.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif

	const QStringList components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	if (components.isEmpty())
		return NameRejection::Empty;

	for (const QString& component : components)
	{
		if (const NameRejection rejection = checkNewEntryName(component); rejection != NameRejection::None)
			return rejection;
	}

	return NameRejection::None;
}
