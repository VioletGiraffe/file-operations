#include "ctransferexecutor.h"

#include "cfilesystemmutator.h"
#include "coperationexecutioncontext.h"
#include "cstagedfilecopy.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "lang/utils.hpp" // mv()
#include "qtcore_helpers/qstring_hash.hpp" // qtutils


DISABLE_COMPILER_WARNINGS
#include <3rdparty/ankerl/unordered_dense.h>
RESTORE_COMPILER_WARNINGS

#include <algorithm>

// The final names planned into one destination directory. Two items planned onto one name is a collision execution
// would prompt for; where a rename caused it, the planner refuses that rename instead.
class CNameClaims
{
public:
	// Returns the source whose rename is refused: this one, or the earlier claimant if it was the one renamed.
	// nullopt: no clash, or a clash of two unrenamed names, which execution prompts for.
	[[nodiscard]] std::optional<CEntryPath> claim(const CEntryPath& source, const QString& finalName)
	{
		const bool renamed = finalName != source.name();
		const auto [claimed, inserted] = _claims.try_emplace(foldedName(finalName), Claim{ source, renamed });
		if (inserted)
			return {};
		if (renamed)
			return source;
		if (claimed->second.renamed)
			return claimed->second.source;
		return {};
	}

private:
	// Folded where the mainstream filesystems fold case; a wrong guess for a filesystem is one live prompt at execution.
	[[nodiscard]] static QString foldedName(const QString& name)
	{
#if defined(_WIN32) || defined(__APPLE__)
		return name.toCaseFolded();
#else
		return name;
#endif
	}

	struct Claim
	{
		CEntryPath source;
		bool renamed;
	};

	ankerl::unordered_dense::map<QString, Claim, QStringHash, std::equal_to<>> _claims;
};

namespace
{

// Diagnostic context for committed-cleanup prompts and records: the published destination entry.
EntrySnapshot destinationSnapshotFor(const EntrySnapshot& source, const CEntryPath& destination)
{
	const bool directoryLike = source.kind == OperationEntryKind::Directory || source.kind == OperationEntryKind::DirectoryLink;
	return EntrySnapshot{ destination, directoryLike ? OperationEntryKind::Directory : OperationEntryKind::RegularFile, source.size };
}

// Identities of the existing directories that will contain every root's destination, each followed as a native path follows it.
std::vector<thin_io::entry_identity> destinationAncestorIdentities(const DestinationSpec& destination)
{
	std::vector<thin_io::entry_identity> identities;
	CEntryPath ancestor = destination.intent == DestinationIntent::IntoDirectory ? destination.path : destination.path.parent();
	for (;;)
	{
		if (const auto identity = readEntryIdentity(ancestor, thin_io::link_behavior::follow); identity && *identity)
			identities.push_back(**identity);
		if (ancestor.isRoot())
			return identities;
		ancestor = ancestor.parent();
	}
}

bool isDirectoryLike(const OperationEntryKind kind) noexcept
{
	return kind == OperationEntryKind::Directory || kind == OperationEntryKind::DirectoryLink;
}

bool isFileLike(const OperationEntryKind kind) noexcept
{
	return kind == OperationEntryKind::RegularFile || kind == OperationEntryKind::FileLink;
}

SourceNode singleEntryManifest(EntrySnapshot entry)
{
	SourceNode leaf{ .entry = mv(entry) };
	leaf.subtreeBytes = leaf.entry.size;
	leaf.subtreeItems = 1;
	return leaf;
}

bool hasRenameAnswer(const ConflictPlan& plan)
{
	return std::ranges::any_of(plan, [](const PlannedConflict& row) { return row.resolution && row.resolution->action == DecisionAction::Rename; });
}

bool hasUnansweredRow(const ConflictPlan& plan)
{
	return std::ranges::any_of(plan, [](const PlannedConflict& row) { return !row.resolution || row.renameTargetTakenByAnotherItem; });
}

} // namespace

CTransferExecutor::CTransferExecutor(COperationExecutionContext& context, const uint64_t transferChunkSize) noexcept
	: _context{ context }
	, _transferChunkSize{ transferChunkSize }
{
	assert_debug_only(transferChunkSize > 0);
}

OperationSummary CTransferExecutor::run(const TransferRequest& request)
{
	_requestKind = request.kind;
	_destinationAncestorIdentities = destinationAncestorIdentities(request.destination);

	std::vector<PlannedRoot> roots;
	size_t rootsMovedByRename = 0;
	bool anyFailed = false;
	for (const RootTransferIntent& intent : rootTransferIntents(request))
	{
		auto planned = planRoot(intent);
		if (auto* root = std::get_if<PlannedRoot>(&planned))
		{
			roots.push_back(mv(*root));
			continue;
		}

		const NodeOutcome outcome = finishUnscannedRoot(std::get<NodeOutcome>(planned));
		if (outcome == NodeOutcome::Cancelled)
			return _context.makeSummary(CompletionStatus::Cancelled); // Remaining roots stay untouched
		anyFailed = anyFailed || outcome == NodeOutcome::Failed;
		rootsMovedByRename += outcome == NodeOutcome::Completed ? 1 : 0;
	}

	if (!resolveConflictPlan(roots))
		return _context.makeSummary(CompletionStatus::Cancelled);

	uint64_t totalBytes = 0;
	size_t totalItems = rootsMovedByRename;
	for (const PlannedRoot& root : roots)
	{
		totalBytes += root.tree.subtreeBytes;
		totalItems += root.tree.subtreeItems;
	}
	_context.progress().setTotals(totalBytes, totalItems);
	_context.publishProgressSnapshot(); // Working, with exact totals from here on

	for (const PlannedRoot& root : roots)
	{
		const NodeOutcome outcome = _requestKind == TransferKind::Move ? moveRoot(root) : copyRoot(root);
		if (outcome == NodeOutcome::Cancelled)
			return _context.makeSummary(CompletionStatus::Cancelled);
		anyFailed = anyFailed || outcome == NodeOutcome::Failed;
	}

	return _context.makeSummary(anyFailed ? CompletionStatus::Failed : CompletionStatus::Completed);
}

