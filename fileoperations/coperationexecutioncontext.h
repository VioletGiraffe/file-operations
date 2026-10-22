#pragma once

#include "fileoperationtypes.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"
#include "hash/rapidhash.hpp"


DISABLE_COMPILER_WARNINGS
#include <3rdparty/ankerl/unordered_dense.h>
#include <3rdparty/magic_enum/magic_enum.hpp>

#include <QString>
RESTORE_COMPILER_WARNINGS

#include <array>
#include <chrono>
#include <functional>
#include <vector>

// Timing and arithmetic for Working-phase progress; Scanning snapshots come from the tree builder.
// Receives semantic updates and assembles the snapshots the dialog renders. Time spent paused or
// waiting for a decision is excluded from speed and ETA.
class COperationProgress
{
public:
	explicit COperationProgress(PrimaryProgressUnit primaryUnit) noexcept;

	// Only once every required manifest is known; a partial aggregate must never look exact.
	void setTotals(uint64_t bytesTotal, size_t itemsTotal) noexcept;

	void setCurrentEntry(CEntryPath entry, std::optional<uint64_t> bytesTotal);
	void clearCurrentEntry();
	// Un-counts the current entry's partial progress (a failed attempt whose staging was discarded),
	// so a retry does not double-count the bytes.
	void currentEntryAbandoned() noexcept;

	void fileTransferAdvanced(uint64_t bytes) noexcept;
	void itemCompleted() noexcept;
	// Skipped, already-satisfied, or structurally retained work: advances the processed totals without
	// entering the speed basis.
	void advanceWithoutTransfer(uint64_t bytes, size_t items) noexcept;

	void waitStarted() noexcept;
	void waitEnded() noexcept;

	[[nodiscard]] ProgressSnapshot makeSnapshot() const;

private:
	[[nodiscard]] std::chrono::steady_clock::duration activeDuration() const noexcept;

	const PrimaryProgressUnit _primaryUnit;

	uint64_t _bytesProcessed = 0;
	size_t _itemsProcessed = 0;
	std::optional<uint64_t> _bytesTotal;
	std::optional<size_t> _itemsTotal;

	std::optional<CEntryPath> _currentEntry;
	uint64_t _currentEntryBytesProcessed = 0;
	std::optional<uint64_t> _currentEntryBytesTotal;

	uint64_t _transferredPrimaryUnits = 0; // The speed basis: genuinely performed work only

	std::chrono::steady_clock::time_point _activeSince;
	std::chrono::steady_clock::duration _accumulatedActive{ 0 };
	bool _waiting = false;
};

// The one concrete seam between the synchronous executors and their environment. The job binds the
// callbacks to its mutex/condition-variable/event queue; synchronous tests bind them to scripted providers.
// Owns the remembered-decision table, the progress tracker, and the warning/failure/summary accumulation.
// Not an application-level service: private to the file-operation module.
class COperationExecutionContext
{
public:
	// Returns false when cancellation has won; a pause blocks inside the callback.
	using CheckpointCallback = std::function<bool()>;
	// Presents one decision request; nullopt means cancellation overrode the wait.
	using DecisionCallback = std::function<std::optional<Decision>(const DecisionRequest&)>;
	// Presents the conflict plan; nullopt means cancellation overrode the wait.
	using PlanCallback = std::function<std::optional<ConflictResolutions>(const ConflictPlan&)>;
	using ProgressCallback = std::function<void(const ProgressSnapshot&)>;

	COperationExecutionContext(PrimaryProgressUnit primaryUnit,
		CheckpointCallback checkpoint, DecisionCallback decisionProvider, PlanCallback planPresenter, ProgressCallback progressPublisher);

	// False = cancellation; no new mutation may start.
	[[nodiscard]] bool checkpoint();

	// The one decision entry point. Builds the DecisionRequest from the normative table, consults the
	// remembered table first (only when remainingMatchingScopeAllowed), forwards to the decision provider
	// otherwise, and stores a RemainingMatchingIssues response of a rememberable action.
	// A request that disallows the scope neither consults nor updates remembered state, and a
	// RemainingMatchingIssues response to it is recorded as ThisItem.
	// Decision waits are excluded from progress speed/ETA.
	// nullopt = cancellation overrode the decision.
	// A conflict kind (never ActionFailed) is answered from the planned decisions first, see setPlannedDecisions.
	[[nodiscard]] std::optional<Decision> resolveDecision(OperationIssue issue, bool remainingMatchingScopeAllowed = true);

