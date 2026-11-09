#include "fileoperationtypes.h"

// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()
#include "math/math.hpp"


#include <algorithm>

namespace
{

// A panel's synthetic parent item ([..]) arrives as a directory path ending in "../". It is filtered on the raw
// text so selecting only that sentinel behaves like selecting no source, rather than like supplying an invalid path.
bool isSyntheticParentEntry(const QString& rawPath)
{
	QString path = rawPath;
#ifdef _WIN32
	if (path.endsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('\\')))
#else
	if (path.endsWith(QLatin1Char('/')))
#endif
		path.chop(1);

#ifdef _WIN32
	if (path.endsWith(QLatin1String("\\.."))) // A separator here; on POSIX "x\.." is one ordinary name
		return true;
#endif

	return path == QLatin1String("..") || path.endsWith(QLatin1String("/.."));
}

std::expected<std::vector<CEntryPath>, RequestValidationError> parseSourcePaths(const QStringList& rawSourcePaths)
{
	std::vector<CEntryPath> sources;
	sources.reserve(static_cast<size_t>(rawSourcePaths.size()));
	for (const QString& rawPath : rawSourcePaths)
	{
		if (isSyntheticParentEntry(rawPath))
			continue;

		auto parsed = parseOperationPath(rawPath);
		if (!parsed)
			return std::unexpected{ RequestValidationError::InvalidPath };
		if (parsed->isRoot())
			return std::unexpected{ RequestValidationError::RootSource };

		sources.push_back(mv(*parsed));
	}

	if (sources.empty())
		return std::unexpected{ RequestValidationError::NoSources };
	return sources;
}

} // namespace

std::expected<TransferRequest, RequestValidationError> makeTransferRequest(
	const TransferKind kind, const QStringList& rawSourcePaths, const DestinationIntent intent, const QString& rawDestinationPath)
{
	auto sources = parseSourcePaths(rawSourcePaths);
	if (!sources)
		return std::unexpected{ sources.error() };

	if (intent == DestinationIntent::ExactEntry && sources->size() != 1)
		return std::unexpected{ RequestValidationError::ExactEntryRequiresSingleSource };

	auto destination = parseOperationPath(rawDestinationPath);
	if (!destination)
		return std::unexpected{ RequestValidationError::InvalidPath };
	// A root can receive entries (IntoDirectory) but cannot itself be a proposed entry: it has no parent
	// for the resolver's rename loop and could never be published over.
	if (intent == DestinationIntent::ExactEntry && destination->isRoot())
		return std::unexpected{ RequestValidationError::InvalidPath };

	TransferRequest request{ kind, mv(*sources), DestinationSpec{ intent, mv(*destination) } };
	for (const RootTransferIntent& root : rootTransferIntents(request))
	{
		if (root.proposedDestination.isStrictDescendantOf(root.source))
			return std::unexpected{ RequestValidationError::DestinationInsideSource };
	}
	return request;
}

std::expected<PermanentDeleteRequest, RequestValidationError> makePermanentDeleteRequest(const QStringList& rawSourcePaths)
{
	auto sources = parseSourcePaths(rawSourcePaths);
	if (!sources)
		return std::unexpected{ sources.error() };

	return PermanentDeleteRequest{ mv(*sources) };
}

std::vector<RootTransferIntent> rootTransferIntents(const TransferRequest& request)
{
	std::vector<RootTransferIntent> intents;
	intents.reserve(request.sources.size());

	if (request.destination.intent == DestinationIntent::ExactEntry)
	{
		assert_debug_only(request.sources.size() == 1);
		intents.push_back(RootTransferIntent{ request.sources.front(), request.destination.path });
		return intents;
	}

	for (const CEntryPath& source : request.sources)
		intents.push_back(RootTransferIntent{ source, request.destination.path.child(source.name()) });
	return intents;
}

NodeOutcome aggregateChildOutcome(const NodeOutcome aggregate, const NodeOutcome child) noexcept
{
	if (aggregate == NodeOutcome::Cancelled || child == NodeOutcome::Cancelled)
		return NodeOutcome::Cancelled;
	if (aggregate == NodeOutcome::Failed || child == NodeOutcome::Failed)
		return NodeOutcome::Failed;
	if (child == NodeOutcome::Skipped || child == NodeOutcome::Partial)
		return NodeOutcome::Partial;
	return aggregate;
}

AllowedActions allowedActionsFor(const IssueKind kind)
{
	using enum DecisionAction;
	switch (kind)
	{
	case IssueKind::FileReplacement:
		return { Replace, Rename, Skip, Cancel };
	case IssueKind::RootDirectoryMerge:
		return { Merge, Rename, Skip, Cancel };
	case IssueKind::TypeMismatch:
		return { Rename, Skip, Cancel };
	case IssueKind::ActionFailed:
		return { Retry, Skip, Cancel };
	case IssueKind::ReadOnlySourceRemoval:
		return { MakeWritable, Retry, Skip, Cancel };
	case IssueKind::UnsupportedEntry:
		return { Skip, Cancel };
	}

	assert_unconditional_r("Unknown IssueKind");
	return {};
}

