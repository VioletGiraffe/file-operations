#include "coperationexecutioncontext.h"

#include "cfilesystemmutator.h"

// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()


#include <algorithm>
#include <utility>

namespace
{

// Enough for the completion dialog; the aggregate counts stay exact regardless.
constexpr size_t maxRepresentativeDiagnostics = 16;

} // namespace

COperationProgress::COperationProgress(const PrimaryProgressUnit primaryUnit) noexcept
	: _primaryUnit{ primaryUnit }
	, _activeSince{ std::chrono::steady_clock::now() }
{
}

void COperationProgress::setTotals(const uint64_t bytesTotal, const size_t itemsTotal) noexcept
{
	_bytesTotal = bytesTotal;
	_itemsTotal = itemsTotal;
}

void COperationProgress::setCurrentEntry(CEntryPath entry, const std::optional<uint64_t> bytesTotal)
{
	_currentEntry = mv(entry);
	_currentEntryBytesProcessed = 0;
	_currentEntryBytesTotal = bytesTotal;
}

void COperationProgress::clearCurrentEntry()
{
	_currentEntry.reset();
	_currentEntryBytesProcessed = 0;
	_currentEntryBytesTotal.reset();
}

void COperationProgress::currentEntryAbandoned() noexcept
{
	_bytesProcessed -= _currentEntryBytesProcessed;
	if (_primaryUnit == PrimaryProgressUnit::Bytes)
		_transferredPrimaryUnits -= _currentEntryBytesProcessed;
	_currentEntryBytesProcessed = 0;
}

void COperationProgress::fileTransferAdvanced(const uint64_t bytes) noexcept
{
	_bytesProcessed += bytes;
	_currentEntryBytesProcessed += bytes;
	if (_primaryUnit == PrimaryProgressUnit::Bytes)
		_transferredPrimaryUnits += bytes;
}

void COperationProgress::itemCompleted() noexcept
{
	++_itemsProcessed;
	if (_primaryUnit == PrimaryProgressUnit::Items)
		++_transferredPrimaryUnits;
}

void COperationProgress::advanceWithoutTransfer(const uint64_t bytes, const size_t items) noexcept
{
	_bytesProcessed += bytes;
	_itemsProcessed += items;
}

void COperationProgress::waitStarted() noexcept
{
	assert_debug_only(!_waiting);
	_accumulatedActive += std::chrono::steady_clock::now() - _activeSince;
	_waiting = true;
}

void COperationProgress::waitEnded() noexcept
{
	assert_debug_only(_waiting);
	_activeSince = std::chrono::steady_clock::now();
	_waiting = false;
}

std::chrono::steady_clock::duration COperationProgress::activeDuration() const noexcept
{
	return _waiting ? _accumulatedActive : _accumulatedActive + (std::chrono::steady_clock::now() - _activeSince);
}

ProgressSnapshot COperationProgress::makeSnapshot() const
{
	ProgressSnapshot snapshot;
	snapshot.phase = OperationPhase::Working;
	snapshot.currentEntry = _currentEntry;
	snapshot.bytesProcessed = _bytesProcessed;
	snapshot.bytesTotal = _bytesTotal;
	snapshot.currentEntryBytesProcessed = _currentEntryBytesProcessed;
	snapshot.currentEntryBytesTotal = _currentEntryBytesTotal;
	snapshot.itemsProcessed = _itemsProcessed;
	snapshot.itemsTotal = _itemsTotal;

	using namespace std::chrono;
	const auto activeMs = duration_cast<milliseconds>(activeDuration()).count();
	if (activeMs >= 500) // Too little basis produces absurd spikes, not information
	{
		snapshot.primaryUnitsPerSecond = _transferredPrimaryUnits * 1000 / static_cast<uint64_t>(activeMs);

		const uint64_t totalUnits = _primaryUnit == PrimaryProgressUnit::Bytes
			? _bytesTotal.value_or(0) : static_cast<uint64_t>(_itemsTotal.value_or(0));
		const uint64_t processedUnits = _primaryUnit == PrimaryProgressUnit::Bytes ? _bytesProcessed : _itemsProcessed;
		const bool totalKnown = _primaryUnit == PrimaryProgressUnit::Bytes ? _bytesTotal.has_value() : _itemsTotal.has_value();
		if (totalKnown && snapshot.primaryUnitsPerSecond > 0 && totalUnits > processedUnits)
			snapshot.secondsRemaining = static_cast<uint32_t>((totalUnits - processedUnits) / snapshot.primaryUnitsPerSecond);
	}

	return snapshot;
}

COperationExecutionContext::COperationExecutionContext(const PrimaryProgressUnit primaryUnit,
	CheckpointCallback checkpoint, DecisionCallback decisionProvider, PlanCallback planPresenter, ProgressCallback progressPublisher)
	: _checkpoint{ mv(checkpoint) }
	, _decisionProvider{ mv(decisionProvider) }
	, _planPresenter{ mv(planPresenter) }
	, _progressPublisher{ mv(progressPublisher) }
	, _progress{ primaryUnit }
{
	assert_r(_checkpoint && _decisionProvider && _planPresenter && _progressPublisher);
}

bool COperationExecutionContext::checkpoint()
{
	// A pause blocks inside the callback; that time must not count toward speed/ETA, same as decision waits.
	_progress.waitStarted();
	const bool proceed = _checkpoint();
	_progress.waitEnded();
	return proceed;
}

