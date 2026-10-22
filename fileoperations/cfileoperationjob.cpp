#include "cfileoperationjob.h"

#include "cdeleteexecutor.h"
#include "coperationexecutioncontext.h"
#include "ctransferexecutor.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()
#include "utility/on_scope_exit.hpp"


#include <algorithm>

CFileOperationJob::CFileOperationJob(FileOperationRequest request, const uint64_t transferChunkSize) :
	_request{ mv(request) },
	_transferChunkSize{ transferChunkSize }
{
}

CFileOperationJob::~CFileOperationJob()
{
	{
		std::lock_guard lock{ _mutex };
		_thread.requestCancellation();
	}
	_stateChanged.notify_all();
	_thread.join();
}

void CFileOperationJob::start()
{
	std::lock_guard lock{ _mutex };
	assert_r(!_started);
	_started = true;
	// Under the mutex: _thread.start() resets the wrapper's cancellation flag, so a concurrent cancel()
	// must not interleave with it. The newborn worker cannot deadlock here: it touches this mutex only
	// from its context callbacks.
	_thread.start([this](const std::atomic<bool>& cancellationRequested) { runOperation(cancellationRequested); });
	// Past the reset: a pre-start cancel() takes effect now, before the worker's first checkpoint can
	// run (it reads the flag under this same mutex), so nothing is ever mutated.
	if (_cancelledBeforeStart)
		_thread.requestCancellation();
}

void CFileOperationJob::cancel()
{
	{
		std::lock_guard lock{ _mutex };
		if (!_started)
		{
			// The wrapper's flag would be wiped by start()'s reset; remember the request for start() to apply.
			_cancelledBeforeStart = true;
			return;
		}
		_thread.requestCancellation();
		std::erase_if(_events, [](const OperationEvent& event) {
			return std::holds_alternative<DecisionRequest>(event) || std::holds_alternative<ConflictPlan>(event);
		});
	}
	_stateChanged.notify_all();
}

void CFileOperationJob::setPaused(const bool paused)
{
	{
		std::lock_guard lock{ _mutex };
		if (_pauseRequested == paused)
			return;
		_pauseRequested = paused;
	}
	if (!paused)
		_stateChanged.notify_all();
}

bool CFileOperationJob::submitDecision(Decision decision)
{
	std::unique_lock lock{ _mutex };
	const auto* request = _pendingQuestion ? std::get_if<DecisionRequest>(&*_pendingQuestion) : nullptr;
	if (!request || _submittedAnswer || _thread.cancellationRequested())
		return false; // No unanswered prompt: invalidated, already answered, or never pending

	// The UI builds its controls from the delivered request, so anything outside it is a caller bug.
	const bool actionAllowed = std::ranges::find(request->allowedActions, decision.action) != request->allowedActions.end();
	const bool scopeAllowed = decision.scope == DecisionScope::ThisItem
		|| (request->remainingMatchingScopeAllowed && isActionRememberable(request->issue.kind, decision.action));
	const bool nameValid = decision.action != DecisionAction::Rename || (decision.newName && !decision.newName->isEmpty());
	assert_r(actionAllowed && scopeAllowed && nameValid);
	if (!actionAllowed || !scopeAllowed || !nameValid)
		return false;

	return submitAnswer(lock, mv(decision));
}

bool CFileOperationJob::submitConflictResolutions(ConflictResolutions resolutions)
{
	std::unique_lock lock{ _mutex };
	const auto* plan = _pendingQuestion ? std::get_if<ConflictPlan>(&*_pendingQuestion) : nullptr;
	if (!plan || _submittedAnswer || _thread.cancellationRequested())
		return false;

	bool valid = resolutions.size() == plan->size();
	for (size_t i = 0; valid && i < resolutions.size(); ++i)
	{
		const std::optional<Decision>& decision = resolutions[i];
		if (!decision)
			continue;

		const AllowedActions plannable = plannableActionsFor((*plan)[i].issue.kind);
		valid = std::ranges::find(plannable, decision->action) != plannable.end()
			&& decision->scope == DecisionScope::ThisItem
			&& (decision->action != DecisionAction::Rename || (decision->newName && !decision->newName->isEmpty()));
	}
	assert_r(valid);
	if (!valid)
		return false;

	return submitAnswer(lock, mv(resolutions));
}

bool CFileOperationJob::submitAnswer(std::unique_lock<std::mutex>& lock, QuestionAnswer answer)
{
	_submittedAnswer = mv(answer);
	lock.unlock();
	_stateChanged.notify_all();
	return true;
}