	// --- Conflict planning ---

	// While recording, a conflict kind without a planned decision is recorded as a new row and answered internally:
	// Merge for RootDirectoryMerge so the planner descends, Skip for every other kind. Remembered decisions are
	// neither consulted nor stored. ActionFailed is forwarded live as always.
	void startConflictRecording();
	// The planned rows consulted and the rows recorded since startConflictRecording, in that encounter order.
	[[nodiscard]] ConflictPlan finishConflictRecording();
	// Only while recording: marks this source's most recent Rename row as taken, so the plan asks for it again.
	void flagRenameTargetTaken(const CEntryPath& source);

	// The rows' resolutions become the planned decisions: one queue per (source path, kind), consumed in order by
	// resolveDecision. Replaces the previous plan. A row without a resolution stores nothing.
	void setPlannedDecisions(ConflictPlan plan);

	// The wait is excluded from progress speed/ETA. nullopt = cancellation overrode the plan.
	[[nodiscard]] std::optional<ConflictResolutions> presentConflictPlan(const ConflictPlan& plan);

	[[nodiscard]] COperationProgress& progress() noexcept { return _progress; }
	void publishProgress(const ProgressSnapshot& snapshot);
	// Publishes the progress tracker's current Working snapshot.
	void publishProgressSnapshot();

	// Bounded accumulation: every call counts, only the first few diagnostics are retained verbatim.
	void recordWarning(OperationDiagnostic diagnostic);
	void recordFailure(OperationDiagnostic diagnostic);

	void addCompletedItems(const size_t count) noexcept { _accumulatedSummary.completedItems += count; }
	void addSkippedItems(const size_t count) noexcept { _accumulatedSummary.skippedItems += count; }
	void addAlreadySatisfiedItems(const size_t count) noexcept { _accumulatedSummary.alreadySatisfiedItems += count; }
	void addTransferredBytes(const uint64_t bytes) noexcept { _accumulatedSummary.transferredBytes += bytes; }

	// Failed items are counted by recordFailure: every originating failed item records exactly one
	// terminal diagnostic, and propagated parent outcomes record none.
	[[nodiscard]] OperationSummary makeSummary(CompletionStatus status) const;

private:
	struct PlannedDecisionKey
	{
		QString sourcePath;
		IssueKind kind;

		[[nodiscard]] bool operator==(const PlannedDecisionKey&) const noexcept = default;
	};

	struct PlannedDecisionKeyHash
	{
		using is_avalanching = std::true_type;
		[[nodiscard]] size_t operator()(const PlannedDecisionKey& key) const noexcept
		{
			return ::rapidhash64(key.sourcePath.constData(), static_cast<size_t>(key.sourcePath.size()) * sizeof(QChar), static_cast<uint64_t>(key.kind));
		}
	};

	// Rows of _plannedRows with a resolution, in plan order; consumed from the front.
	struct PlannedDecisionQueue
	{
		std::vector<size_t> rows;
		size_t nextRow = 0;
	};

	[[nodiscard]] std::optional<Decision> takePlannedDecision(const OperationIssue& issue);
	void recordConflict(OperationIssue issue);

	CheckpointCallback _checkpoint;
	DecisionCallback _decisionProvider;
	PlanCallback _planPresenter;
	ProgressCallback _progressPublisher;

	COperationProgress _progress;

	// Keyed by IssueKind; only actions rememberable per the normative table are ever stored.
	std::array<std::optional<DecisionAction>, magic_enum::enum_count<IssueKind>()> _rememberedDecisions;

	ConflictPlan _plannedRows;
	ankerl::unordered_dense::map<PlannedDecisionKey, PlannedDecisionQueue, PlannedDecisionKeyHash> _plannedDecisions;
	bool _recording = false;
	ConflictPlan _recordedPlan;

	OperationSummary _accumulatedSummary;
};