// --- Planning ---

std::variant<CTransferExecutor::PlannedRoot, NodeOutcome> CTransferExecutor::planRoot(const RootTransferIntent& intent)
{
	if (!_context.checkpoint())
		return NodeOutcome::Cancelled;

	auto rootEntry = inspectSourceRoot(intent.source);
	if (const auto* endedOutcome = std::get_if<NodeOutcome>(&rootEntry))
		return *endedOutcome;
	EntrySnapshot& root = std::get<EntrySnapshot>(rootEntry);

	// Before a move creates the parent chain: it would be created inside the source.
	if (root.kind == OperationEntryKind::Directory && refuseDestinationInsideSource(root, intent.proposedDestination))
		return NodeOutcome::Failed;

	bool knownCrossDevice = false;
	bool predictedCrossDevice = false;
	if (_requestKind == TransferKind::Move)
	{
		if (const auto outcome = ensureDestinationParentExistsWithPolicy(root, intent.proposedDestination))
			return *outcome;

		// The optimistic whole-root rename. RequireAbsent cannot clobber anything.
		// A case-only respell succeeds here through the primitive's exclusive temporary-sibling path, so resolution's
		// silent same-object handling never sees one.
		auto renamed = renameEntryWithPolicy(root, intent.proposedDestination, ReplacementMode::RequireAbsent);
		if (const auto* outcome = std::get_if<NodeOutcome>(&renamed))
			return *outcome;
		knownCrossDevice = std::get<RenameBlock>(renamed) == RenameBlock::CrossDevice;
		if (!knownCrossDevice)
		{
			const CEntryPath& destination = intent.proposedDestination;
			predictedCrossDevice = crossDevicePredicted(root.path, destination.isRoot() ? destination : destination.parent());
		}
	}

	if (!isDirectoryLike(root.kind))
		return PlannedRoot{ intent, singleEntryManifest(mv(root)), knownCrossDevice, predictedCrossDevice };

	auto built = buildManifestWithRetry(root);
	if (const auto* endedOutcome = std::get_if<NodeOutcome>(&built))
		return *endedOutcome;
	return PlannedRoot{ intent, mv(std::get<SourceNode>(built)), knownCrossDevice, predictedCrossDevice };
}

bool CTransferExecutor::resolveConflictPlan(const std::vector<PlannedRoot>& roots)
{
	ConflictPlan plan;
	for (;;)
	{
		_context.startConflictRecording();
		const bool completed = planAllRootConflicts(roots);
		plan = _context.finishConflictRecording();
		if (!completed)
			return false;
		if (!hasUnansweredRow(plan))
			break;

		auto resolutions = _context.presentConflictPlan(plan);
		if (!resolutions)
			return false;

		assert_r(resolutions->size() == plan.size());
		for (size_t i = 0; i < plan.size() && i < resolutions->size(); ++i)
		{
			plan[i].resolution = mv((*resolutions)[i]);
			plan[i].renameTargetTakenByAnotherItem = false;
		}
		_context.setPlannedDecisions(plan);

		// Only a rename changes a destination: without one, nothing below the answered rows can have changed.
		if (!hasRenameAnswer(plan))
			return true;
	}

	_context.setPlannedDecisions(mv(plan)); // The recording consumed the answers; arm them again for execution
	return true;
}

bool CTransferExecutor::planAllRootConflicts(const std::vector<PlannedRoot>& roots)
{
	CNameClaims rootNameClaims;
	for (const PlannedRoot& root : roots)
	{
		_planningItemsFound = root.tree.subtreeItems;
		if (!planSubtree(root.tree, root.intent.proposedDestination, TransferNodePosition::SelectedRoot, true,
			root.knownCrossDevice || root.crossDevicePredicted, rootNameClaims))
			return false;
	}
	return true;
}

