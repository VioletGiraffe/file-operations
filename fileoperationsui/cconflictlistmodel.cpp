#include "cconflictlistmodel.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()
#include "utils/displayformatting.h"


DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QFont>
#include <QPalette>
#include <QStringBuilder>
#include <QStyle>
RESTORE_COMPILER_WARNINGS

namespace
{

bool isDirectoryLike(const OperationEntryKind kind) noexcept
{
	return kind == OperationEntryKind::Directory || kind == OperationEntryKind::DirectoryLink;
}

QString conflictText(const PlannedConflict& conflict)
{
	const OperationIssue& issue = conflict.issue;
	if (issue.kind == IssueKind::FileReplacement)
		return sideComparisonText(compareSides(conflict));

	const QString kindText = fileOperationIssueKindText(issue.kind);
	if (issue.kind != IssueKind::TypeMismatch || !issue.destination)
		return kindText;
	return QObject::tr("%1: %2 → %3").arg(kindText, fileOperationEntryKindNoun(issue.source.kind), fileOperationEntryKindNoun(issue.destination->kind));
}

// Empty for an entry without a size of its own.
QString sizeText(const EntrySnapshot& entry)
{
	const bool hasSize = entry.kind == OperationEntryKind::RegularFile || entry.kind == OperationEntryKind::FileLink;
	return hasSize ? fileSizeToString(entry.size) : QString{};
}

QString lastWriteText(const thin_io::timestamp lastWrite)
{
	return lastWrite.is_set() ? QDateTime::fromSecsSinceEpoch(lastWrite.seconds).toString(QStringLiteral("dd.MM.yyyy hh:mm:ss")) : QString{};
}

bool answeredSkipOrRename(const std::optional<Decision>& resolution) noexcept
{
	return resolution && (resolution->action == DecisionAction::Skip || resolution->action == DecisionAction::Rename);
}

} // namespace

SideComparison compareSides(const PlannedConflict& conflict)
{
	const OperationIssue& issue = conflict.issue;
	assert_and_return_r(issue.kind == IssueKind::FileReplacement && issue.destination, SideComparison::TimeUnknown);

	if (!conflict.sourceLastWrite.is_set() || !conflict.destinationLastWrite.is_set())
		return SideComparison::TimeUnknown;
	if (conflict.destinationLastWrite.seconds < conflict.sourceLastWrite.seconds)
		return SideComparison::DestinationOlder;
	if (conflict.destinationLastWrite.seconds > conflict.sourceLastWrite.seconds)
		return SideComparison::DestinationNewer;
	return issue.destination->size == issue.source.size ? SideComparison::SameSizeAndTime : SideComparison::SizeDiffers;
}

QString sideComparisonText(const SideComparison comparison)
{
	using enum SideComparison;
	switch (comparison)
	{
	case DestinationOlder: return QObject::tr("Destination older");
	case DestinationNewer: return QObject::tr("Destination newer");
	case SameSizeAndTime: return QObject::tr("Same size and time");
	case SizeDiffers: return QObject::tr("Same time, different size");
	case TimeUnknown: return QObject::tr("Already exists, time unknown");
	}
	return {};
}

CConflictListModel::CConflictListModel(ConflictPlan plan, const PromptOperation operation, QObject* parent)
	: QAbstractItemModel{ parent }
	, _operation{ operation }
	, _fileIcon{ QFileIconProvider{}.icon(QFileIconProvider::File) }
	, _folderIcon{ QFileIconProvider{}.icon(QFileIconProvider::Folder) }
	, _unansweredIcon{ QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning) }
{
	_rows.reserve(plan.size());
	for (PlannedConflict& conflict : plan)
	{
		Row row{ .conflict = mv(conflict) };
		// A refused rename stays visible in the row's text but is not an answer.
		if (!row.conflict.renameTargetTakenByAnotherItem)
			row.resolution = row.conflict.resolution;

		// The plan lists a directory before what it holds, so the parent is a top-level directory row seen earlier.
		for (const size_t candidate : _topLevelRows)
		{
			const EntrySnapshot& candidateSource = _rows[candidate].conflict.issue.source;
			if (isDirectoryLike(candidateSource.kind) && row.conflict.issue.source.path.isStrictDescendantOf(candidateSource.path))
			{
				row.parentRow = candidate;
				break;
			}
		}

		const size_t planRow = _rows.size();
		std::vector<size_t>& siblings = row.parentRow ? _rows[*row.parentRow].childRows : _topLevelRows;
		row.positionInParent = siblings.size();
		siblings.push_back(planRow);
		_rows.push_back(mv(row));
	}
}

