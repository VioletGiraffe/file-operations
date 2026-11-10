#include "cfileoperationprompt.h"
#include "usermessagebox.h"

#include "fileoperations/fileoperationtexts.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include "ui_cfileoperationprompt.h"

#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <optional>

namespace
{

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

} // namespace

CFileOperationPrompt::CFileOperationPrompt(const DecisionRequest& request, const PromptOperation operation, QWidget* parent) :
	QDialog{ parent },
	ui{ new Ui::CFileOperationPrompt },
	_request{ request },
	_operation{ operation }
{
	ui->setupUi(this);

	setWindowTitle(fileOperationName(_operation));
	ui->lblQuestion->setText(fileOperationIssueText(_request.issue, _operation));
	setupEntryInfo();
	setupAuxiliaryTexts();
	createActionButtons();
	if (_renameButton)
		updateRenameControls();
	setInitialFocus();
}

CFileOperationPrompt::~CFileOperationPrompt()
{
	delete ui;
}

Decision CFileOperationPrompt::ask()
{
	adjustSize();
	exec();
	return _decision;
}

void CFileOperationPrompt::onActionChosen(const DecisionAction action)
{
	if (action == DecisionAction::Rename && !renameNameAccepted())
		return; // Reason given; the prompt stays open on the text the user typed

	const bool remember = !ui->scopeCheckBox->isHidden() && ui->scopeCheckBox->isChecked()
		&& isActionRememberable(_request.issue.kind, action);
	_decision = Decision{ .action = action, .scope = remember ? DecisionScope::RemainingMatchingIssues : DecisionScope::ThisItem, .newName = {} };
	if (action == DecisionAction::Rename)
		_decision.newName = ui->renameEdit->text();
	accept();
}

bool CFileOperationPrompt::renameNameAccepted()
{
	const auto refusal = renameRefusal(ui->renameEdit->text(), _request.issue.source.path.name());
	if (refusal)
		showUserMessage(this, *refusal);
	return !refusal;
}

void CFileOperationPrompt::updateRenameControls()
{
	// Enter follows the intent expressed in the edit: a changed name makes Rename the default, even one it cannot use, so that
	// Enter explains the refusal and never runs the kind's own default instead. The unchanged name restores the kind's default,
	// which may be none.
	// The button stays enabled either way: a refusal the user can read beats a dead control they cannot explain.
	const bool nameChanged = ui->renameEdit->text() != _request.issue.source.path.name();
	_renameButton->setDefault(nameChanged);
	if (!nameChanged && _defaultButton)
		_defaultButton->setDefault(true);
}

void CFileOperationPrompt::createActionButtons()
{
	const std::optional<DecisionAction> defaultAction = defaultActionFor(_request.issue.kind);
	for (const DecisionAction action : _request.allowedActions)
	{
		auto* button = new QPushButton{ fileOperationActionLabel(action, _operation), this };
		button->setObjectName(buttonName(action));
		button->setAutoDefault(false); // Enter does nothing in a prompt without a default action
		button->setDefault(defaultAction && action == *defaultAction);
		connect(button, &QPushButton::clicked, this, [this, action] { onActionChosen(action); });
		ui->buttonsLayout->addWidget(button);
		if (action == DecisionAction::Rename)
			_renameButton = button;
		if (button->isDefault())
			_defaultButton = button;
	}
}

void CFileOperationPrompt::setInitialFocus()
{
	// Chosen, not inherited from the tab order. The rename field is the only control that expects typing, so
	// it takes focus where it exists, with the old name selected for replacement. Otherwise Skip: it changes
	// nothing, and it keeps a mutating button - or the scope box, which silently broadens the next decision
	// to every remaining item - from being one Space away.
	if (!ui->renameEdit->isHidden())
	{
		ui->renameEdit->setFocus();
		ui->renameEdit->selectAll();
		return;
	}

	auto* skipButton = findChild<QPushButton*>(buttonName(DecisionAction::Skip));
	skipButton->setFocus(); // Every issue kind offers Skip
}

void CFileOperationPrompt::setupEntryInfo()
{
	const OperationIssue& issue = _request.issue;

	ui->lblSourcePath->setText(QDir::toNativeSeparators(issue.source.path.value()));
	ui->lblSourceDetails->setText(fileOperationEntryDetailsText(issue.source));

	if (issue.destination)
	{
		ui->lblDestinationPath->setText(QDir::toNativeSeparators(issue.destination->path.value()));
		ui->lblDestinationDetails->setText(fileOperationEntryDetailsText(*issue.destination));
	}
	else
	{
		ui->lblDestinationCaption->hide();
		ui->lblDestinationPath->hide();
		ui->lblDestinationDetails->hide();
	}
}

void CFileOperationPrompt::setupAuxiliaryTexts()
{
	const OperationIssue& issue = _request.issue;

	const auto setTextOrHide = [](QLabel* label, const QString& text) {
		if (text.isEmpty())
			label->hide();
		else
			label->setText(text);
	};
	setTextOrHide(ui->lblFailure, fileOperationIssueFailureText(issue));
	setTextOrHide(ui->lblConsequences, fileOperationCancelConsequencesText(_request));

	if (remainingMatchingScopeOffered(_request))
		ui->scopeCheckBox->setText(fileOperationScopeLabel(issue.kind));
	else
		ui->scopeCheckBox->hide();

	if (std::ranges::find(_request.allowedActions, DecisionAction::Rename) != _request.allowedActions.end())
	{
		ui->renameEdit->setText(issue.source.path.name());
		connect(ui->renameEdit, &QLineEdit::textChanged, this, &CFileOperationPrompt::updateRenameControls);
	}
	else
	{
		ui->lblRenameCaption->hide();
		ui->renameEdit->hide();
	}
}
