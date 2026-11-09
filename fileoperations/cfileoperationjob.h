#pragma once

#include "fileoperationtypes.h"


// Submodule includes
#include "threading/cinterruptablethread.h"


#include <condition_variable>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

// Everything the job queues, in queue order. Repeated progress snapshots
// coalesce to the latest; DecisionRequest, ConflictPlan and OperationSummary are ordering barriers.
using OperationEvent = std::variant<ProgressSnapshot, DecisionRequest, ConflictPlan, OperationSummary>;

// The front end of one job, called by processEvents(). A question's answer is its callback's return value,
// so the callback blocks while the user decides: a modal prompt.
class CFileOperationListener
{
public:
	virtual ~CFileOperationListener() = default;

	virtual void onProgress(const ProgressSnapshot& snapshot) = 0;
	[[nodiscard]] virtual Decision askDecision(const DecisionRequest& request) = 0;
	// nullopt cancels the operation
	[[nodiscard]] virtual std::optional<ConflictResolutions> askConflictPlan(const ConflictPlan& plan) = 0;
	virtual void onFinished(const OperationSummary& summary) = 0;
};

// Derived presentation state. Pause and decision waiting are deliberately not in it: the dialog sets
// pause from its own UI state, and the pending decision has its own query.
enum class JobStatus
{
	NotStarted,
	Running,
	Finished
};

// The one owner of all cross-thread file-operation state: one worker thread, one mutex covering both
// the control state and the event queue, one condition variable waking resume, cancellation, and
// decision submission. Wraps the synchronous executors; the wrapper thread's flag is the only
// cancellation predicate - there is no duplicate job-side cancellation Boolean.
class CFileOperationJob
{
public:
	explicit CFileOperationJob(FileOperationRequest request, uint64_t transferChunkSize = 8 * 1024 * 1024);
	~CFileOperationJob(); // Requests cancellation, wakes every wait, joins

	CFileOperationJob(const CFileOperationJob&) = delete;
	CFileOperationJob& operator=(const CFileOperationJob&) = delete;

	// One-shot; the job never reuses its thread.
	void start();
	// Never joins. Sets the wrapper's flag under the job mutex, so no wait can evaluate its predicate
	// between the store and blocking - a lost wakeup is unrepresentable. Also drops any undrained
	// decision or plan event: cancellation makes it unanswerable. Valid in any state: a cancel() before start()
	// is remembered and applied there, past the wrapper's flag reset.
	void cancel();
	void setPaused(bool paused);
	// False = no unanswered decision is pending, or the supplied answer is invalid. Duplicate and late responses
	// are therefore rejected.
	// There are no decision IDs: the worker cannot reach a second question while blocked on the first.
	bool submitDecision(Decision decision);
	// The plan's counterpart: one resolution per row, nullopt where none applies. False as for submitDecision.
	bool submitConflictResolutions(ConflictResolutions resolutions);

	[[nodiscard]] JobStatus status() const;
	// The check before presenting a question: a DecisionRequest or ConflictPlan already taken out of the queue must not
	// be presented once cancellation has invalidated it or another response has answered it.
	[[nodiscard]] bool hasPendingQuestion() const;

	// Delivers the queued events to the listener and submits its answers. Never asks a question that is no longer pending.
	// Calls the listener with the mutex released: a modal prompt may enter a nested event loop and call back into the job.
	// Must not be called from a listener callback: later events would arrive while the question is still open.
	void processEvents(CFileOperationListener& listener);
	// Takes the queued events, for a caller that uses submitDecision() and submitConflictResolutions() instead of a listener.
	[[nodiscard]] std::vector<OperationEvent> takeEvents();

private:
	// --- Worker side. The flag reference is the wrapper's cancellation flag, passed into the payload. ---

	using WorkerQuestion = std::variant<DecisionRequest, ConflictPlan>;
	using QuestionAnswer = std::variant<Decision, ConflictResolutions>;

	void runOperation(const std::atomic<bool>& cancellationRequested);
	[[nodiscard]] bool workerCheckpoint(const std::atomic<bool>& cancellationRequested);
	// Queues the question as a barrier event and blocks until its answer is submitted or cancellation wins (nullopt).
	[[nodiscard]] std::optional<QuestionAnswer> workerAsk(WorkerQuestion question, const std::atomic<bool>& cancellationRequested);
	[[nodiscard]] std::optional<Decision> workerRequestDecision(DecisionRequest request, const std::atomic<bool>& cancellationRequested);
	[[nodiscard]] std::optional<ConflictResolutions> workerPresentPlan(ConflictPlan plan, const std::atomic<bool>& cancellationRequested);
	void enqueueProgress(const ProgressSnapshot& snapshot);

	// Stores a validated answer to the pending question and wakes the worker. Requires the mutex held; releases it.
	bool submitAnswer(std::unique_lock<std::mutex>& lock, QuestionAnswer answer);

	const FileOperationRequest _request;
	const uint64_t _transferChunkSize;

	mutable std::mutex _mutex;
	std::condition_variable _stateChanged;

	bool _started = false;
	bool _finished = false; // Set only after the summary event is queued: Finished implies the summary is observable
	bool _cancelledBeforeStart = false; // Write-once, consumed by start(); never read as a runtime cancellation predicate
	bool _pauseRequested = false;
	std::optional<WorkerQuestion> _pendingQuestion;
	std::optional<QuestionAnswer> _submittedAnswer;

	std::vector<OperationEvent> _events;

	CInterruptableThread _thread{ "File operation" };
};
