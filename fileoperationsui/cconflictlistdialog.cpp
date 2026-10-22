#include "cconflictlistdialog.h"

#include "fileoperations/newnamecheck.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()


DISABLE_COMPILER_WARNINGS
#include <3rdparty/magic_enum/magic_enum.hpp>

#include "ui_cconflictlistdialog.h"

#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <array>

namespace
{

// Presentation order of the action buttons; only the actions some row of the plan allows get a button.
constexpr std::array plannableActionOrder{ DecisionAction::Replace, DecisionAction::Merge, DecisionAction::MakeWritable, DecisionAction::Rename, DecisionAction::Skip };

QString buttonName(const DecisionAction action)
{
	using enum DecisionAction;
	switch (action)
	{
	case Skip: return QStringLiteral("btnSkip");
	case Replace: return QStringLiteral("btnReplace");
	case Merge: return QStringLiteral("btnMerge");
	case MakeWritable: return QStringLiteral("btnMakeWritable");
	case Rename: return QStringLiteral("btnRename");
	case Retry: return QStringLiteral("btnRetry");
	case Cancel: return QStringLiteral("btnCancel");
	}
	return {};
}

bool actionAllowed(const IssueKind kind, const DecisionAction action)
{
	const AllowedActions plannable = plannableActionsFor(kind);
	return std::ranges::find(plannable, action) != plannable.end();
}

} // namespace

CConflictListDialog::CConflictListDialog(ConflictPlan plan, const PromptOperation operation, QWidget* parent) :
	QDialog{ parent },
	ui{ new Ui::CConflictListDialog },
	_model{ mv(plan), operation },
	_operation{ operation }
{
	ui->setupUi(this);

	switch (_operation)
	{
	case PromptOperation::Copy: setWindowTitle(tr("Copy: conflicts")); break;
	case PromptOperation::Move: setWindowTitle(tr("Move: conflicts")); break;
	case PromptOperation::Delete: setWindowTitle(tr("Delete: conflicts")); break;
	}

	ui->conflictsView->setModel(&_model);
	ui->conflictsView->expandAll();
	ui->conflictsView->header()->setStretchLastSection(true);
	for (int column = 0; column < CConflictListModel::ColumnCount - 1; ++column)
		ui->conflictsView->resizeColumnToContents(column);

	createActionButtons();
	createSelectionMenu();

	connect(ui->conflictsView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &CConflictListDialog::updateSummaryAndButtons);
	connect(ui->btnStart, &QPushButton::clicked, this, &QDialog::accept);
	connect(ui->btnCancelOperation, &QPushButton::clicked, this, &QDialog::reject);
	ui->btnStart->setDefault(true);

	updateSummaryAndButtons();
}

CConflictListDialog::~CConflictListDialog()
{
	delete ui;
}

std::optional<ConflictResolutions> CConflictListDialog::ask()
{
	if (exec() != QDialog::Accepted)
		return {};
	return _model.resolutions();
}

std::optional<QString> CConflictListDialog::askNewName(const QString& currentName)
{
	bool accepted = false;
	const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal, currentName, &accepted);
	if (!accepted)
		return {};
	return name;
}

void CConflictListDialog::createActionButtons()
{
	for (const DecisionAction action : plannableActionOrder)
	{
		bool anyRowAllows = false;
		for (size_t i = 0; i < _model.rowsTotal() && !anyRowAllows; ++i)
			anyRowAllows = actionAllowed(_model.conflict(i).issue.kind, action);
		if (!anyRowAllows)
			continue;

		auto* button = new QPushButton{ fileOperationActionLabel(action, _operation), this };
		button->setObjectName(buttonName(action));
		button->setAutoDefault(false);
		connect(button, &QPushButton::clicked, this, [this, action] {
			if (action == DecisionAction::Rename)
				renameSelectedRow();
			else
				applyActionToSelection(action);
		});
		ui->actionsLayout->addWidget(button);
		_actionButtons.emplace_back(action, button);
	}
}