JobStatus CFileOperationJob::status() const
{
	std::lock_guard lock{ _mutex };
	if (!_started)
		return JobStatus::NotStarted;
	return _finished ? JobStatus::Finished : JobStatus::Running;
}

bool CFileOperationJob::hasPendingQuestion() const
{
	std::lock_guard lock{ _mutex };
	return _pendingQuestion.has_value() && !_submittedAnswer.has_value() && !_thread.cancellationRequested();
}

void CFileOperationJob::processEvents(CFileOperationListener& listener)
{
	std::vector<OperationEvent> drained;
	{
		std::lock_guard lock{ _mutex };
		drained.swap(_events);
	}

	for (const OperationEvent& event : drained)
		listener.onOperationEvent(event);
}

void CFileOperationJob::runOperation(const std::atomic<bool>& cancellationRequested)
{
	COperationExecutionContext context{
		std::holds_alternative<PermanentDeleteRequest>(_request) ? PrimaryProgressUnit::Items : PrimaryProgressUnit::Bytes,
		[this, &cancellationRequested] { return workerCheckpoint(cancellationRequested); },
		[this, &cancellationRequested](const DecisionRequest& request) { return workerRequestDecision(request, cancellationRequested); },
		[this, &cancellationRequested](const ConflictPlan& plan) { return workerPresentPlan(plan, cancellationRequested); },
		[this](const ProgressSnapshot& snapshot) { enqueueProgress(snapshot); }
	};

	// The job is not finished until a summary is queued, and an executor that throws must not cost the dialog its completion.
	OperationSummary summary{ .status = CompletionStatus::Failed };
	EXEC_ON_SCOPE_EXIT([this, &summary] {
		std::lock_guard lock{ _mutex };
		_events.emplace_back(mv(summary)); // The summary precedes the status flip: Finished implies it is queued
		_finished = true;
	});

	summary = std::visit([&]<typename Request>(const Request& request) {
		if constexpr (std::is_same_v<Request, TransferRequest>)
			return CTransferExecutor{ context, _transferChunkSize }.run(request);
		else
			return CDeleteExecutor{ context }.run(request);
	}, _request);
}

bool CFileOperationJob::workerCheckpoint(const std::atomic<bool>& cancellationRequested)
{
	std::unique_lock lock{ _mutex };
	_stateChanged.wait(lock, [this, &cancellationRequested] { return !_pauseRequested || cancellationRequested; });
	return !cancellationRequested;
}

std::optional<CFileOperationJob::QuestionAnswer> CFileOperationJob::workerAsk(WorkerQuestion question, const std::atomic<bool>& cancellationRequested)
{
	std::unique_lock lock{ _mutex };
	assert_r(!_pendingQuestion && !_submittedAnswer);

	if (cancellationRequested)
		return {};

	_pendingQuestion = question;
	std::visit([this](auto& payload) { _events.emplace_back(mv(payload)); }, question); // A barrier event: appended, never coalesced over

	_stateChanged.wait(lock, [this, &cancellationRequested] { return _submittedAnswer.has_value() || cancellationRequested; });

	_pendingQuestion.reset();
	std::optional<QuestionAnswer> answer = mv(_submittedAnswer);
	_submittedAnswer.reset();

	// Cancellation wins even over an answer that arrived in the same wakeup: it terminates the job
	// rather than advancing it.
	if (cancellationRequested)
		return {};
	return answer;
}

std::optional<Decision> CFileOperationJob::workerRequestDecision(DecisionRequest request, const std::atomic<bool>& cancellationRequested)
{
	auto answer = workerAsk(mv(request), cancellationRequested);
	if (!answer)
		return {};
	return std::get<Decision>(mv(*answer));
}

std::optional<ConflictResolutions> CFileOperationJob::workerPresentPlan(ConflictPlan plan, const std::atomic<bool>& cancellationRequested)
{
	auto answer = workerAsk(mv(plan), cancellationRequested);
	if (!answer)
		return {};
	return std::get<ConflictResolutions>(mv(*answer));
}

void CFileOperationJob::enqueueProgress(const ProgressSnapshot& snapshot)
{
	std::lock_guard lock{ _mutex };
	if (!_events.empty() && std::holds_alternative<ProgressSnapshot>(_events.back()))
		_events.back() = snapshot; // Repeated progress coalesces to the latest; barriers stay in place
	else
		_events.emplace_back(snapshot);
}