std::optional<Decision> COperationExecutionContext::resolveDecision(OperationIssue issue, const bool remainingMatchingScopeAllowed)
{
	const auto kindIndex = static_cast<size_t>(issue.kind);
	assert_debug_only(kindIndex < _rememberedDecisions.size());

	if (issue.kind != IssueKind::ActionFailed)
	{
		if (auto planned = takePlannedDecision(issue))
			return planned;
		if (_recording)
		{
			const DecisionAction internalAnswer = issue.kind == IssueKind::RootDirectoryMerge ? DecisionAction::Merge : DecisionAction::Skip;
			recordConflict(mv(issue));
			return Decision{ internalAnswer };
		}
	}

	if (remainingMatchingScopeAllowed)
	{
		if (const auto& remembered = _rememberedDecisions[kindIndex]; remembered)
			return Decision{ *remembered, DecisionScope::RemainingMatchingIssues };
	}

	const IssueKind kind = issue.kind;
	const DecisionRequest request{ mv(issue), allowedActionsFor(kind), remainingMatchingScopeAllowed };

	_progress.waitStarted();
	auto decision = _decisionProvider(request);
	_progress.waitEnded();

	if (!decision)
		return {};

	assert_r(std::find(request.allowedActions.begin(), request.allowedActions.end(), decision->action) != request.allowedActions.end());

	if (decision->scope == DecisionScope::RemainingMatchingIssues)
	{
		if (remainingMatchingScopeAllowed && isActionRememberable(kind, decision->action))
			_rememberedDecisions[kindIndex] = decision->action;
		else
			decision->scope = DecisionScope::ThisItem; // An All response is void here; the action still answers this item
	}

	return decision;
}

std::optional<Decision> COperationExecutionContext::takePlannedDecision(const OperationIssue& issue)
{
	const auto queue = _plannedDecisions.find(PlannedDecisionKey{ issue.source.path.value(), issue.kind });
	if (queue == _plannedDecisions.end() || queue->second.nextRow == queue->second.rows.size())
		return {};

	const PlannedConflict& row = _plannedRows[queue->second.rows[queue->second.nextRow++]];
	if (_recording)
		_recordedPlan.push_back(row);
	return row.resolution;
}

void COperationExecutionContext::recordConflict(OperationIssue issue)
{
	PlannedConflict row{ .issue = mv(issue) };
	row.sourceLastWrite = readLastWriteTime(row.issue.source.path);
	if (row.issue.destination)
		row.destinationLastWrite = readLastWriteTime(row.issue.destination->path);
	_recordedPlan.push_back(mv(row));
}

void COperationExecutionContext::startConflictRecording()
{
	assert_debug_only(!_recording && _recordedPlan.empty());
	_recording = true;
}

ConflictPlan COperationExecutionContext::finishConflictRecording()
{
	assert_debug_only(_recording);
	_recording = false;
	return std::exchange(_recordedPlan, {});
}

void COperationExecutionContext::flagRenameTargetTaken(const CEntryPath& source)
{
	assert_debug_only(_recording);
	const auto row = std::find_if(_recordedPlan.rbegin(), _recordedPlan.rend(), [&source](const PlannedConflict& candidate) {
		return candidate.issue.source.path == source && candidate.resolution && candidate.resolution->action == DecisionAction::Rename;
	});
	assert_and_return_r(row != _recordedPlan.rend(), );
	row->renameTargetTakenByAnotherItem = true;
}

void COperationExecutionContext::setPlannedDecisions(ConflictPlan plan)
{
	_plannedDecisions.clear();
	_plannedRows = mv(plan);
	for (size_t i = 0; i < _plannedRows.size(); ++i)
	{
		const PlannedConflict& row = _plannedRows[i];
		if (!row.resolution)
			continue;

		const AllowedActions plannable = plannableActionsFor(row.issue.kind);
		assert_r(std::ranges::find(plannable, row.resolution->action) != plannable.end() && row.resolution->scope == DecisionScope::ThisItem);
		_plannedDecisions[PlannedDecisionKey{ row.issue.source.path.value(), row.issue.kind }].rows.push_back(i);
	}
}

std::optional<ConflictResolutions> COperationExecutionContext::presentConflictPlan(const ConflictPlan& plan)
{
	_progress.waitStarted();
	auto resolutions = _planPresenter(plan);
	_progress.waitEnded();
	return resolutions;
}

void COperationExecutionContext::publishProgress(const ProgressSnapshot& snapshot)
{
	_progressPublisher(snapshot);
}

void COperationExecutionContext::publishProgressSnapshot()
{
	_progressPublisher(_progress.makeSnapshot());
}

void COperationExecutionContext::recordWarning(OperationDiagnostic diagnostic)
{
	++_accumulatedSummary.warningCount;
	if (_accumulatedSummary.representativeWarnings.size() < maxRepresentativeDiagnostics)
		_accumulatedSummary.representativeWarnings.push_back(mv(diagnostic));
}

void COperationExecutionContext::recordFailure(OperationDiagnostic diagnostic)
{
	++_accumulatedSummary.failedItems;
	if (_accumulatedSummary.representativeFailures.size() < maxRepresentativeDiagnostics)
		_accumulatedSummary.representativeFailures.push_back(mv(diagnostic));
}

OperationSummary COperationExecutionContext::makeSummary(const CompletionStatus status) const
{
	OperationSummary summary = _accumulatedSummary;
	summary.status = status;
	return summary;
}