bool CTransferExecutor::planSubtree(const SourceNode& node, const CEntryPath& proposedDestination, const TransferNodePosition position,
	const bool destinationMayExist, const bool crossDevice, CNameClaims& siblingClaims)
{
	if (!_context.checkpoint())
		return false;
	publishPlanningProgress(node.entry.path);

	if (node.entry.kind == OperationEntryKind::Other)
	{
		// A copy never streams it. A move renames it where the destination is free on the same volume; everywhere
		// else it is the same question.
		bool unsupported = _requestKind == TransferKind::Copy || crossDevice;
		if (!unsupported && destinationMayExist)
		{
			const auto destination = inspectEntry(proposedDestination);
			unsupported = destination && destination->has_value();
		}
		if (unsupported)
		{
			const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::UnsupportedEntry, node.entry, {}, {} });
			return decision && decision->action != DecisionAction::Cancel;
		}
		return true;
	}

	DestinationChoice choice = UseDestination{ proposedDestination, ReplacementMode::RequireAbsent };
	if (destinationMayExist)
	{
		choice = isFileLike(node.entry.kind)
			? resolveFileDestination(_context, node.entry, proposedDestination)
			: resolveDirectoryDestination(_context, node.entry, proposedDestination, position);
	}
	if (std::holds_alternative<CancelOperation>(choice))
		return false;
	if (std::holds_alternative<AlreadySatisfied>(choice))
		return true;

	// Asked whatever the collision answer turns out to be: the copy-based move removes the source only after
	// publication, and that removal is the question.
	if (_requestKind == TransferKind::Move && crossDevice && node.ownership == SourceOwnership::Owned && node.entry.kind == OperationEntryKind::RegularFile)
	{
		const auto preflight = resolveMoveReadOnlyPreflight(node.entry);
		if (const auto* outcome = std::get_if<NodeOutcome>(&preflight); outcome && *outcome == NodeOutcome::Cancelled)
			return false;
	}

	if (std::holds_alternative<SkipNode>(choice))
		return true;

	const auto* merge = std::get_if<MergeDirectory>(&choice);
	const CEntryPath& finalDestination = merge ? merge->path : std::get<UseDestination>(choice).path;
	if (const auto refusedRename = siblingClaims.claim(node.entry.path, finalDestination.name()))
		_context.flagRenameTargetTaken(*refusedRename);

	if (!isDirectoryLike(node.entry.kind))
		return true;

	CNameClaims childClaims;
	for (const SourceNode& child : node.children)
	{
		if (!planSubtree(child, finalDestination.child(child.entry.path.name()), TransferNodePosition::Descendant, merge != nullptr, crossDevice, childClaims))
			return false;
	}
	return true;
}

void CTransferExecutor::publishPlanningProgress(const CEntryPath& currentEntry)
{
	ProgressSnapshot snapshot;
	snapshot.phase = OperationPhase::Scanning;
	snapshot.currentEntry = currentEntry;
	snapshot.itemsProcessed = _planningItemsFound;
	_context.publishProgress(snapshot);
}

std::variant<EntrySnapshot, NodeOutcome> CTransferExecutor::inspectSourceRoot(const CEntryPath& source)
{
	for (;;)
	{
		auto inspected = inspectEntry(source);
		if (inspected && inspected->has_value())
			return mv(**inspected);

		CFileSystemError error = inspected
			? CFileSystemError{ FileErrorCategory::NotFound, 0, QStringLiteral("The source no longer exists") }
			: mv(inspected.error());

		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed,
			EntrySnapshot{ source, OperationEntryKind::Unknown, 0 }, {}, FailureDetails{ FailedAction::InspectSource, source, mv(error) } });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		if (decision->action == DecisionAction::Skip)
			return NodeOutcome::Skipped;
		assert_debug_only(decision->action == DecisionAction::Retry);
	}
}

std::variant<SourceNode, NodeOutcome> CTransferExecutor::buildManifestWithRetry(const EntrySnapshot& root)
{
	// The diagnostic names the entry that failed to scan, not necessarily the root.
	for (;;)
	{
		auto built = buildSourceTree(_context, root, SourceTreeBuildMode::MaterializingTransfer);
		if (auto* node = std::get_if<SourceNode>(&built))
			return mv(*node);
		if (std::holds_alternative<ScanCancelled>(built))
			return NodeOutcome::Cancelled;

		auto& diagnostic = std::get<OperationDiagnostic>(built);
		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed, mv(diagnostic.source), {}, mv(diagnostic.failure) });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		if (decision->action == DecisionAction::Skip)
			return NodeOutcome::Skipped;
		assert_debug_only(decision->action == DecisionAction::Retry);
	}
}

NodeOutcome CTransferExecutor::finishUnscannedRoot(const NodeOutcome outcome)
{
	switch (outcome)
	{
	case NodeOutcome::Completed: // Renamed wholesale
		_context.addCompletedItems(1);
		_context.progress().itemCompleted();
		_context.publishProgressSnapshot();
		break;
	case NodeOutcome::Skipped:
		_context.addSkippedItems(1);
		break;
	case NodeOutcome::AlreadySatisfied:
		_context.addAlreadySatisfiedItems(1);
		break;
	default: // A Failed root has already recorded its terminal diagnostic
		assert_debug_only(outcome == NodeOutcome::Cancelled || outcome == NodeOutcome::Failed);
		break;
	}
	return outcome;
}

bool CTransferExecutor::refuseDestinationInsideSource(const EntrySnapshot& root, const CEntryPath& proposedDestination)
{
	assert_debug_only(root.kind == OperationEntryKind::Directory);

	const auto identity = readEntryIdentity(root.path, thin_io::link_behavior::do_not_follow);
	if (!identity || !*identity) // No identity, no proof
		return false;
	if (std::find(_destinationAncestorIdentities.begin(), _destinationAncestorIdentities.end(), **identity) == _destinationAncestorIdentities.end())
		return false;

	const FailedAction action = _requestKind == TransferKind::Move ? FailedAction::RenameEntry : FailedAction::CreateDestinationDirectory;
	_context.recordFailure(OperationDiagnostic{ FailureDetails{ action, proposedDestination,
		CFileSystemError{ FileErrorCategory::Unsupported, 0, QStringLiteral("The destination is inside the source") } }, root, {} });
	return true;
}

