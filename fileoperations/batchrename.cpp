#include "batchrename.h"

DISABLE_COMPILER_WARNINGS
#include <QHash>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <numeric>
#include <utility>

namespace {

[[nodiscard]] QString withLetterCase(const QString& text, const LetterCase letterCase)
{
	using enum LetterCase;
	switch (letterCase)
	{
	case Unchanged: return text;
	case Lower: return text.toLower();
	case Upper: return text.toUpper();
	case Title:
	{
		QString result = text.toLower();
		bool wordStart = true;
		for (QChar& c : result)
		{
			if (wordStart)
				c = c.toUpper();
			wordStart = !c.isLetterOrNumber() && c != u'\'';
		}
		return result;
	}
	}
	return text;
}

// "N2-5", "N3", "N2-": a 1-based inclusive range of the name. nullopt for any other token.
[[nodiscard]] std::optional<QString> nameRange(const QStringView token, const QString& name)
{
	static const QRegularExpression rangeToken{ QStringLiteral("^N(\\d+)(-(\\d*))?$") };
	const QRegularExpressionMatch match = rangeToken.matchView(token);
	if (!match.hasMatch())
		return std::nullopt;

	const qsizetype first = match.capturedView(1).toLongLong();
	if (first < 1)
		return std::nullopt;

	qsizetype last = first;
	if (match.hasCaptured(2))
		last = match.capturedView(3).isEmpty() ? name.size() : match.capturedView(3).toLongLong();
	return name.mid(first - 1, std::max<qsizetype>(0, last - first + 1));
}

[[nodiscard]] std::optional<QString> tokenValue(const QStringView token, const BatchRenameSource& source, const QString& counter)
{
	if (token.size() != 1)
		return nameRange(token, source.name);

	switch (token[0].unicode())
	{
	case 'N': return source.name;
	case 'E': return source.extension;
	case 'P': return source.parentFolderName;
	case 'C': return counter;
	case 'Y': return source.modificationTime.toString(QStringLiteral("yyyy"));
	case 'M': return source.modificationTime.toString(QStringLiteral("MM"));
	case 'D': return source.modificationTime.toString(QStringLiteral("dd"));
	case 'h': return source.modificationTime.toString(QStringLiteral("hh"));
	case 'm': return source.modificationTime.toString(QStringLiteral("mm"));
	case 's': return source.modificationTime.toString(QStringLiteral("ss"));
	default: return std::nullopt;
	}
}

[[nodiscard]] QString expandMask(const QString& mask, const BatchRenameSource& source, const QString& counter)
{
	QString result;
	for (qsizetype i = 0; i < mask.size();)
	{
		if (mask[i] == u'[')
		{
			if (const qsizetype close = mask.indexOf(u']', i + 1); close >= 0)
			{
				if (const auto value = tokenValue(QStringView{ mask }.sliced(i + 1, close - i - 1), source, counter))
				{
					result += *value;
					i = close + 1;
					continue;
				}
			}
		}

		result += mask[i++];
	}
	return result;
}

// The destination exists: either a file inlineRename would only replace on confirmation, or an entry it never replaces
[[nodiscard]] bool isNameTaken(const InlineRenameResult& result)
{
	return result.status == InlineRenameStatus::ReplacementRequired || result.status == InlineRenameStatus::Rejected;
}

} // namespace

CBatchRenamePattern::CBatchRenamePattern(BatchRenameRules rules) :
	_rules{ std::move(rules) },
	_findRegex{ _rules.findIsRegex ? _rules.find : QString{},
		_rules.findIsCaseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption }
{
}

QString CBatchRenamePattern::findPatternError() const
{
	return _findRegex.isValid() ? QString{} : _findRegex.errorString();
}

QString CBatchRenamePattern::newName(const BatchRenameSource& source, const int index) const
{
	const qint64 counterValue = qint64{ _rules.counterStart } + qint64{ index } * _rules.counterStep;
	const QString counter = QString::number(counterValue).rightJustified(_rules.counterDigits, u'0');

	QString result = withLetterCase(expandMask(_rules.nameMask, source, counter), _rules.letterCase);
	const LetterCase extensionCase = _rules.letterCase == LetterCase::Title ? LetterCase::Lower : _rules.letterCase;
	if (const QString extension = withLetterCase(expandMask(_rules.extensionMask, source, counter), extensionCase); !extension.isEmpty())
	{
		result += u'.';
		result += extension;
	}

	if (_rules.find.isEmpty())
		return result;

	if (!_rules.findIsRegex)
		return result.replace(_rules.find, _rules.replaceWith, _rules.findIsCaseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);

	if (_findRegex.isValid())
		result.replace(_findRegex, _rules.replaceWith);
	return result;
}

std::vector<std::optional<InlineRenameResult>> batchRename(const std::vector<BatchRenameItem>& items, const std::function<bool()>& shouldContinue)
{
	std::vector<std::optional<InlineRenameResult>> results(items.size());
	std::vector<size_t> pending(items.size());
	std::iota(pending.begin(), pending.end(), size_t{ 0 });

	// Another pass follows any pass that renamed something: the waiting lookup matches exact spelling, and a filesystem may not.
	for (bool renamedAny = true; renamedAny && !pending.empty();)
	{
		renamedAny = false;
		QHash<QString, size_t> waitingForName; // Taken target path -> the item waiting for it

		for (size_t item : pending)
		{
			while (true)
			{
				if (!shouldContinue())
					return results;

				const BatchRenameItem& current = items[item];
				const InlineRenameResult& result = results[item].emplace(inlineRename(current.source, current.newName, false));
				if (isNameTaken(result))
				{
					waitingForName.insert(current.source.parent().child(current.newName).value(), item);
					break;
				}

				if (result.status != InlineRenameStatus::Renamed)
					break;

				renamedAny = true;
				// The old name is free now, so the item waiting for it goes next
				const auto waiting = waitingForName.constFind(current.source.value());
				if (waiting == waitingForName.cend())
					break;

				item = *waiting;
				waitingForName.erase(waiting);
			}
		}

		std::erase_if(pending, [&results](const size_t item) { return !isNameTaken(*results[item]); });
	}

	return results;
}