bool isActionRememberable(const IssueKind kind, const DecisionAction action)
{
	using enum DecisionAction;
	switch (kind)
	{
	case IssueKind::FileReplacement:
		return action == Replace || action == Skip;
	case IssueKind::RootDirectoryMerge:
		return action == Merge || action == Skip;
	case IssueKind::TypeMismatch:
	case IssueKind::ActionFailed:
	case IssueKind::UnsupportedEntry:
		return action == Skip;
	case IssueKind::ReadOnlySourceRemoval:
		return action == MakeWritable || action == Skip;
	}

	assert_unconditional_r("Unknown IssueKind");
	return false;
}

std::optional<DecisionAction> defaultActionFor(const IssueKind kind)
{
	using enum DecisionAction;
	switch (kind)
	{
	case IssueKind::FileReplacement: return Replace;
	case IssueKind::RootDirectoryMerge: return Merge;
	case IssueKind::ActionFailed: return Retry;
	case IssueKind::TypeMismatch: // Nothing to proceed with: an entry cannot replace one of a different type
	case IssueKind::ReadOnlySourceRemoval: // Overriding the read-only flag to delete the file must be deliberate
	case IssueKind::UnsupportedEntry: // Only Skip and Cancel exist, and neither should happen by reflex
		return {};
	}
	return {};
}

bool remainingMatchingScopeOffered(const DecisionRequest& request)
{
	return request.remainingMatchingScopeAllowed && std::ranges::any_of(request.allowedActions,
		[&request](const DecisionAction action) { return isActionRememberable(request.issue.kind, action); });
}

AllowedActions plannableActionsFor(const IssueKind kind)
{
	AllowedActions actions = allowedActionsFor(kind);
	std::erase_if(actions, [](const DecisionAction action) { return action == DecisionAction::Retry || action == DecisionAction::Cancel; });
	return actions;
}

SideComparison compareSides(const PlannedConflict& conflict)
{
	const OperationIssue& issue = conflict.issue;
	assert_and_return_r(issue.kind == IssueKind::FileReplacement && issue.destination, SideComparison::TimeUnknown);

	if (!conflict.sourceLastWrite.is_set() || !conflict.destinationLastWrite.is_set())
		return SideComparison::TimeUnknown;
	if (conflict.destinationLastWrite.seconds < conflict.sourceLastWrite.seconds)
		return SideComparison::DestinationOlder;
	if (conflict.destinationLastWrite.seconds > conflict.sourceLastWrite.seconds)
		return SideComparison::DestinationNewer;
	return issue.destination->size == issue.source.size ? SideComparison::SameSizeAndTime : SideComparison::SizeDiffers;
}

std::optional<DecisionAction> planWideActionFor(const PlannedConflict& conflict, const PlanWideAnswer answer)
{
	if (conflict.issue.kind == IssueKind::RootDirectoryMerge)
		return DecisionAction::Merge;
	if (conflict.issue.kind != IssueKind::FileReplacement)
		return {};

	switch (answer)
	{
	case PlanWideAnswer::ReplaceAll: return DecisionAction::Replace;
	case PlanWideAnswer::SkipAll: return DecisionAction::Skip;
	case PlanWideAnswer::ReplaceOlder:
		// Only the times decide: an unknown time, or the same time with a different size, is left for the user
		switch (compareSides(conflict))
		{
		case SideComparison::DestinationOlder: return DecisionAction::Replace;
		case SideComparison::DestinationNewer:
		case SideComparison::SameSizeAndTime: return DecisionAction::Skip;
		case SideComparison::SizeDiffers:
		case SideComparison::TimeUnknown: return {};
		}
	}
	return {};
}

PrimaryProgressUnit primaryProgressUnitFor(const FileOperationRequest& request) noexcept
{
	return std::holds_alternative<PermanentDeleteRequest>(request) ? PrimaryProgressUnit::Items : PrimaryProgressUnit::Bytes;
}

std::optional<int> overallPercent(const ProgressSnapshot& snapshot, const PrimaryProgressUnit unit)
{
	if (snapshot.phase == OperationPhase::Scanning)
		return {};

	const bool bytes = unit == PrimaryProgressUnit::Bytes;
	const uint64_t processed = bytes ? snapshot.bytesProcessed : snapshot.itemsProcessed;
	const std::optional<uint64_t> total = bytes ? snapshot.bytesTotal
		: (snapshot.itemsTotal ? std::optional<uint64_t>{ *snapshot.itemsTotal } : std::nullopt);
	if (!total || *total == 0)
		return {};
	return Math::round<int>(static_cast<double>(processed) * 100.0 / static_cast<double>(*total));
}

std::optional<int> currentEntryPercent(const ProgressSnapshot& snapshot)
{
	if (!snapshot.currentEntryBytesTotal || *snapshot.currentEntryBytesTotal == 0)
		return {};
	return Math::round<int>(static_cast<double>(snapshot.currentEntryBytesProcessed) * 100.0 / static_cast<double>(*snapshot.currentEntryBytesTotal));
}