std::optional<NodeOutcome> CTransferExecutor::ensureDestinationParentExistsWithPolicy(
	const EntrySnapshot& source, const CEntryPath& proposedDestination)
{
	const CEntryPath parent = proposedDestination.isRoot() ? proposedDestination : proposedDestination.parent();
	for (;;)
	{
		if (!_context.checkpoint())
			return NodeOutcome::Cancelled;

		std::optional<FailureDetails> failure;
		auto parentEntry = inspectEntry(parent);
		if (!parentEntry)
		{
			failure = FailureDetails{ FailedAction::InspectDestination, parent, mv(parentEntry.error()) };
		}
		else if (parentEntry->has_value())
		{
			const OperationEntryKind kind = (**parentEntry).kind;
			if (kind == OperationEntryKind::Directory || kind == OperationEntryKind::DirectoryLink)
				return std::nullopt;

			failure = FailureDetails{ FailedAction::CreateDestinationDirectory, parent, CFileSystemError{ FileErrorCategory::AlreadyExists, 0,
				QStringLiteral("The destination parent is not a directory") } };
		}
		else
		{
			auto created = CFileSystemMutator::createDirectories(parent);
			if (created)
			{
				if (*created == DirectoryCreationOutcome::CreatedFinalDirectory)
					return std::nullopt;

				assert_debug_only(*created == DirectoryCreationOutcome::FinalEntryAlreadyExisted);
				continue; // An entry raced into the final parent: inspect its kind freshly.
			}
			failure = FailureDetails{ FailedAction::CreateDestinationDirectory, parent, mv(created.error()) };
		}

		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed, source, {}, mv(*failure) });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		if (decision->action == DecisionAction::Skip)
			return NodeOutcome::Skipped;
		assert_debug_only(decision->action == DecisionAction::Retry);
	}
}

// --- Copy ---

NodeOutcome CTransferExecutor::copyRoot(const PlannedRoot& root)
{
	const SourceNode& tree = root.tree;
	const CEntryPath& proposedDestination = root.intent.proposedDestination;
	if (isDirectoryLike(tree.entry.kind))
	{
		if (!_context.checkpoint())
			return NodeOutcome::Cancelled;
		return runDirectoryNode(tree, TransferNodePosition::SelectedRoot,
			resolveDirectoryDestination(_context, tree.entry, proposedDestination, TransferNodePosition::SelectedRoot), false);
	}

	if (isFileLike(tree.entry.kind))
	{
		if (const auto outcome = ensureDestinationParentExistsWithPolicy(tree.entry, proposedDestination))
			return finishUnscannedRoot(*outcome);
	}
	// Other and file kinds route through the ordinary node dispatch.
	return copyNode(tree, proposedDestination, TransferNodePosition::SelectedRoot);
}

NodeOutcome CTransferExecutor::copyNode(const SourceNode& node, CEntryPath proposedDestination, const TransferNodePosition position)
{
	if (!_context.checkpoint())
		return NodeOutcome::Cancelled;

	if (node.entry.kind == OperationEntryKind::Other)
	{
		// Never streamed as a file; the one policy question is skip or cancel.
		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::UnsupportedEntry, node.entry, {}, {} });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		assert_debug_only(decision->action == DecisionAction::Skip);
		accountSkippedSubtree(node);
		return NodeOutcome::Skipped;
	}

	const bool fileLike = node.entry.kind == OperationEntryKind::RegularFile || node.entry.kind == OperationEntryKind::FileLink;
	if (!fileLike)
		return runDirectoryNode(node, position, resolveDirectoryDestination(_context, node.entry, mv(proposedDestination), position), false);

	for (;;)
	{
		DestinationChoice choice = resolveFileDestination(_context, node.entry, mv(proposedDestination));
		if (std::holds_alternative<SkipNode>(choice))
		{
			accountSkippedSubtree(node);
			return NodeOutcome::Skipped;
		}
		if (std::holds_alternative<CancelOperation>(choice))
			return NodeOutcome::Cancelled;
		if (std::holds_alternative<AlreadySatisfied>(choice))
		{
			accountAlreadySatisfiedSubtree(node);
			return NodeOutcome::AlreadySatisfied;
		}

		auto& use = std::get<UseDestination>(choice); // MergeDirectory cannot come from the file resolver
		const auto outcome = stagedFileTransferWithPolicy(node.entry, use.path, use.replacement, PublishedSourceAction::RetainSource, false);
		if (outcome)
			return *outcome;
		proposedDestination = mv(use.path); // A new collision appeared at publication: resolve it freshly
	}
}

NodeOutcome CTransferExecutor::runDirectoryNode(const SourceNode& node, const TransferNodePosition position, DestinationChoice choice, const bool knownCrossDevice)
{
	for (;;)
	{
		if (std::holds_alternative<SkipNode>(choice))
		{
			accountSkippedSubtree(node);
			return NodeOutcome::Skipped;
		}
		if (std::holds_alternative<CancelOperation>(choice))
			return NodeOutcome::Cancelled;
		if (std::holds_alternative<AlreadySatisfied>(choice))
		{
			accountAlreadySatisfiedSubtree(node);
			return NodeOutcome::AlreadySatisfied;
		}
		if (const auto* merge = std::get_if<MergeDirectory>(&choice))
		{
			return _requestKind == TransferKind::Move
				? moveDirectoryContents(node, merge->path, false, knownCrossDevice)
				: copyDirectoryContents(node, merge->path, false);
		}

		const auto& use = std::get<UseDestination>(choice);
		const auto created = CFileSystemMutator::createDirectories(use.path);
		if (created)
		{
			if (*created == DirectoryCreationOutcome::CreatedFinalDirectory)
			{
				return _requestKind == TransferKind::Move
					? moveDirectoryContents(node, use.path, true, knownCrossDevice)
					: copyDirectoryContents(node, use.path, true);
			}

			assert_debug_only(*created == DirectoryCreationOutcome::FinalEntryAlreadyExisted);
			// An entry appeared since resolution: a fresh collision, resolved anew.
			choice = resolveDirectoryDestination(_context, node.entry, use.path, position);
			continue;
		}

		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed, node.entry,
			{}, FailureDetails{ FailedAction::CreateDestinationDirectory, use.path, created.error() } });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		if (decision->action == DecisionAction::Skip)
		{
			accountSkippedSubtree(node);
			return NodeOutcome::Skipped;
		}
		assert_debug_only(decision->action == DecisionAction::Retry);
	}
}

