#pragma once

#include "cconflictlistmodel.h"
#include "fileoperationtexts.h" // PromptOperation

#include "fileoperations/fileoperationtypes.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QDialog>
#include <QSize>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace Ui {
class CConflictListDialog;
}

class QMenu;
class QPushButton;

// The conflict plan: every row answered before the operation starts. It knows nothing of the job.
// A fresh plan with existing files or folders opens compact: a breakdown by kind and comparison, and buttons that answer
// the whole plan and start. Rows such a button cannot answer are left selected in the list.
// In the list, action buttons apply to the selected rows that allow the action; the Select menu picks rows by kind or
// by how the two sides compare. Start is enabled only once every applicable row has an answer.
class CConflictListDialog : public QDialog
{
public:
	CConflictListDialog(ConflictPlan plan, PromptOperation operation, QWidget* parent = nullptr);
	~CConflictListDialog() override;

	CConflictListDialog(const CConflictListDialog&) = delete;
	CConflictListDialog& operator=(const CConflictListDialog&) = delete;

	// Runs the modal loop. nullopt cancels the operation: the Cancel button, or closing the dialog by any other path.
	[[nodiscard]] std::optional<ConflictResolutions> ask();

	[[nodiscard]] CConflictListModel& model() noexcept { return _model; }

protected:
	// The new name for the one selected row; nullopt = backed out. A test overrides this to answer without a modal box.
	[[nodiscard]] virtual std::optional<QString> askNewName(const QString& currentName);

private:
	void createActionButtons();
	void createSelectionMenu();
	void createBreakdown();

	// Merges every folder and gives every file replacement fileAnswer's action; nullopt leaves the row unanswered.
	// Starts when no row is left unanswered, otherwise shows the list with those rows selected.
	void answerAllAndStart(const std::function<std::optional<DecisionAction>(const PlannedConflict&)>& fileAnswer);
	void showList();

	void applyActionToSelection(DecisionAction action);
	void renameSelectedRow();
	// Selects the applicable rows the predicate accepts, and nothing else, and scrolls to the first.
	void selectRows(const std::function<bool(size_t planRow)>& predicate);

	void updateSummaryAndButtons();
	[[nodiscard]] std::vector<size_t> selectedPlanRows() const;
	[[nodiscard]] bool anyPlanRow(const std::function<bool(size_t planRow)>& predicate) const;

	Ui::CConflictListDialog* ui;
	CConflictListModel _model;
	const PromptOperation _operation;
	QSize _listSize; // The list's initial size, from the form
	QMenu* _selectionMenu = nullptr;
	std::vector<std::pair<DecisionAction, QPushButton*>> _actionButtons;
};
