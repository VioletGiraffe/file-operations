#include "cconflictlistdialog.h"

#include "fileoperations/newnamecheck.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()
#include "utils/displayformatting.h"
#include "utils/filemanager.h"


DISABLE_COMPILER_WARNINGS
#include <3rdparty/magic_enum/magic_enum.hpp>

#include "ui_cconflictlistdialog.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QRect>
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
	case PromptOperation::Copy:
		setWindowTitle(tr("Copy: conflicts"));
		ui->btnStart->setText(tr("Copy"));
		break;
	case PromptOperation::Move:
		setWindowTitle(tr("Move: conflicts"));
		ui->btnStart->setText(tr("Move"));
		break;
	case PromptOperation::Delete:
		setWindowTitle(tr("Delete: conflicts"));
		ui->btnStart->setText(tr("Delete"));
		break;
	}

	ui->conflictsView->setModel(&_model);
	ui->conflictsView->expandAll();
	// Name takes the width left by the other columns, so a deep relative path cannot push them out of view
	ui->conflictsView->setTextElideMode(Qt::ElideMiddle);
	ui->conflictsView->header()->setStretchLastSection(false);
	ui->conflictsView->header()->setSectionResizeMode(CConflictListModel::Name, QHeaderView::Stretch);
	for (int column = CConflictListModel::Name + 1; column < CConflictListModel::ColumnCount; ++column)
		ui->conflictsView->resizeColumnToContents(column);

	createActionButtons();
	createSelectionMenu();

	connect(ui->conflictsView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &CConflictListDialog::updateSummaryAndButtons);
	ui->conflictsView->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui->conflictsView, &QWidget::customContextMenuRequested, this, [this](const QPoint& viewportPosition) {
		const QModelIndex index = ui->conflictsView->indexAt(viewportPosition);
		if (!index.isValid())
			return;
		QMenu menu{ this };
		addRowActions(menu, _model.planRow(index));
		menu.exec(ui->conflictsView->viewport()->mapToGlobal(viewportPosition));
	});
	connect(ui->btnStart, &QPushButton::clicked, this, &QDialog::accept);
	connect(ui->btnCancelOperation, &QPushButton::clicked, this, &QDialog::reject);
	connect(ui->btnShowList, &QPushButton::clicked, this, &CConflictListDialog::showList);
	connect(ui->btnReplaceAll, &QPushButton::clicked, this, [this] {
		answerAllAndStart([](const PlannedConflict&) { return DecisionAction::Replace; });
	});
	connect(ui->btnReplaceOlder, &QPushButton::clicked, this, [this] {
		answerAllAndStart([](const PlannedConflict& conflict) -> std::optional<DecisionAction> {
			// Only the times decide: an unknown time, or the same time with a different size, is left for the user
			switch (compareSides(conflict))
			{
			case SideComparison::DestinationOlder: return DecisionAction::Replace;
			case SideComparison::DestinationNewer:
			case SideComparison::SameSizeAndTime: return DecisionAction::Skip;
			case SideComparison::SizeDiffers:
			case SideComparison::TimeUnknown: return {};
			}
			return {};
		});
	});
	connect(ui->btnSkipAll, &QPushButton::clicked, this, [this] {
		answerAllAndStart([](const PlannedConflict&) { return DecisionAction::Skip; });
	});

	updateSummaryAndButtons();

	_listSize = size();
	const auto planHasKind = [this](const IssueKind kind) {
		return anyPlanRow([this, kind](const size_t planRow) { return _model.conflict(planRow).issue.kind == kind; });
	};
	const bool anyReplacement = planHasKind(IssueKind::FileReplacement);
	// A re-presented plan carries answers given in the list, so it reopens there
	const bool represented = anyPlanRow([this](const size_t planRow) { return _model.conflict(planRow).resolution.has_value(); });
	if ((!anyReplacement && !planHasKind(IssueKind::RootDirectoryMerge)) || represented)
	{
		showList();
		return;
	}

	createBreakdown();
	ui->listPanel->hide();
	ui->btnStart->hide();
	if (!anyReplacement)
	{
		ui->btnReplaceAll->setText(tr("Merge"));
		ui->btnReplaceOlder->hide();
		ui->btnSkipAll->hide();
	}
	ui->btnReplaceAll->setDefault(true);
	ui->btnReplaceAll->setFocus(); // An autoDefault button with focus would take Enter instead
	adjustSize();
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