void CConflictListDialog::createSelectionMenu()
{
	_selectionMenu = new QMenu{ this };
	_selectionMenu->addAction(tr("All"), this, [this] { selectRows([](const PlannedConflict&) { return true; }); });

	// One entry per kind present, in the kinds' declaration order.
	for (const IssueKind kind : magic_enum::enum_values<IssueKind>())
	{
		bool present = false;
		for (size_t i = 0; i < _model.rowsTotal() && !present; ++i)
			present = _model.conflict(i).issue.kind == kind;
		if (present)
			_selectionMenu->addAction(tr("All: %1").arg(fileOperationIssueKindText(kind)), this, [this, kind] {
				selectRows([kind](const PlannedConflict& conflict) { return conflict.issue.kind == kind; });
			});
	}

	bool anyReplacement = false;
	for (size_t i = 0; i < _model.rowsTotal() && !anyReplacement; ++i)
		anyReplacement = _model.conflict(i).issue.kind == IssueKind::FileReplacement;
	if (anyReplacement)
	{
		_selectionMenu->addSeparator();
		_selectionMenu->addAction(tr("Destination older than source"), this, [this] {
			selectRows([](const PlannedConflict& conflict) {
				return conflict.issue.kind == IssueKind::FileReplacement && conflict.sourceLastWrite.is_set() && conflict.destinationLastWrite.is_set()
					&& conflict.destinationLastWrite.seconds < conflict.sourceLastWrite.seconds;
			});
		});
		_selectionMenu->addAction(tr("Destination newer than source"), this, [this] {
			selectRows([](const PlannedConflict& conflict) {
				return conflict.issue.kind == IssueKind::FileReplacement && conflict.sourceLastWrite.is_set() && conflict.destinationLastWrite.is_set()
					&& conflict.destinationLastWrite.seconds > conflict.sourceLastWrite.seconds;
			});
		});
		_selectionMenu->addAction(tr("Same size and time"), this, [this] {
			selectRows([](const PlannedConflict& conflict) {
				return conflict.issue.kind == IssueKind::FileReplacement && conflict.issue.destination
					&& conflict.issue.destination->size == conflict.issue.source.size
					&& conflict.sourceLastWrite.is_set() && conflict.destinationLastWrite == conflict.sourceLastWrite;
			});
		});
	}

	ui->btnSelect->setMenu(_selectionMenu);
}

void CConflictListDialog::applyActionToSelection(const DecisionAction action)
{
	for (const size_t planRow : selectedPlanRows())
	{
		if (actionAllowed(_model.conflict(planRow).issue.kind, action))
			_model.setResolution(planRow, Decision{ action });
	}
	updateSummaryAndButtons();
}

void CConflictListDialog::renameSelectedRow()
{
	const std::vector<size_t> selected = selectedPlanRows();
	assert_and_return_r(selected.size() == 1 && actionAllowed(_model.conflict(selected.front()).issue.kind, DecisionAction::Rename), );

	const size_t planRow = selected.front();
	const QString currentName = _model.conflict(planRow).issue.source.path.name();
	const auto newName = askNewName(currentName);
	if (!newName)
		return;

	if (const NameRejection rejection = checkNewEntryName(*newName); rejection != NameRejection::None)
	{
		QMessageBox::warning(this, tr("Cannot use this name"), newNameRejectionText(rejection));
		return;
	}
	// An unchanged name is not a rename; an exact-case respell of the same name is, hence the case-sensitive compare.
	if (*newName == currentName)
	{
		QMessageBox::warning(this, tr("Nothing to rename"), tr("This is already the entry's name. Enter a different one to rename it."));
		return;
	}

	_model.setResolution(planRow, Decision{ DecisionAction::Rename, DecisionScope::ThisItem, *newName });
	updateSummaryAndButtons();
}

void CConflictListDialog::selectRows(const std::function<bool(const PlannedConflict&)>& predicate)
{
	QItemSelection selection;
	for (size_t i = 0; i < _model.rowsTotal(); ++i)
	{
		if (_model.isApplicable(i) && predicate(_model.conflict(i)))
			selection.select(_model.indexOf(i, 0), _model.indexOf(i, CConflictListModel::ColumnCount - 1));
	}
	ui->conflictsView->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void CConflictListDialog::updateSummaryAndButtons()
{
	const size_t unanswered = _model.unansweredCount();
	ui->lblSummary->setText(tr("%1 conflicts, %2 unanswered").arg(_model.rowsTotal()).arg(unanswered));
	ui->btnStart->setEnabled(unanswered == 0);

	const std::vector<size_t> selected = selectedPlanRows();
	for (const auto& [action, button] : _actionButtons)
	{
		bool enabled = false;
		if (action == DecisionAction::Rename)
			enabled = selected.size() == 1 && actionAllowed(_model.conflict(selected.front()).issue.kind, action);
		else
			enabled = std::ranges::any_of(selected, [this, action](const size_t planRow) { return actionAllowed(_model.conflict(planRow).issue.kind, action); });
		button->setEnabled(enabled);
	}
}

std::vector<size_t> CConflictListDialog::selectedPlanRows() const
{
	std::vector<size_t> rows;
	for (const QModelIndex& index : ui->conflictsView->selectionModel()->selectedRows())
	{
		if (_model.isApplicable(_model.planRow(index)))
			rows.push_back(_model.planRow(index));
	}
	return rows;
}