NodeOutcome CTransferExecutor::copyDirectoryContents(const SourceNode& node, const CEntryPath& destination, const bool operationCreated)
{
	_context.addCompletedItems(1); // The directory entry itself: created or merged successfully
	_context.progress().itemCompleted();
	_context.publishProgressSnapshot();

	// Only an operation-created directory is stamped; a pre-existing merge target keeps its own times.
	std::optional<CopyableDirectoryTimes> copyableTimes;
	if (operationCreated)
	{
		auto captured = readCopyableDirectoryTimes(node.entry.path);
		if (captured)
			copyableTimes = *captured;
		else
			recordTimestampWarning(node.entry, destination, node.entry.path, mv(captured.error()));
	}

	NodeOutcome aggregate = NodeOutcome::Completed;
	for (const SourceNode& child : node.children)
	{
		const NodeOutcome childOutcome = copyNode(child, destination.child(child.entry.path.name()), TransferNodePosition::Descendant);
		aggregate = aggregateChildOutcome(aggregate, childOutcome);
		if (childOutcome == NodeOutcome::Cancelled)
			break;
	}

	// Post-order, so child mutations cannot disturb the stamp; skipped on cancellation because the
	// directory's content story did not finish.
	if (operationCreated && copyableTimes && aggregate != NodeOutcome::Cancelled)
	{
		if (const auto applied = CFileSystemMutator::applyDirectoryTimes(destination, *copyableTimes); !applied)
			recordTimestampWarning(node.entry, destination, destination, applied.error());
	}

	return aggregate;
}

// --- Move ---

NodeOutcome CTransferExecutor::moveRoot(const PlannedRoot& root)
{
	const SourceNode& tree = root.tree;
	bool knownCrossDevice = root.knownCrossDevice;
	if (!isDirectoryLike(tree.entry.kind))
		return moveNode(tree, root.intent.proposedDestination, TransferNodePosition::SelectedRoot, knownCrossDevice);

	// Planning already tried the whole-root rename. A rename decision loops back so the respelled target gets its own attempt.
	CEntryPath proposedDestination = root.intent.proposedDestination;
	for (;;)
	{
		if (!_context.checkpoint())
			return NodeOutcome::Cancelled;

		DestinationChoice choice = resolveDirectoryDestination(_context, tree.entry, proposedDestination, TransferNodePosition::SelectedRoot);
		if (const auto* use = std::get_if<UseDestination>(&choice); use && !knownCrossDevice)
		{
			auto renamed = renameEntryWithPolicy(tree.entry, use->path, use->replacement);
			if (const auto* outcome = std::get_if<NodeOutcome>(&renamed))
			{
				if (*outcome == NodeOutcome::Completed)
					accountRenamedSubtree(tree);
				else if (*outcome == NodeOutcome::Skipped)
					accountSkippedSubtree(tree);
				return *outcome;
			}
			if (std::get<RenameBlock>(renamed) == RenameBlock::CrossDevice)
				knownCrossDevice = true;
			else
			{
				proposedDestination = use->path; // An entry raced in: resolve the new collision freshly
				continue;
			}
		}

		// Merge, or cross-device materialization: traversal is required from here.
		return runDirectoryNode(tree, TransferNodePosition::SelectedRoot, mv(choice), knownCrossDevice);
	}
}

NodeOutcome CTransferExecutor::moveNode(const SourceNode& node, CEntryPath proposedDestination, const TransferNodePosition position, bool knownCrossDevice)
{
	if (!_context.checkpoint())
		return NodeOutcome::Cancelled;

	const bool owned = node.ownership == SourceOwnership::Owned;

	if (owned && !knownCrossDevice)
	{
		// Rename-first at this boundary; borrowed entries are materialized, never renamed away from their source.
		auto renamed = renameEntryWithPolicy(node.entry, proposedDestination, ReplacementMode::RequireAbsent);
		if (const auto* outcome = std::get_if<NodeOutcome>(&renamed))
		{
			if (*outcome == NodeOutcome::Completed)
				accountRenamedSubtree(node);
			else if (*outcome == NodeOutcome::Skipped)
				accountSkippedSubtree(node);
			return *outcome;
		}
		knownCrossDevice = std::get<RenameBlock>(renamed) == RenameBlock::CrossDevice;
		// DestinationOccupied falls through to resolution
	}

	if (node.entry.kind == OperationEntryKind::Other)
	{
		// With rename unavailable this entry cannot move - it is never streamed as a file.
		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::UnsupportedEntry, node.entry, {}, {} });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		assert_debug_only(decision->action == DecisionAction::Skip);
		accountSkippedSubtree(node);
		return NodeOutcome::Skipped;
	}

	const bool fileLike = node.entry.kind == OperationEntryKind::RegularFile || node.entry.kind == OperationEntryKind::FileLink;
	if (!fileLike)
		return runDirectoryNode(node, position, resolveDirectoryDestination(_context, node.entry, mv(proposedDestination), position), knownCrossDevice);

	return moveFileNode(node, mv(proposedDestination), knownCrossDevice);
}

