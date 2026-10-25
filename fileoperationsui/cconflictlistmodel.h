#pragma once

#include "fileoperationtexts.h" // PromptOperation

#include "fileoperations/fileoperationtypes.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QAbstractItemModel>
#include <QIcon>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <vector>

// A FileReplacement row's destination against its source. Times compare to the second, the resolution the list shows.
enum class SideComparison
{
	DestinationOlder,
	DestinationNewer,
	SameSizeAndTime,
	SizeDiffers, // Same time
	TimeUnknown
};

// Requires a FileReplacement row.
[[nodiscard]] SideComparison compareSides(const PlannedConflict& conflict);
[[nodiscard]] QString sideComparisonText(SideComparison comparison);

// The plan as a two-level tree: a row whose source lies under an earlier directory row's source is that row's child,
// every other row is top-level. Holds the resolutions as the user sets them. A row is applicable while its parent row
// is not answered Skip or Rename; an inapplicable row is disabled and resolves to nullopt.
class CConflictListModel final : public QAbstractItemModel
{
public:
	enum Column
	{
		Name,
		Conflict,
		SourceSize,
		SourceModified,
		DestinationSize,
		DestinationModified,
		Action,
		ColumnCount
	};

	CConflictListModel(ConflictPlan plan, PromptOperation operation, QObject* parent = nullptr);

	[[nodiscard]] size_t rowsTotal() const noexcept { return _rows.size(); }
	[[nodiscard]] const PlannedConflict& conflict(size_t planRow) const noexcept { return _rows[planRow].conflict; }
	[[nodiscard]] const std::optional<Decision>& resolution(size_t planRow) const noexcept { return _rows[planRow].resolution; }
	[[nodiscard]] bool isApplicable(size_t planRow) const;
	[[nodiscard]] bool isAnswered(size_t planRow) const;
	[[nodiscard]] size_t unansweredCount() const;

	[[nodiscard]] size_t planRow(const QModelIndex& index) const noexcept { return static_cast<size_t>(index.internalId()); }
	[[nodiscard]] QModelIndex indexOf(size_t planRow, int column = Name) const;

	// Also re-renders the children: their applicability follows this row's answer.
	void setResolution(size_t planRow, Decision decision);

	// One per plan row: the resolution, or nullopt for an inapplicable row.
	[[nodiscard]] ConflictResolutions resolutions() const;

	[[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
	[[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;
	[[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
	[[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
	[[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
	[[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
	[[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
	struct Row
	{
		PlannedConflict conflict;
		std::optional<Decision> resolution;
		std::optional<size_t> parentRow;
		size_t positionInParent = 0; // Within the parent's children, or within the top-level rows
		std::vector<size_t> childRows;
	};

	[[nodiscard]] QString nameText(const Row& row) const;
	[[nodiscard]] QString actionText(size_t planRow) const;

	std::vector<Row> _rows;
	std::vector<size_t> _topLevelRows;
	const PromptOperation _operation;
	const QIcon _fileIcon;
	const QIcon _folderIcon;
	const QIcon _unansweredIcon;
};
