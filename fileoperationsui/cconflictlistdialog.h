#pragma once

#include "cconflictlistmodel.h"
#include "fileoperationtexts.h" // PromptOperation

#include "fileoperations/fileoperationtypes.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QDialog>
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

// The conflict plan as one list: every row answered before the operation starts. Action buttons apply to the selected
// rows that allow the action; the Select menu picks rows by kind or by how the two sides compare. Start is enabled
// only once every applicable row has an answer. It knows nothing of the job.
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

	void applyActionToSelection(DecisionAction action);
	void renameSelectedRow();
	// Selects the applicable rows the predicate accepts, and nothing else.
	void selectRows(const std::function<bool(const PlannedConflict&)>& predicate);

	void updateSummaryAndButtons();
	[[nodiscard]] std::vector<size_t> selectedPlanRows() const;

	Ui::CConflictListDialog* ui;
	CConflictListModel _model;
	const PromptOperation _operation;
	QMenu* _selectionMenu = nullptr;
	std::vector<std::pair<DecisionAction, QPushButton*>> _actionButtons;
};