NodeOutcome CTransferExecutor::moveFileNode(const SourceNode& node, CEntryPath proposedDestination, bool knownCrossDevice)
{
	const bool owned = node.ownership == SourceOwnership::Owned;
	std::optional<bool> makeWritableAuthorized; // The pre-publication read-only policy answer, resolved once per node

	for (;;)
	{
		if (!_context.checkpoint())
			return NodeOutcome::Cancelled;

		DestinationChoice choice = resolveFileDestination(_context, node.entry, mv(proposedDestination));
		if (std::holds_alternative<SkipNode>(choice))
		{
			accountSkippedSubtree(node);
			return NodeOutcome::Skipped;
		}
		if (std::holds_alternative<CancelOperation>(choice))
			return NodeOutcome::Cancelled;
		if (std::holds_alternative<AlreadySatisfied>(choice))
		{
			// Same object: move never removes a source that is also the destination.
			accountAlreadySatisfiedSubtree(node);
			return NodeOutcome::AlreadySatisfied;
		}

		auto& use = std::get<UseDestination>(choice); // MergeDirectory cannot come from the file resolver

		if (owned && !knownCrossDevice)
		{
			// The resolved disposition can still be one atomic native rename: an authorized replacement,
			// or a respelled target from a Rename decision.
			auto renamed = renameEntryWithPolicy(node.entry, use.path, use.replacement);
			if (const auto* outcome = std::get_if<NodeOutcome>(&renamed))
			{
				if (*outcome == NodeOutcome::Completed)
					accountRenamedSubtree(node);
				else if (*outcome == NodeOutcome::Skipped)
					accountSkippedSubtree(node);
				return *outcome;
			}
			if (std::get<RenameBlock>(renamed) == RenameBlock::CrossDevice)
				knownCrossDevice = true;
			else
			{
				proposedDestination = mv(use.path); // The destination raced: resolve the new collision freshly
				continue;
			}
		}

		if (owned && node.entry.kind == OperationEntryKind::RegularFile && !makeWritableAuthorized.has_value())
		{
			auto preflight = resolveMoveReadOnlyPreflight(node.entry);
			if (const auto* outcome = std::get_if<NodeOutcome>(&preflight))
			{
				if (*outcome == NodeOutcome::Skipped)
					accountSkippedSubtree(node);
				return *outcome;
			}
			makeWritableAuthorized = std::get<bool>(preflight);
		}

		const auto sourceAction = owned ? PublishedSourceAction::RemoveOwnedSource : PublishedSourceAction::RetainSource;
		const auto outcome = stagedFileTransferWithPolicy(node.entry, use.path, use.replacement, sourceAction, makeWritableAuthorized.value_or(false));
		if (outcome)
			return *outcome;
		proposedDestination = mv(use.path); // A new collision appeared at publication: resolve it freshly
	}
}

NodeOutcome CTransferExecutor::moveDirectoryContents(const SourceNode& node, const CEntryPath& destination, const bool operationCreated, const bool knownCrossDevice)
{
	const bool owned = node.ownership == SourceOwnership::Owned;
	if (!owned)
	{
		// Materialization is a borrowed entry's complete required work; an owned directory counts only
		// after its source entry is removed.
		_context.addCompletedItems(1);
		_context.progress().itemCompleted();
		_context.publishProgressSnapshot();
	}

	// Only an operation-created directory is stamped; a pre-existing merge target keeps its own times.
	// For a materialized directory link, the capture deliberately follows the link.
	std::optional<CopyableDirectoryTimes> copyableTimes;
	if (operationCreated)
	{
		auto captured = readCopyableDirectoryTimes(node.entry.path);
		if (captured)
			copyableTimes = *captured;
		else
			recordTimestampWarning(node.entry, destination, node.entry.path, mv(captured.error()));
	}

	NodeOutcome aggregate = NodeOutcome::Completed;
	bool allChildrenCompleted = true;
	for (const SourceNode& child : node.children)
	{
		const NodeOutcome childOutcome = moveNode(child, destination.child(child.entry.path.name()), TransferNodePosition::Descendant, knownCrossDevice);
		aggregate = aggregateChildOutcome(aggregate, childOutcome);
		allChildrenCompleted = allChildrenCompleted && childOutcome == NodeOutcome::Completed;
		if (childOutcome == NodeOutcome::Cancelled)
			break;
	}

	if (operationCreated && copyableTimes && aggregate != NodeOutcome::Cancelled)
	{
		if (const auto applied = CFileSystemMutator::applyDirectoryTimes(destination, *copyableTimes); !applied)
			recordTimestampWarning(node.entry, destination, destination, applied.error());
	}

	if (!owned || aggregate == NodeOutcome::Cancelled)
		return aggregate;

	// An AlreadySatisfied child keeps its source entry in place, so it blocks cleanup exactly like a skip.
	if (!allChildrenCompleted)
	{
		// The owned directory was resolved and deliberately retained, but was neither moved nor independently skipped.
		_context.progress().advanceWithoutTransfer(0, 1);
		_context.publishProgressSnapshot();
		return aggregate == NodeOutcome::Completed ? NodeOutcome::Partial : aggregate;
	}

	// Committed cleanup of the emptied source directory (or the directory-link entry); timestamps are
	// finalized above, and no cancellation checkpoint separates them from this removal.
	return removePublishedSourceWithPolicy(node.entry, destination, false);
}