void CConflictListDialog::addRowActions(QMenu& menu, const size_t planRow)
{
	const auto addEntryActions = [&](const CEntryPath& path, const QString& revealText, const QString& copyText) {
		const QString nativePath = QDir::toNativeSeparators(path.value());
		menu.addAction(revealText, this, [this, nativePath] {
			// The plan lists a link as an entry of its own
			if (!revealInFileManager(nativePath, LinkHandling::NoFollow))
				QMessageBox::warning(this, tr("File not found"), tr("This file or folder no longer exists:\n%1").arg(nativePath));
		});
		menu.addAction(copyText, this, [nativePath] { QGuiApplication::clipboard()->setText(nativePath); });
	};

	const OperationIssue& issue = _model.conflict(planRow).issue;
	addEntryActions(issue.source.path,
		textForPlatform<QString>(tr("Open source in Explorer"), tr("Reveal source in Finder"), tr("Show source in file manager")),
		tr("Copy full source path"));
	if (issue.destination)
	{
		menu.addSeparator();
		addEntryActions(issue.destination->path,
			textForPlatform<QString>(tr("Open destination in Explorer"), tr("Reveal destination in Finder"), tr("Show destination in file manager")),
			tr("Copy full destination path"));
	}
}

void CConflictListDialog::createActionButtons()
{
	for (const DecisionAction action : plannableActionOrder)
	{
		if (!anyPlanRow([this, action](const size_t planRow) { return actionAllowed(_model.conflict(planRow).issue.kind, action); }))
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
	_selectionMenu->addAction(tr("All"), this, [this] { selectRows([](size_t) { return true; }); });

	// One entry per kind present, then per comparison present, in declaration order.
	for (const IssueKind kind : magic_enum::enum_values<IssueKind>())
	{
		const auto hasKind = [this, kind](const size_t planRow) { return _model.conflict(planRow).issue.kind == kind; };
		if (anyPlanRow(hasKind))
			_selectionMenu->addAction(tr("All: %1").arg(fileOperationIssueKindText(kind)), this, [this, hasKind] { selectRows(hasKind); });
	}

	bool separatorAdded = false;
	for (const SideComparison comparison : magic_enum::enum_values<SideComparison>())
	{
		const auto comparesAs = [this, comparison](const size_t planRow) {
			const PlannedConflict& conflict = _model.conflict(planRow);
			return conflict.issue.kind == IssueKind::FileReplacement && compareSides(conflict) == comparison;
		};
		if (!anyPlanRow(comparesAs))
			continue;

		if (!separatorAdded)
		{
			_selectionMenu->addSeparator();
			separatorAdded = true;
		}
		_selectionMenu->addAction(sideComparisonText(comparison), this, [this, comparesAs] { selectRows(comparesAs); });
	}

	ui->btnSelect->setMenu(_selectionMenu);
}

void CConflictListDialog::createBreakdown()
{
	std::array<size_t, magic_enum::enum_count<IssueKind>()> kindCounts{};
	std::array<size_t, magic_enum::enum_count<SideComparison>()> comparisonCounts{};
	for (size_t i = 0; i < _model.rowsTotal(); ++i)
	{
		const PlannedConflict& conflict = _model.conflict(i);
		++kindCounts[static_cast<size_t>(conflict.issue.kind)];
		if (conflict.issue.kind == IssueKind::FileReplacement)
			++comparisonCounts[static_cast<size_t>(compareSides(conflict))];
	}

	int gridRow = 0;
	const auto addLine = [&](const size_t count, const QString& text) {
		if (count == 0)
			return;
		auto* countLabel = new QLabel{ QString::number(count), ui->compactPanel };
		countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		ui->breakdownLayout->addWidget(countLabel, gridRow, 0);
		ui->breakdownLayout->addWidget(new QLabel{ text, ui->compactPanel }, gridRow, 1);
		++gridRow;
	};

	for (const SideComparison comparison : magic_enum::enum_values<SideComparison>())
		addLine(comparisonCounts[static_cast<size_t>(comparison)], sideComparisonText(comparison));
	addLine(kindCounts[static_cast<size_t>(IssueKind::RootDirectoryMerge)], fileOperationIssueKindText(IssueKind::RootDirectoryMerge));

	bool headingAdded = false;
	for (const IssueKind kind : magic_enum::enum_values<IssueKind>())
	{
		if (kind == IssueKind::FileReplacement || kind == IssueKind::RootDirectoryMerge || kindCounts[static_cast<size_t>(kind)] == 0)
			continue;

		if (!headingAdded)
		{
			ui->breakdownLayout->addWidget(new QLabel{ tr("Left for you to decide in the list:"), ui->compactPanel }, gridRow++, 0, 1, 2);
			headingAdded = true;
		}
		addLine(kindCounts[static_cast<size_t>(kind)], fileOperationIssueKindText(kind));
	}
	ui->breakdownLayout->setColumnStretch(1, 1);
}

void CConflictListDialog::answerAllAndStart(const std::function<std::optional<DecisionAction>(const PlannedConflict&)>& fileAnswer)
{
	for (size_t i = 0; i < _model.rowsTotal(); ++i)
	{
		const PlannedConflict& conflict = _model.conflict(i);
		std::optional<DecisionAction> action;
		if (conflict.issue.kind == IssueKind::RootDirectoryMerge)
			action = DecisionAction::Merge;
		else if (conflict.issue.kind == IssueKind::FileReplacement)
			action = fileAnswer(conflict);

		if (action)
			_model.setResolution(i, Decision{ *action });
	}

	if (_model.unansweredCount() == 0)
	{
		accept();
		return;
	}

	showList();
	selectRows([this](const size_t planRow) { return !_model.isAnswered(planRow); });
	updateSummaryAndButtons();
}

void CConflictListDialog::showList()
{
	ui->compactPanel->hide();
	ui->btnShowList->hide();
	ui->btnReplaceAll->hide();
	ui->btnReplaceOlder->hide();
	ui->btnSkipAll->hide();
	ui->btnReplaceAll->setDefault(false);
	ui->listPanel->show();
	ui->btnStart->show();
	ui->btnStart->setDefault(true);
	ui->conflictsView->setFocus();

	if (!isVisible())
		return;

	// Grows from compact around the same centre
	const QRect frame = frameGeometry();
	const QSize newSize = _listSize.expandedTo(size());
	const QSize newFrameSize = newSize + frame.size() - size();
	resize(newSize);
	move(frame.center() - QPoint{ newFrameSize.width() / 2, newFrameSize.height() / 2 });
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

void CConflictListDialog::selectRows(const std::function<bool(size_t planRow)>& predicate)
{
	QItemSelection selection;
	for (size_t i = 0; i < _model.rowsTotal(); ++i)
	{
		if (_model.isApplicable(i) && predicate(i))
			selection.select(_model.indexOf(i, 0), _model.indexOf(i, CConflictListModel::ColumnCount - 1));
	}
	ui->conflictsView->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	if (!selection.isEmpty())
		ui->conflictsView->scrollTo(selection.first().topLeft());
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

bool CConflictListDialog::anyPlanRow(const std::function<bool(size_t planRow)>& predicate) const
{
	for (size_t i = 0; i < _model.rowsTotal(); ++i)
	{
		if (predicate(i))
			return true;
	}
	return false;
}
