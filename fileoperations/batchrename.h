#pragma once

#include "centrypath.h"
#include "inlinerename.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QDateTime>
#include <QRegularExpression>
#include <QString>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <optional>
#include <vector>

enum class LetterCase
{
	Unchanged,
	Lower,
	Upper,
	Title // Each word capitalized; the extension is lowercased
};

// Mask tokens, case-sensitive; any other bracketed text is literal:
//   [N] name without extension, [N2-5] its characters 2 to 5 (1-based), [N3] one character, [N2-] the rest from 2
//   [E] extension, [P] parent folder name, [C] counter
//   [Y] [M] [D] [h] [m] [s] modification time: year, month, day, hour, minute, second
struct BatchRenameRules
{
	QString nameMask = QStringLiteral("[N]");
	QString extensionMask = QStringLiteral("[E]"); // An empty result drops the dot too
	LetterCase letterCase = LetterCase::Unchanged;
	QString find; // Empty: no replacement
	QString replaceWith; // A regex replacement may refer to captures as \1
	bool findIsRegex = false;
	bool findIsCaseSensitive = false;
	int counterStart = 1;
	int counterStep = 1;
	int counterDigits = 1; // Zero-padded to this width

	[[nodiscard]] bool operator==(const BatchRenameRules&) const = default;
};

struct BatchRenameSource
{
	QString name; // Without extension; a folder's full name
	QString extension;
	QString parentFolderName;
	QDateTime modificationTime;
};

// Applies the masks, then the letter case, then find and replace.
// The default rules reproduce the source name: name + '.' + extension, or the name alone without an extension.
class CBatchRenamePattern
{
public:
	explicit CBatchRenamePattern(BatchRenameRules rules);

	// Empty unless the find text is an invalid regex; newName() then skips the replacement
	[[nodiscard]] QString findPatternError() const;
	// index: the item's position in the batch, for the counter
	[[nodiscard]] QString newName(const BatchRenameSource& source, int index) const;

private:
	const BatchRenameRules _rules;
	const QRegularExpression _findRegex;
};

struct BatchRenameItem
{
	CEntryPath source;
	QString newName;
};

// Renames each item in its own folder through inlineRename(), never replacing an existing entry.
// An item whose new name is taken is retried once another item frees that name: a chain (1->2, 2->3) completes in any order, a cycle (1->2, 2->1) does not.
// shouldContinue is called before every rename attempt; false stops the batch, and the items never attempted stay nullopt.
[[nodiscard]] std::vector<std::optional<InlineRenameResult>> batchRename(const std::vector<BatchRenameItem>& items, const std::function<bool()>& shouldContinue);