bool CConflictListModel::isApplicable(const size_t planRow) const
{
	const Row& row = _rows[planRow];
	return !row.parentRow || !answeredSkipOrRename(_rows[*row.parentRow].resolution);
}

bool CConflictListModel::isAnswered(const size_t planRow) const
{
	return !isApplicable(planRow) || _rows[planRow].resolution.has_value();
}

size_t CConflictListModel::unansweredCount() const
{
	size_t count = 0;
	for (size_t i = 0; i < _rows.size(); ++i)
		count += isAnswered(i) ? 0 : 1;
	return count;
}

QModelIndex CConflictListModel::indexOf(const size_t planRow, const int column) const
{
	assert_and_return_r(planRow < _rows.size(), {});
	return createIndex(static_cast<int>(_rows[planRow].positionInParent), column, static_cast<quintptr>(planRow));
}

void CConflictListModel::setResolution(const size_t planRow, Decision decision)
{
	assert_and_return_r(planRow < _rows.size(), );
	Row& row = _rows[planRow];
	row.resolution = mv(decision);
	emit dataChanged(indexOf(planRow, Action), indexOf(planRow, Action));

	if (!row.childRows.empty())
	{
		const QModelIndex thisIndex = indexOf(planRow);
		emit dataChanged(index(0, 0, thisIndex), index(static_cast<int>(row.childRows.size()) - 1, ColumnCount - 1, thisIndex));
	}
}

ConflictResolutions CConflictListModel::resolutions() const
{
	ConflictResolutions result;
	result.reserve(_rows.size());
	for (size_t i = 0; i < _rows.size(); ++i)
		result.push_back(isApplicable(i) ? _rows[i].resolution : std::nullopt);
	return result;
}

QModelIndex CConflictListModel::index(const int row, const int column, const QModelIndex& parent) const
{
	if (row < 0 || column < 0 || column >= ColumnCount)
		return {};

	const std::vector<size_t>& rows = parent.isValid() ? _rows[planRow(parent)].childRows : _topLevelRows;
	if (static_cast<size_t>(row) >= rows.size())
		return {};
	return createIndex(row, column, static_cast<quintptr>(rows[static_cast<size_t>(row)]));
}

QModelIndex CConflictListModel::parent(const QModelIndex& index) const
{
	if (!index.isValid())
		return {};
	const std::optional<size_t>& parentRow = _rows[planRow(index)].parentRow;
	return parentRow ? indexOf(*parentRow) : QModelIndex{};
}

int CConflictListModel::rowCount(const QModelIndex& parent) const
{
	if (parent.column() > 0)
		return 0;
	return static_cast<int>(parent.isValid() ? _rows[planRow(parent)].childRows.size() : _topLevelRows.size());
}

int CConflictListModel::columnCount(const QModelIndex&) const
{
	return ColumnCount;
}

