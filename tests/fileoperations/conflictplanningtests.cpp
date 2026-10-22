// Up-front conflict planning: the plan's contents, the re-plan rounds renames cause, and how execution applies the
// planned answers to what it finds.

#include "fileoperations/ctransferexecutor.h"
#include "fileoperations/coperationexecutioncontext.h"
#include "fileoperations/operationtesthooks.h"

#include "fileoperationtesthelpers.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QTemporaryDir>
RESTORE_COMPILER_WARNINGS

#ifndef _WIN32
#include <errno.h>
#endif

using OperationTestHooks::CFaultHookScope;
using OperationTestHooks::Point;

namespace
{

#ifdef _WIN32
constexpr NativeErrorCode crossDeviceCode = ERROR_NOT_SAME_DEVICE;
#else
constexpr NativeErrorCode crossDeviceCode = EXDEV;
#endif

Decision renameTo(const QString& newName)
{
	return Decision{ DecisionAction::Rename, DecisionScope::ThisItem, newName };
}

} // namespace

TEST_CASE("conflict planning: plannable actions", "[planning]")
{
	using enum DecisionAction;
	CHECK(plannableActionsFor(IssueKind::FileReplacement) == AllowedActions{ Replace, Rename, Skip });
	CHECK(plannableActionsFor(IssueKind::RootDirectoryMerge) == AllowedActions{ Merge, Rename, Skip });
	CHECK(plannableActionsFor(IssueKind::TypeMismatch) == AllowedActions{ Rename, Skip });
	CHECK(plannableActionsFor(IssueKind::ActionFailed) == AllowedActions{ Skip });
	CHECK(plannableActionsFor(IssueKind::ReadOnlySourceRemoval) == AllowedActions{ MakeWritable, Skip });
	CHECK(plannableActionsFor(IssueKind::UnsupportedEntry) == AllowedActions{ Skip });
}

