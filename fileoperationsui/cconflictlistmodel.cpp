#include "cconflictlistmodel.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()
#include "utils/displayformatting.h"


DISABLE_COMPILER_WARNINGS
#include <QDateTime>
#include <QDir>
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

namespace
{

QString issueText(const OperationIssue& issue)
{
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

CConflictListModel::CConflictListModel(ConflictPlan plan, const PromptOperation operation, QObject* parent)
	: QAbstractItemModel{ parent }
	, _operation{ operation }
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
			const bool directoryLike = candidateSource.kind == OperationEntryKind::Directory || candidateSource.kind == OperationEntryKind::DirectoryLink;
			if (directoryLike && row.conflict.issue.source.path.isStrictDescendantOf(candidateSource.path))
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

	const Row& row = _rows[planRow(index)];
	const OperationIssue& issue = row.conflict.issue;
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
	case Issue: return issueText(issue);
	case SourceSize: return sizeText(issue.source);
	case SourceModified: return lastWriteText(row.conflict.sourceLastWrite);
	case DestinationSize: return issue.destination ? sizeText(*issue.destination) : QString{};
	case DestinationModified: return lastWriteText(row.conflict.destinationLastWrite);
	case Action: return actionText(row);
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
	case Issue: return tr("Issue");
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

QString CConflictListModel::actionText(const Row& row) const
{
	if (row.resolution)
	{
		if (row.resolution->action == DecisionAction::Rename)
			return tr("Rename to \"%1\"").arg(row.resolution->newName.value_or(QString{}));
		return fileOperationActionLabel(row.resolution->action, _operation);
	}

	const PlannedConflict& conflict = row.conflict;
	if (conflict.renameTargetTakenByAnotherItem && conflict.resolution && conflict.resolution->newName)
		return tr("\"%1\" is taken by another item of this operation, choose another name").arg(*conflict.resolution->newName);
	return {};
}