QVariant CConflictListModel::data(const QModelIndex& index, const int role) const
{
	if (!index.isValid())
		return {};

	const size_t thisRow = planRow(index);
	const Row& row = _rows[thisRow];
	const OperationIssue& issue = row.conflict.issue;
	if (role == Qt::DecorationRole)
	{
		if (index.column() == Name)
			return isDirectoryLike(issue.source.kind) ? _folderIcon : _fileIcon;
		if (index.column() == Action && !isAnswered(thisRow))
			return _unansweredIcon;
		return {};
	}
	if (role == Qt::FontRole)
	{
		// The newer of the two times is bold
		if (issue.kind != IssueKind::FileReplacement)
			return {};
		const SideComparison comparison = compareSides(row.conflict);
		const bool newer = (index.column() == SourceModified && comparison == SideComparison::DestinationOlder)
			|| (index.column() == DestinationModified && comparison == SideComparison::DestinationNewer);
		if (!newer)
			return {};
		QFont font;
		font.setBold(true);
		return font;
	}
	if (role == Qt::ForegroundRole)
	{
		const bool skipped = index.column() == Action && row.resolution && row.resolution->action == DecisionAction::Skip;
		return skipped ? QVariant{ QApplication::palette().color(QPalette::PlaceholderText) } : QVariant{};
	}
	if (role == Qt::ToolTipRole)
	{
		QString tip = QDir::toNativeSeparators(issue.source.path.value());
		if (issue.destination)
			tip += QStringLiteral("\n→ ") % QDir::toNativeSeparators(issue.destination->path.value());
		return tip;
	}
	if (role == Qt::TextAlignmentRole)
	{
		const bool sizeColumn = index.column() == SourceSize || index.column() == DestinationSize;
		return sizeColumn ? QVariant::fromValue(Qt::Alignment{ Qt::AlignRight | Qt::AlignVCenter }) : QVariant{};
	}
	if (role != Qt::DisplayRole)
		return {};

	switch (index.column())
	{
	case Name: return nameText(row);
	case Conflict: return conflictText(row.conflict);
	case SourceSize: return sizeText(issue.source);
	case SourceModified: return lastWriteText(row.conflict.sourceLastWrite);
	case DestinationSize: return issue.destination ? sizeText(*issue.destination) : QString{};
	case DestinationModified: return lastWriteText(row.conflict.destinationLastWrite);
	case Action: return actionText(thisRow);
	}
	return {};
}

QVariant CConflictListModel::headerData(const int section, const Qt::Orientation orientation, const int role) const
{
	if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
		return {};

	switch (section)
	{
	case Name: return tr("Name");
	case Conflict: return tr("Conflict");
	case SourceSize: return tr("Source size");
	case SourceModified: return tr("Source modified");
	case DestinationSize: return tr("Destination size");
	case DestinationModified: return tr("Destination modified");
	case Action: return tr("Action");
	}
	return {};
}

Qt::ItemFlags CConflictListModel::flags(const QModelIndex& index) const
{
	if (!index.isValid())
		return Qt::NoItemFlags;
	return isApplicable(planRow(index)) ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

QString CConflictListModel::nameText(const Row& row) const
{
	const QString& path = row.conflict.issue.source.path.value();
	if (!row.parentRow)
		return row.conflict.issue.source.path.name();

	// Relative to the parent row's directory, so a deep collision is located at a glance.
	const QString& parentPath = _rows[*row.parentRow].conflict.issue.source.path.value();
	return QDir::toNativeSeparators(path.mid(parentPath.size() + 1));
}

QString CConflictListModel::actionText(const size_t planRow) const
{
	const Row& row = _rows[planRow];
	if (row.resolution)
	{
		if (row.resolution->action == DecisionAction::Rename)
			return tr("Rename to \"%1\"").arg(row.resolution->newName.value_or(QString{}));
		return fileOperationActionLabel(row.resolution->action, _operation);
	}
	if (!isApplicable(planRow))
		return {};

	const PlannedConflict& conflict = row.conflict;
	if (conflict.renameTargetTakenByAnotherItem && conflict.resolution && conflict.resolution->newName)
		return tr("\"%1\" is taken by another item of this operation, choose another name").arg(*conflict.resolution->newName);
	return tr("Choose an action");
}