std::variant<NodeOutcome, CTransferExecutor::RenameBlock> CTransferExecutor::renameEntryWithPolicy(
	const EntrySnapshot& source, const CEntryPath& destination, const ReplacementMode replacement)
{
	for (;;)
	{
		auto renamed = CFileSystemMutator::renameEntry(source.path, destination, replacement);
		if (renamed)
			return NodeOutcome::Completed;

		CFileSystemError error = mv(renamed.error());
		if (error.category == FileErrorCategory::CrossDevice)
			return RenameBlock::CrossDevice;
		if (error.category == FileErrorCategory::AlreadyExists)
			return RenameBlock::DestinationOccupied;

		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed, source, {},
			FailureDetails{ FailedAction::RenameEntry, destination, mv(error) } });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		if (decision->action == DecisionAction::Skip)
			return NodeOutcome::Skipped;
		assert_debug_only(decision->action == DecisionAction::Retry);
	}
}

std::variant<bool, NodeOutcome> CTransferExecutor::resolveMoveReadOnlyPreflight(const EntrySnapshot& entry)
{
	for (;;)
	{
		// Inspection trouble is deliberately not prompted here: the staged transfer surfaces the truthful failure.
		const auto writable = isEntryWritableNoFollow(entry);
		if (!writable || *writable)
			return false;

		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ReadOnlySourceRemoval, entry, {}, {} });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		if (decision->action == DecisionAction::Skip)
			return NodeOutcome::Skipped;
		if (decision->action == DecisionAction::MakeWritable)
			return true; // A local authorization only; the source is not touched until after publication
		assert_debug_only(decision->action == DecisionAction::Retry);
	}
}

NodeOutcome CTransferExecutor::removePublishedSourceWithPolicy(const EntrySnapshot& entry, const CEntryPath& destination, const bool makeWritableAuthorized)
{
	const bool remediableFile = entry.kind == OperationEntryKind::RegularFile;
	assert_debug_only(!makeWritableAuthorized || remediableFile); // The preflight only ever authorizes a regular file
	bool applyWritableAuthorization = makeWritableAuthorized;

	for (;;)
	{
		if (applyWritableAuthorization)
		{
			if (const auto madeWritable = CFileSystemMutator::setEntryWritable(entry, true); !madeWritable)
			{
				FailureDetails failure{ FailedAction::MakeWritable, entry.path, madeWritable.error() };
				const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed, entry,
					destinationSnapshotFor(entry, destination), failure }, false);
				if (!decision || decision->action == DecisionAction::Cancel)
					return recordTerminalCleanupFailure(entry, destination, mv(failure), NodeOutcome::Cancelled);
				if (decision->action == DecisionAction::Skip)
					return recordTerminalCleanupFailure(entry, destination, mv(failure), NodeOutcome::Failed);
				assert_debug_only(decision->action == DecisionAction::Retry);
				continue;
			}
			applyWritableAuthorization = false; // Applied; a removal retry must not redo it
		}

		auto removed = CFileSystemMutator::removeEntry(entry.path);
		if (removed)
			break;

		CFileSystemError error = mv(removed.error());
		if (error.category == FileErrorCategory::NotFound)
			break; // The source is gone - the required end state

		// A raced read-only result returns to the read-only question only when fresh inspection confirms
		// it; directories and links never enter remediation, and generic access denied never means read-only.
		if (remediableFile && error.category == FileErrorCategory::ReadOnly)
		{
			if (const auto writable = isEntryWritableNoFollow(entry); writable && !*writable)
			{
				FailureDetails failure{ FailedAction::RemovePublishedMoveSource, entry.path, mv(error) };
				const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ReadOnlySourceRemoval, entry,
					destinationSnapshotFor(entry, destination), failure }, false);
				if (!decision || decision->action == DecisionAction::Cancel)
					return recordTerminalCleanupFailure(entry, destination, mv(failure), NodeOutcome::Cancelled);
				if (decision->action == DecisionAction::Skip)
					return recordTerminalCleanupFailure(entry, destination, mv(failure), NodeOutcome::Failed);
				if (decision->action == DecisionAction::MakeWritable)
					applyWritableAuthorization = true;
				else
					assert_debug_only(decision->action == DecisionAction::Retry);
				continue;
			}
		}

		FailureDetails failure{ FailedAction::RemovePublishedMoveSource, entry.path, mv(error) };
		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed, entry,
			destinationSnapshotFor(entry, destination), failure }, false);
		if (!decision || decision->action == DecisionAction::Cancel)
			return recordTerminalCleanupFailure(entry, destination, mv(failure), NodeOutcome::Cancelled);
		if (decision->action == DecisionAction::Skip)
			return recordTerminalCleanupFailure(entry, destination, mv(failure), NodeOutcome::Failed);
		assert_debug_only(decision->action == DecisionAction::Retry);
	}

	_context.addCompletedItems(1); // Moved: required source removal is done
	_context.progress().itemCompleted();
	_context.progress().clearCurrentEntry();
	_context.publishProgressSnapshot();
	return NodeOutcome::Completed;
}

NodeOutcome CTransferExecutor::recordTerminalCleanupFailure(const EntrySnapshot& entry, const CEntryPath& destination,
	FailureDetails failure, const NodeOutcome outcome)
{
	// Both entries are retained; the published destination is never deleted as rollback.
	_context.recordFailure(OperationDiagnostic{ mv(failure), entry, destinationSnapshotFor(entry, destination) });
	_context.progress().advanceWithoutTransfer(0, 1);
	_context.progress().clearCurrentEntry();
	_context.publishProgressSnapshot();
	return outcome;
}

void CTransferExecutor::accountRenamedSubtree(const SourceNode& node)
{
	_context.addCompletedItems(node.subtreeItems);
	_context.progress().advanceWithoutTransfer(node.subtreeBytes, node.subtreeItems);
	_context.publishProgressSnapshot();
}