TEST_CASE("conflict planning: a planned answer applies to the same situation only", "[planning]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	writeTestFile(base % "/a.bin", patternedContents(300));
	REQUIRE(QDir{}.mkpath(base % "/dest"));
	writeTestFile(base % "/dest/a.bin", QByteArray{ "OLD" });

	OperationScript script;

	SECTION("the row carries both entries and their times")
	{
		script.decisions = { act(DecisionAction::Replace) };
		const auto summary = runCopy(script, { base % "/a.bin" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		REQUIRE(script.seenPlans.size() == 1);
		REQUIRE(script.seenPlans[0].size() == 1);
		const PlannedConflict& row = script.seenPlans[0][0];
		CHECK(row.issue.kind == IssueKind::FileReplacement);
		CHECK(row.issue.source.path == ep(base % "/a.bin"));
		REQUIRE(row.issue.destination.has_value());
		CHECK(row.issue.destination->path == ep(base % "/dest/a.bin"));
		CHECK(row.sourceLastWrite.seconds == entryLastWriteSeconds(base % "/a.bin"));
		CHECK(row.destinationLastWrite.seconds == entryLastWriteSeconds(base % "/dest/a.bin"));
		CHECK(!row.resolution.has_value());
		CHECK(!row.renameTargetTakenByAnotherItem);
		CHECK(readFileContents(base % "/dest/a.bin") == patternedContents(300));
	}

	SECTION("a destination gone by execution time is copied without a question")
	{
		script.answerPlan = [&](const ConflictPlan&) {
			REQUIRE(QFile::remove(base % "/dest/a.bin"));
			return ConflictResolutions{ act(DecisionAction::Skip) };
		};
		const auto summary = runCopy(script, { base % "/a.bin" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 1);
		CHECK(summary.skippedItems == 0);
		CHECK(script.seenRequests.empty());
		CHECK(readFileContents(base % "/dest/a.bin") == patternedContents(300));
	}

	SECTION("a collision of another kind by execution time prompts live")
	{
		script.answerPlan = [&](const ConflictPlan&) {
			REQUIRE(QFile::remove(base % "/dest/a.bin"));
			REQUIRE(QDir{}.mkpath(base % "/dest/a.bin"));
			return ConflictResolutions{ act(DecisionAction::Replace) };
		};
		script.decisions = { act(DecisionAction::Skip) };
		const auto summary = runCopy(script, { base % "/a.bin" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.skippedItems == 1);
		REQUIRE(script.seenRequests.size() == 1);
		CHECK(script.seenRequests[0].issue.kind == IssueKind::TypeMismatch);
		CHECK(QFileInfo{ base % "/dest/a.bin" }.isDir());
	}

	SECTION("cancelling at the plan writes nothing")
	{
		script.cancelInsteadOfAnswering = true;
		const auto summary = runCopy(script, { base % "/a.bin" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Cancelled);
		CHECK(summary.completedItems == 0);
		CHECK(script.seenPlans.size() == 1);
		CHECK(readFileContents(base % "/dest/a.bin") == QByteArray{ "OLD" });
	}
}

TEST_CASE("conflict planning: a rename is re-planned", "[planning]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	writeTestFile(base % "/a.bin", patternedContents(300));
	REQUIRE(QDir{}.mkpath(base % "/dest"));
	writeTestFile(base % "/dest/a.bin", QByteArray{ "OLD A" });

	OperationScript script;

	SECTION("a rename onto a free name needs no second round")
	{
		script.decisions = { renameTo(QStringLiteral("free.bin")) };
		const auto summary = runCopy(script, { base % "/a.bin" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 1);
		CHECK(script.seenPlans.size() == 1); // The re-plan found nothing new, so nothing was presented again
		CHECK(script.seenRequests.empty());
		CHECK(readFileContents(base % "/dest/free.bin") == patternedContents(300));
		CHECK(readFileContents(base % "/dest/a.bin") == QByteArray{ "OLD A" });
	}

	SECTION("a rename onto an occupied name is a second row for the same source, and both answers apply in order")
	{
		writeTestFile(base % "/dest/b.bin", QByteArray{ "OLD B" });

		script.decisions = { renameTo(QStringLiteral("b.bin")), act(DecisionAction::Replace) };
		const auto summary = runCopy(script, { base % "/a.bin" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 1);
		REQUIRE(script.seenPlans.size() == 2);
		REQUIRE(script.seenPlans[1].size() == 2);
		REQUIRE(script.seenPlans[1][0].resolution.has_value());
		CHECK(script.seenPlans[1][0].resolution->action == DecisionAction::Rename);
		CHECK(!script.seenPlans[1][1].resolution.has_value());
		CHECK(script.seenPlans[1][1].issue.kind == IssueKind::FileReplacement);
		CHECK(script.seenPlans[1][1].issue.source.path == ep(base % "/a.bin"));
		REQUIRE(script.seenPlans[1][1].issue.destination.has_value());
		CHECK(script.seenPlans[1][1].issue.destination->path == ep(base % "/dest/b.bin"));
		CHECK(script.seenRequests.empty());
		CHECK(readFileContents(base % "/dest/b.bin") == patternedContents(300));
		CHECK(readFileContents(base % "/dest/a.bin") == QByteArray{ "OLD A" });
	}

	SECTION("a renamed root drops the rows planned under its assumed merge")
	{
		REQUIRE(QDir{}.mkpath(base % "/src"));
		writeTestFile(base % "/src/c.bin", patternedContents(100));
		REQUIRE(QDir{}.mkpath(base % "/dest/src"));
		writeTestFile(base % "/dest/src/c.bin", QByteArray{ "OLD C" });

		script.answerPlan = [&](const ConflictPlan& plan) {
			REQUIRE(plan.size() == 2); // The root merge, and c.bin under the assumed merge
			CHECK(plan[0].issue.kind == IssueKind::RootDirectoryMerge);
			CHECK(plan[1].issue.kind == IssueKind::FileReplacement);
			return ConflictResolutions{ renameTo(QStringLiteral("src2")), act(DecisionAction::Replace) };
		};
		const auto summary = runCopy(script, { base % "/src" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 2);
		CHECK(script.seenPlans.size() == 1); // Nothing below the fresh destination collides, so the re-plan presents nothing
		CHECK(readFileContents(base % "/dest/src2/c.bin") == patternedContents(100));
		CHECK(readFileContents(base % "/dest/src/c.bin") == QByteArray{ "OLD C" });
	}
}

TEST_CASE("conflict planning: a rename onto a name another item takes is refused", "[planning]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	REQUIRE(QDir{}.mkpath(base % "/src"));
	writeTestFile(base % "/src/a.bin", patternedContents(100));
	writeTestFile(base % "/src/b.bin", patternedContents(200));
	REQUIRE(QDir{}.mkpath(base % "/dest/src"));
	writeTestFile(base % "/dest/src/a.bin", QByteArray{ "OLD A" });

	OperationScript script;

	SECTION("taken by an unrenamed sibling")
	{
		script.decisions = { act(DecisionAction::Merge), renameTo(QStringLiteral("b.bin")), renameTo(QStringLiteral("c.bin")) };
		const auto summary = runCopy(script, { base % "/src" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 3);
		REQUIRE(script.seenPlans.size() == 2);
		const ConflictPlan& refused = script.seenPlans[1];
		REQUIRE(refused.size() == 2);
		CHECK(!refused[0].renameTargetTakenByAnotherItem);
		CHECK(refused[1].issue.source.path == ep(base % "/src/a.bin"));
		CHECK(refused[1].renameTargetTakenByAnotherItem);
		REQUIRE(refused[1].resolution.has_value());
		CHECK(refused[1].resolution->newName == QStringLiteral("b.bin"));
		CHECK(script.seenRequests.empty());
		CHECK(readFileContents(base % "/dest/src/c.bin") == patternedContents(100));
		CHECK(readFileContents(base % "/dest/src/b.bin") == patternedContents(200));
		CHECK(readFileContents(base % "/dest/src/a.bin") == QByteArray{ "OLD A" });
	}

	SECTION("taken by another rename")
	{
		writeTestFile(base % "/dest/src/b.bin", QByteArray{ "OLD B" });

		// Both collide; both renamed to the same new name - the second rename is the one refused.
		script.decisions = { act(DecisionAction::Merge), renameTo(QStringLiteral("x.bin")), renameTo(QStringLiteral("x.bin")), renameTo(QStringLiteral("y.bin")) };
		const auto summary = runCopy(script, { base % "/src" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 3);
		REQUIRE(script.seenPlans.size() == 2);
		const ConflictPlan& refused = script.seenPlans[1];
		REQUIRE(refused.size() == 3);
		CHECK(!refused[1].renameTargetTakenByAnotherItem);
		CHECK(refused[2].renameTargetTakenByAnotherItem);
		// Listing order decides which file is planned first and keeps x.bin.
		CHECK(readFileContents(base % "/dest/src/x.bin") == readFileContents(refused[1].issue.source.path.value()));
		CHECK(readFileContents(base % "/dest/src/y.bin") == readFileContents(refused[2].issue.source.path.value()));
	}

	SECTION("taken by another root")
	{
		script.decisions = { renameTo(QStringLiteral("b.bin")), renameTo(QStringLiteral("c.bin")) };
		const auto summary = runCopy(script, { base % "/src/a.bin", base % "/src/b.bin" }, DestinationIntent::IntoDirectory, base % "/dest/src");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 2);
		REQUIRE(script.seenPlans.size() == 2);
		REQUIRE(script.seenPlans[1].size() == 1);
		CHECK(script.seenPlans[1][0].renameTargetTakenByAnotherItem);
		CHECK(readFileContents(base % "/dest/src/c.bin") == patternedContents(100));
		CHECK(readFileContents(base % "/dest/src/b.bin") == patternedContents(200));
	}
}

TEST_CASE("conflict planning: rows under a skipped root need no answer", "[planning]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	REQUIRE(QDir{}.mkpath(base % "/src"));
	writeTestFile(base % "/src/a.bin", patternedContents(100));
	REQUIRE(QDir{}.mkpath(base % "/dest/src"));
	writeTestFile(base % "/dest/src/a.bin", QByteArray{ "OLD A" });

	OperationScript script;
	script.answerPlan = [](const ConflictPlan& plan) {
		REQUIRE(plan.size() == 2);
		return ConflictResolutions{ act(DecisionAction::Skip), std::nullopt };
	};
	const auto summary = runCopy(script, { base % "/src" }, DestinationIntent::IntoDirectory, base % "/dest");

	CHECK(summary.status == CompletionStatus::Completed);
	CHECK(summary.skippedItems == 2);
	CHECK(summary.completedItems == 0);
	CHECK(script.seenRequests.empty());
	CHECK(readFileContents(base % "/dest/src/a.bin") == QByteArray{ "OLD A" });
}

TEST_CASE("conflict planning: a move plans the questions of its copy-based path", "[planning]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	REQUIRE(QDir{}.mkpath(base % "/dest"));

	SECTION("roots moved by rename complete during planning; a cancelled plan leaves the rest untouched")
	{
		writeTestFile(base % "/free.bin", patternedContents(100));
		writeTestFile(base % "/taken.bin", patternedContents(200));
		writeTestFile(base % "/dest/taken.bin", QByteArray{ "OLD" });

		OperationScript script;
		script.answerPlan = [&](const ConflictPlan& plan) -> std::optional<ConflictResolutions> {
			REQUIRE(plan.size() == 1);
			CHECK(plan[0].issue.source.path == ep(base % "/taken.bin"));
			CHECK(entryAbsent(base % "/free.bin")); // Already moved
			return std::nullopt; // Cancelled at the plan
		};
		const auto summary = runMove(script, { base % "/free.bin", base % "/taken.bin" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Cancelled);
		CHECK(summary.completedItems == 1);
		CHECK(readFileContents(base % "/dest/free.bin") == patternedContents(100));
		CHECK(readFileContents(base % "/taken.bin") == patternedContents(200));
		CHECK(readFileContents(base % "/dest/taken.bin") == QByteArray{ "OLD" });
	}

	SECTION("a read-only source is planned where the move is known to cross volumes")
	{
		if (readOnlySemanticsUnavailable())
			return;

		REQUIRE(QDir{}.mkpath(base % "/src"));
		writeTestFile(base % "/src/ro.bin", patternedContents(100));
		writeTestFile(base % "/src/rw.bin", patternedContents(200));
		setFileReadOnly(base % "/src/ro.bin", true);

		CFaultHookScope hooks;
		hooks.forceNativeError(Point::RenameEntry_Native, crossDeviceCode); // The root's optimistic rename

		OperationScript script{ .decisions = { act(DecisionAction::MakeWritable) } };
		const auto summary = runMove(script, { base % "/src" }, DestinationIntent::IntoDirectory, base % "/dest");

		CHECK(summary.status == CompletionStatus::Completed);
		CHECK(summary.completedItems == 3);
		CHECK(script.seenRequests.empty());
		REQUIRE(script.seenPlans.size() == 1);
		REQUIRE(script.seenPlans[0].size() == 1);
		CHECK(script.seenPlans[0][0].issue.kind == IssueKind::ReadOnlySourceRemoval);
		CHECK(script.seenPlans[0][0].issue.source.path == ep(base % "/src/ro.bin"));
		CHECK(entryAbsent(base % "/src"));
		CHECK(readFileContents(base % "/dest/src/ro.bin") == patternedContents(100));

		setFileReadOnly(base % "/dest/src/ro.bin", false);
	}
}
