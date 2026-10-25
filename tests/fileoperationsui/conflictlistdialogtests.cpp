// The conflict list dialog: the compact summary and its whole-plan answers, the plan as a tree, bulk answers over a
// selection, renames, rows under a skipped or renamed root, the selection helpers, and the returned resolutions.

#include "fileoperationsui/cconflictlistdialog.h"
#include "fileoperationsui/cconflictlistmodel.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include "qtcore_helpers/catch_qt.hpp" // qtutils

#include <QAction>
#include <QDir>
#include <QFont>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStringBuilder>
#include <QStringList>
#include <QTimer>
#include <QTreeView>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <functional>

namespace
{

QString testPath(const char* relativePath)
{
#ifdef _WIN32
	return QStringLiteral("C:/list-test/") % QLatin1String(relativePath);
#else
	return QStringLiteral("/list-test/") % QLatin1String(relativePath);
#endif
}

EntrySnapshot snapshot(const char* relativePath, const OperationEntryKind kind, const uint64_t size = 0)
{
	const auto path = parseOperationPath(testPath(relativePath));
	REQUIRE(path.has_value());
	return EntrySnapshot{ .path = *path, .kind = kind, .size = size };
}

PlannedConflict conflict(const IssueKind kind, EntrySnapshot source, std::optional<EntrySnapshot> destination,
	const int64_t sourceSeconds = 0, const int64_t destinationSeconds = 0)
{
	PlannedConflict row{ .issue = OperationIssue{ kind, std::move(source), std::move(destination), {} } };
	row.sourceLastWrite = thin_io::timestamp{ .seconds = sourceSeconds };
	row.destinationLastWrite = thin_io::timestamp{ .seconds = destinationSeconds };
	return row;
}

// A merged directory root with three rows below it, and a separate file root. Row 1's destination is older than its
// source, row 2's is newer, row 4's matches its source in size and time.
ConflictPlan mergePlan()
{
	using enum IssueKind;
	using enum OperationEntryKind;
	return {
		conflict(RootDirectoryMerge, snapshot("src", Directory), snapshot("dest/src", Directory), 1000, 2000),
		conflict(FileReplacement, snapshot("src/a.bin", RegularFile, 700), snapshot("dest/src/a.bin", RegularFile, 50), 2000, 1000),
		conflict(FileReplacement, snapshot("src/sub/b.bin", RegularFile, 300), snapshot("dest/src/sub/b.bin", RegularFile, 300), 1000, 2000),
		conflict(TypeMismatch, snapshot("src/c", Directory), snapshot("dest/src/c", RegularFile, 10), 1000, 1000),
		conflict(FileReplacement, snapshot("other.bin", RegularFile, 100), snapshot("dest/other.bin", RegularFile, 100), 5000, 5000),
	};
}

// mergePlan() without the type mismatch: every row is a folder merge or a file replacement.
ConflictPlan replaceablePlan()
{
	ConflictPlan plan = mergePlan();
	plan.erase(plan.begin() + 3);
	return plan;
}

bool shown(CConflictListDialog& dialog, const char* widgetName)
{
	auto* widget = dialog.findChild<QWidget*>(QLatin1String(widgetName));
	REQUIRE(widget != nullptr);
	return widget->isVisibleTo(&dialog);
}

class TestListDialog final : public CConflictListDialog
{
public:
	using CConflictListDialog::CConflictListDialog;