// --- Staged transfer (copy, and the copy-based move fallback) ---

std::optional<NodeOutcome> CTransferExecutor::stagedFileTransferWithPolicy(const EntrySnapshot& source, const CEntryPath& destination,
	const ReplacementMode replacement, const PublishedSourceAction sourceAction, const bool makeWritableAuthorized)
{
	for (;;)
	{
		if (!_context.checkpoint())
			return NodeOutcome::Cancelled;

		_context.progress().setCurrentEntry(source.path, source.size);
		_context.publishProgressSnapshot();

		std::optional<FailureDetails> failure;
		bool sessionCompleted = false;
		bool freshCollisionAtPublication = false;

		auto session = CStagedFileCopy::begin(source.path, destination);
		if (!session)
		{
			auto beginFailure = mv(session.error());
			failure = mv(beginFailure.primaryFailure);
			if (beginFailure.cleanupFailure)
				_context.recordWarning(OperationDiagnostic{ mv(*beginFailure.cleanupFailure), source, {} });
		}
		else
		{
			bool transferFailed = false;
			for (;;)
			{
				if (!_context.checkpoint())
				{
					abortStagedSession(*session, source);
					_context.progress().currentEntryAbandoned();
					return NodeOutcome::Cancelled;
				}

				auto chunk = session->writeNext(_transferChunkSize);
				if (!chunk)
				{
					failure = mv(chunk.error());
					transferFailed = true;
					break;
				}
				if (chunk->bytesWritten != 0)
				{
					_context.progress().fileTransferAdvanced(chunk->bytesWritten);
					_context.addTransferredBytes(chunk->bytesWritten);
					_context.publishProgressSnapshot();
				}
				if (chunk->readyToCommit)
					break;
			}

			if (!transferFailed)
			{
				if (!_context.checkpoint())
				{
					abortStagedSession(*session, source);
					_context.progress().currentEntryAbandoned();
					return NodeOutcome::Cancelled;
				}

				// The durability contract: flushing is required exactly where publication destroys the only
				// other copy - an authorized replacement, and every owned-source move (removal follows).
				const CommitDurability durability = (sourceAction == PublishedSourceAction::RemoveOwnedSource || replacement == ReplacementMode::ReplaceExistingFile)
					? CommitDurability::FlushBeforePublish : CommitDurability::NoFlush;
				if (auto committed = session->commit(replacement, durability); committed)
					sessionCompleted = true;
				else
					failure = mv(committed.error());
			}

			if (!sessionCompleted)
			{
				abortStagedSession(*session, source);
				// A publication-time AlreadyExists may re-enter resolution, but only when fresh inspection
				// proves the new collision really exists (nothing was published either way).
				if (failure->action == FailedAction::PublishDestination
					&& failure->filesystemError.category == FileErrorCategory::AlreadyExists)
				{
					if (const auto fresh = inspectEntry(destination); fresh && fresh->has_value())
						freshCollisionAtPublication = true;
				}
			}
		}

		if (sessionCompleted)
		{
			// Publication is the move's commit point: from here the committed cleanup segment runs to its end.
			if (sourceAction == PublishedSourceAction::RemoveOwnedSource)
				return removePublishedSourceWithPolicy(source, destination, makeWritableAuthorized);

			_context.addCompletedItems(1);
			_context.progress().itemCompleted();
			_context.progress().clearCurrentEntry();
			_context.publishProgressSnapshot();
			return NodeOutcome::Completed;
		}

		_context.progress().currentEntryAbandoned();
		if (freshCollisionAtPublication)
			return {};

		const auto decision = _context.resolveDecision(OperationIssue{ IssueKind::ActionFailed, source, {}, mv(*failure) });
		if (!decision || decision->action == DecisionAction::Cancel)
			return NodeOutcome::Cancelled;
		if (decision->action == DecisionAction::Skip)
		{
			_context.addSkippedItems(1);
			_context.progress().advanceWithoutTransfer(source.size, 1);
			_context.progress().clearCurrentEntry();
			_context.publishProgressSnapshot();
			return NodeOutcome::Skipped;
		}
		assert_debug_only(decision->action == DecisionAction::Retry); // A new staging session, from the top
	}
}

void CTransferExecutor::abortStagedSession(CStagedFileCopy& session, const EntrySnapshot& source)
{
	if (auto aborted = session.abort(); !aborted)
		_context.recordWarning(OperationDiagnostic{ mv(aborted.error()), source, {} });
}

void CTransferExecutor::recordTimestampWarning(const EntrySnapshot& source, const CEntryPath& destination, const CEntryPath& failedPath, CFileSystemError error)
{
	_context.recordWarning(OperationDiagnostic{ FailureDetails{ FailedAction::PreserveDirectoryTimestamps, failedPath, mv(error) },
		source, EntrySnapshot{ destination, OperationEntryKind::Directory, 0 } });
}

void CTransferExecutor::accountSkippedSubtree(const SourceNode& node)
{
	_context.addSkippedItems(node.subtreeItems);
	_context.progress().advanceWithoutTransfer(node.subtreeBytes, node.subtreeItems);
	_context.publishProgressSnapshot();
}

void CTransferExecutor::accountAlreadySatisfiedSubtree(const SourceNode& node)
{
	_context.addAlreadySatisfiedItems(node.subtreeItems);
	_context.progress().advanceWithoutTransfer(node.subtreeBytes, node.subtreeItems);
	_context.publishProgressSnapshot();
}