	std::optional<QString> nextName; // What the rename box "types"; nullopt backs out

protected:
	std::optional<QString> askNewName(const QString&) override { return nextName; }
};

void click(CConflictListDialog& dialog, const char* buttonName)
{
	auto* button = dialog.findChild<QPushButton*>(QLatin1String(buttonName));
	REQUIRE(button != nullptr);
	REQUIRE(button->isEnabled());
	button->click();
}

bool buttonEnabled(CConflictListDialog& dialog, const char* buttonName)
{
	auto* button = dialog.findChild<QPushButton*>(QLatin1String(buttonName));
	REQUIRE(button != nullptr);
	return button->isEnabled();
}

void selectPlanRows(CConflictListDialog& dialog, const std::vector<size_t>& planRows)
{
	auto* view = dialog.findChild<QTreeView*>(QStringLiteral("conflictsView"));
	REQUIRE(view != nullptr);
	view->selectionModel()->clearSelection();
	for (const size_t planRow : planRows)
		view->selectionModel()->select(dialog.model().indexOf(planRow), QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

std::vector<size_t> selectedPlanRows(CConflictListDialog& dialog)
{
	auto* view = dialog.findChild<QTreeView*>(QStringLiteral("conflictsView"));
	std::vector<size_t> rows;
	for (const QModelIndex& index : view->selectionModel()->selectedRows())
		rows.push_back(dialog.model().planRow(index));
	std::ranges::sort(rows);
	return rows;
}

void triggerSelectionMenuEntry(CConflictListDialog& dialog, const QString& text)
{
	auto* menu = dialog.findChild<QPushButton*>(QStringLiteral("btnSelect"))->menu();
	REQUIRE(menu != nullptr);
	for (QAction* action : menu->actions())
	{
		if (action->text() == text)
		{
			action->trigger();
			return;
		}
	}
	FAIL("No selection menu entry named " + text.toStdString());
}

// Runs the real modal loop; the interaction is queued so it executes inside exec().
std::optional<ConflictResolutions> askWith(CConflictListDialog& dialog, const std::function<void(CConflictListDialog&)>& interact)
{
	QTimer::singleShot(0, &dialog, [&] { interact(dialog); });
	return dialog.ask();
}

QString cell(const CConflictListModel& model, const size_t planRow, const CConflictListModel::Column column)
{
	return model.data(model.indexOf(planRow, column), Qt::DisplayRole).toString();
}

} // namespace

TEST_CASE("conflict list: the plan is a tree of roots and their descendants", "[conflictlist]")
{
	TestListDialog dialog{ mergePlan(), PromptOperation::Copy };
	const CConflictListModel& model = dialog.model();

	REQUIRE(model.rowCount() == 2); // The directory root and the file root
	const QModelIndex root = model.index(0, 0);
	CHECK(model.rowCount(root) == 3);
	CHECK(model.planRow(model.index(0, 0, root)) == 1);
	CHECK(model.parent(model.index(2, 0, root)) == root);
	CHECK(!model.parent(model.index(1, 0)).isValid());

	CHECK(cell(model, 0, CConflictListModel::Name) == QStringLiteral("src"));
	CHECK(cell(model, 1, CConflictListModel::Name) == QStringLiteral("a.bin"));
	CHECK(cell(model, 2, CConflictListModel::Name) == QDir::toNativeSeparators(QStringLiteral("sub/b.bin")));
	CHECK(cell(model, 0, CConflictListModel::Conflict) == QStringLiteral("Folder exists"));
	CHECK(cell(model, 1, CConflictListModel::Conflict) == QStringLiteral("Destination older"));
	CHECK(cell(model, 2, CConflictListModel::Conflict) == QStringLiteral("Destination newer"));
	CHECK(cell(model, 3, CConflictListModel::Conflict) == QStringLiteral("Type mismatch: folder → file"));
	CHECK(cell(model, 4, CConflictListModel::Conflict) == QStringLiteral("Same size and time"));
	CHECK(model.data(model.indexOf(1, CConflictListModel::SourceModified), Qt::FontRole).value<QFont>().bold()); // The newer side
	CHECK(!model.data(model.indexOf(1, CConflictListModel::DestinationModified), Qt::FontRole).isValid());
	CHECK(cell(model, 1, CConflictListModel::SourceSize).contains(QStringLiteral("700")));
	CHECK(cell(model, 1, CConflictListModel::DestinationSize).contains(QStringLiteral("50")));
	CHECK(cell(model, 0, CConflictListModel::SourceSize).isEmpty()); // A folder has no size
	CHECK(cell(model, 1, CConflictListModel::DestinationModified).endsWith(QStringLiteral(":40"))); // 1000 s past the epoch, to the second
	CHECK(cell(model, 1, CConflictListModel::Action) == QStringLiteral("Choose an action"));
	CHECK(model.data(model.indexOf(1, CConflictListModel::SourceSize), Qt::TextAlignmentRole).value<Qt::Alignment>().testFlag(Qt::AlignRight));

	CHECK(model.unansweredCount() == 5);
	CHECK(dialog.findChild<QLabel*>(QStringLiteral("lblSummary"))->text().contains(QStringLiteral("5")));
	CHECK(!buttonEnabled(dialog, "btnStart"));

	// Only the actions some row allows get a button; nothing in this plan allows MakeWritable.
	CHECK(dialog.findChild<QPushButton*>(QStringLiteral("btnMakeWritable")) == nullptr);
	CHECK(dialog.findChild<QPushButton*>(QStringLiteral("btnMerge")) != nullptr);
}

TEST_CASE("conflict list: actions apply to the selected rows that allow them", "[conflictlist]")
{
	TestListDialog dialog{ mergePlan(), PromptOperation::Copy };
	const CConflictListModel& model = dialog.model();
	click(dialog, "btnShowList");

	CHECK(!buttonEnabled(dialog, "btnReplace")); // Nothing selected
	selectPlanRows(dialog, { 1, 2, 3 });
	CHECK(buttonEnabled(dialog, "btnReplace"));
	CHECK(!buttonEnabled(dialog, "btnMerge")); // No selected row allows it
	click(dialog, "btnReplace");

	CHECK(model.resolution(1)->action == DecisionAction::Replace);
	CHECK(model.resolution(2)->action == DecisionAction::Replace);
	CHECK(!model.resolution(3).has_value()); // A type mismatch cannot be replaced
	CHECK(model.unansweredCount() == 3);
	CHECK(cell(model, 1, CConflictListModel::Action) == QStringLiteral("Replace"));

	triggerSelectionMenuEntry(dialog, QStringLiteral("All"));
	CHECK(selectedPlanRows(dialog) == std::vector<size_t>{ 0, 1, 2, 3, 4 });
	click(dialog, "btnSkip");
	CHECK(model.unansweredCount() == 0);
	CHECK(buttonEnabled(dialog, "btnStart"));

	const auto resolutions = askWith(dialog, [](CConflictListDialog& d) { click(d, "btnStart"); });
	REQUIRE(resolutions.has_value());
	REQUIRE(resolutions->size() == 5);
	for (const size_t rootRow : { size_t{ 0 }, size_t{ 4 } })
	{
		REQUIRE((*resolutions)[rootRow].has_value());
		CHECK((*resolutions)[rootRow]->action == DecisionAction::Skip);
		CHECK((*resolutions)[rootRow]->scope == DecisionScope::ThisItem);
	}
	for (const size_t childRow : { size_t{ 1 }, size_t{ 2 }, size_t{ 3 } })
		CHECK(!(*resolutions)[childRow].has_value()); // Under the skipped root: no answer applies
}

TEST_CASE("conflict list: rows under a skipped or renamed root need no answer", "[conflictlist]")
{
	TestListDialog dialog{ mergePlan(), PromptOperation::Copy };
	const CConflictListModel& model = dialog.model();
	click(dialog, "btnShowList");

	SECTION("a skipped root")
	{
		selectPlanRows(dialog, { 0 });
		click(dialog, "btnSkip");

		CHECK(!model.isApplicable(1));
		CHECK(!model.isApplicable(3));
		CHECK(model.isApplicable(4));
		CHECK(!(model.flags(model.indexOf(1)) & Qt::ItemIsEnabled));
		CHECK(model.unansweredCount() == 1); // other.bin

		selectPlanRows(dialog, { 1, 4 }); // The disabled row cannot be acted on
		click(dialog, "btnSkip");
		CHECK(model.unansweredCount() == 0);

		const auto resolutions = askWith(dialog, [](CConflictListDialog& d) { click(d, "btnStart"); });
		REQUIRE(resolutions.has_value());
		REQUIRE(resolutions->size() == 5);
		CHECK((*resolutions)[0]->action == DecisionAction::Skip);
		CHECK(!(*resolutions)[1].has_value());
		CHECK(!(*resolutions)[2].has_value());
		CHECK(!(*resolutions)[3].has_value());
		CHECK((*resolutions)[4]->action == DecisionAction::Skip);
	}

	SECTION("a renamed root, then merged after all")
	{
		dialog.nextName = QStringLiteral("src2");
		selectPlanRows(dialog, { 0 });
		click(dialog, "btnRename");
		CHECK(model.resolution(0)->action == DecisionAction::Rename);
		CHECK(model.resolution(0)->newName == QStringLiteral("src2"));
		CHECK(!model.isApplicable(1));
		CHECK(model.unansweredCount() == 1);

		click(dialog, "btnMerge");
		CHECK(model.isApplicable(1));
		CHECK(model.unansweredCount() == 4);
	}
}

TEST_CASE("conflict list: renaming", "[conflictlist]")
{
	SECTION("one selected row takes a new name; several cannot")
	{
		TestListDialog dialog{ mergePlan(), PromptOperation::Copy };
		click(dialog, "btnShowList");
		selectPlanRows(dialog, { 1, 2 });
		CHECK(!buttonEnabled(dialog, "btnRename"));

		selectPlanRows(dialog, { 1 });
		dialog.nextName = QStringLiteral("renamed.bin");
		click(dialog, "btnRename");
		REQUIRE(dialog.model().resolution(1).has_value());
		CHECK(dialog.model().resolution(1)->action == DecisionAction::Rename);
		CHECK(dialog.model().resolution(1)->newName == QStringLiteral("renamed.bin"));
		CHECK(cell(dialog.model(), 1, CConflictListModel::Action).contains(QStringLiteral("renamed.bin")));

		dialog.nextName.reset(); // Backing out keeps the answer
		click(dialog, "btnRename");
		CHECK(dialog.model().resolution(1)->newName == QStringLiteral("renamed.bin"));
	}

	SECTION("a re-presented plan: carried answers count, a refused rename does not")
	{
		ConflictPlan plan = mergePlan();
		plan[1].resolution = Decision{ DecisionAction::Replace };
		plan[2].resolution = Decision{ DecisionAction::Rename, DecisionScope::ThisItem, QStringLiteral("taken.bin") };
		plan[2].renameTargetTakenByAnotherItem = true;

		TestListDialog dialog{ std::move(plan), PromptOperation::Copy };
		const CConflictListModel& model = dialog.model();
		CHECK(model.resolution(1)->action == DecisionAction::Replace);
		CHECK(!model.resolution(2).has_value());
		CHECK(cell(model, 2, CConflictListModel::Action).contains(QStringLiteral("taken.bin")));
		CHECK(model.unansweredCount() == 4);
	}
}

TEST_CASE("conflict list: the selection helpers pick rows by how the two sides compare", "[conflictlist]")
{
	TestListDialog dialog{ mergePlan(), PromptOperation::Copy };
	click(dialog, "btnShowList");

	triggerSelectionMenuEntry(dialog, QStringLiteral("Destination older"));
	CHECK(selectedPlanRows(dialog) == std::vector<size_t>{ 1 });

	triggerSelectionMenuEntry(dialog, QStringLiteral("Destination newer"));
	CHECK(selectedPlanRows(dialog) == std::vector<size_t>{ 2 });

	triggerSelectionMenuEntry(dialog, QStringLiteral("Same size and time"));
	CHECK(selectedPlanRows(dialog) == std::vector<size_t>{ 4 });

	triggerSelectionMenuEntry(dialog, QStringLiteral("All: Type mismatch"));
	CHECK(selectedPlanRows(dialog) == std::vector<size_t>{ 3 });
}

TEST_CASE("conflict list: cancelling returns no resolutions", "[conflictlist]")
{
	TestListDialog dialog{ mergePlan(), PromptOperation::Move };
	CHECK(dialog.windowTitle().contains(QStringLiteral("Move")));

	const auto resolutions = askWith(dialog, [](CConflictListDialog& d) { click(d, "btnCancelOperation"); });
	CHECK(!resolutions.has_value());
}

TEST_CASE("conflict list: a fresh plan with existing entries opens compact, others open the list", "[conflictlist]")
{
	SECTION("fresh: the breakdown and the whole-plan buttons")
	{
		TestListDialog dialog{ mergePlan(), PromptOperation::Copy };
		CHECK(shown(dialog, "compactPanel"));
		CHECK(!shown(dialog, "listPanel"));
		CHECK(!shown(dialog, "btnStart"));
		CHECK(shown(dialog, "btnReplaceOlder"));
		CHECK(dialog.findChild<QPushButton*>(QStringLiteral("btnReplaceAll"))->isDefault());

		QStringList texts;
		for (const QLabel* label : dialog.findChild<QWidget*>(QStringLiteral("compactPanel"))->findChildren<QLabel*>())
			texts.push_back(label->text());
		CHECK(texts.contains(QStringLiteral("Destination older")));
		CHECK(texts.contains(QStringLiteral("Folder exists")));
		CHECK(texts.contains(QStringLiteral("Type mismatch")));

		click(dialog, "btnShowList");
		CHECK(!shown(dialog, "compactPanel"));
		CHECK(shown(dialog, "listPanel"));
		CHECK(dialog.findChild<QPushButton*>(QStringLiteral("btnStart"))->isDefault());
		CHECK(dialog.findChild<QPushButton*>(QStringLiteral("btnStart"))->text() == QStringLiteral("Copy"));
	}

	SECTION("re-presented: the answers given in the list")
	{
		ConflictPlan plan = mergePlan();
		plan[1].resolution = Decision{ DecisionAction::Replace };
		TestListDialog dialog{ std::move(plan), PromptOperation::Copy };
		CHECK(shown(dialog, "listPanel"));
		CHECK(!shown(dialog, "btnReplaceAll"));
	}

	SECTION("nothing the whole-plan buttons answer")
	{
		ConflictPlan plan = mergePlan();
		TestListDialog dialog{ ConflictPlan{ plan[3] }, PromptOperation::Copy };
		CHECK(shown(dialog, "listPanel"));
		CHECK(!shown(dialog, "compactPanel"));
	}
}

TEST_CASE("conflict list: the whole-plan buttons merge every folder and start when nothing is left", "[conflictlist]")
{
	SECTION("Replace all")
	{
		TestListDialog dialog{ replaceablePlan(), PromptOperation::Copy };
		const auto resolutions = askWith(dialog, [](CConflictListDialog& d) { click(d, "btnReplaceAll"); });
		REQUIRE(resolutions.has_value());
		REQUIRE(resolutions->size() == 4);
		CHECK((*resolutions)[0]->action == DecisionAction::Merge);
		for (const size_t fileRow : { size_t{ 1 }, size_t{ 2 }, size_t{ 3 } })
			CHECK((*resolutions)[fileRow]->action == DecisionAction::Replace);
	}

	SECTION("Skip all")
	{
		TestListDialog dialog{ replaceablePlan(), PromptOperation::Copy };
		const auto resolutions = askWith(dialog, [](CConflictListDialog& d) { click(d, "btnSkipAll"); });
		REQUIRE(resolutions.has_value());
		REQUIRE(resolutions->size() == 4);
		CHECK((*resolutions)[0]->action == DecisionAction::Merge);
		for (const size_t fileRow : { size_t{ 1 }, size_t{ 2 }, size_t{ 3 } })
			CHECK((*resolutions)[fileRow]->action == DecisionAction::Skip);
	}

	SECTION("only folders: one Merge button")
	{
		ConflictPlan plan = mergePlan();
		TestListDialog dialog{ ConflictPlan{ plan[0] }, PromptOperation::Copy };
		CHECK(dialog.findChild<QPushButton*>(QStringLiteral("btnReplaceAll"))->text() == QStringLiteral("Merge"));
		CHECK(!shown(dialog, "btnReplaceOlder"));
		CHECK(!shown(dialog, "btnSkipAll"));

		const auto resolutions = askWith(dialog, [](CConflictListDialog& d) { click(d, "btnReplaceAll"); });
		REQUIRE(resolutions.has_value());
		CHECK((*resolutions)[0]->action == DecisionAction::Merge);
	}
}

TEST_CASE("conflict list: rows a whole-plan button cannot answer are left selected in the list", "[conflictlist]")
{
	SECTION("Replace all and a type mismatch")
	{
		TestListDialog dialog{ mergePlan(), PromptOperation::Copy };
		const CConflictListModel& model = dialog.model();
		click(dialog, "btnReplaceAll");

		CHECK(dialog.result() != QDialog::Accepted);
		CHECK(shown(dialog, "listPanel"));
		CHECK(model.resolution(0)->action == DecisionAction::Merge);
		CHECK(model.resolution(1)->action == DecisionAction::Replace);
		CHECK(!model.resolution(3).has_value());
		CHECK(selectedPlanRows(dialog) == std::vector<size_t>{ 3 });
		CHECK(!buttonEnabled(dialog, "btnStart"));
	}

	SECTION("Replace older: only the times decide")
	{
		using enum IssueKind;
		using enum OperationEntryKind;
		ConflictPlan plan = replaceablePlan();
		plan.insert(plan.begin() + 3, conflict(FileReplacement, snapshot("src/d.bin", RegularFile, 10), snapshot("dest/src/d.bin", RegularFile, 20), 3000, 3000));
		plan.insert(plan.begin() + 4, conflict(FileReplacement, snapshot("src/e.bin", RegularFile, 10), snapshot("dest/src/e.bin", RegularFile, 10), 3000, 0));

		TestListDialog dialog{ std::move(plan), PromptOperation::Copy };
		const CConflictListModel& model = dialog.model();
		CHECK(cell(model, 3, CConflictListModel::Conflict) == QStringLiteral("Same time, different size"));
		CHECK(cell(model, 4, CConflictListModel::Conflict) == QStringLiteral("Already exists, time unknown"));
		click(dialog, "btnReplaceOlder");

		CHECK(model.resolution(0)->action == DecisionAction::Merge);
		CHECK(model.resolution(1)->action == DecisionAction::Replace); // Destination older
		CHECK(model.resolution(2)->action == DecisionAction::Skip);    // Destination newer
		CHECK(model.resolution(5)->action == DecisionAction::Skip);    // Same size and time
		CHECK(selectedPlanRows(dialog) == std::vector<size_t>{ 3, 4 });
		CHECK(cell(model, 2, CConflictListModel::Action) == QStringLiteral("Skip"));
	}
}
