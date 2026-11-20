#include "destinationresolver.h"

#include "cfilesystemmutator.h"
#include "coperationexecutioncontext.h"
#include "newnamecheck.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "lang/utils.hpp" // mv()

namespace
{

// sourceIsDirectoryLink: the source is published as a directory link, which is a directory entry on Windows.
bool replacementUnsupported(const OperationEntryKind destinationKind, [[maybe_unused]] const bool sourceIsDirectoryLink) noexcept
{
	if (destinationKind == OperationEntryKind::Directory)
		return true;

#ifdef _WIN32
	// POSIX rename() can atomically replace a directory symlink entry without touching its target. Windows
	// records directory symlinks and junctions with FILE_ATTRIBUTE_DIRECTORY, and MoveFileExW with
	// MOVEFILE_REPLACE_EXISTING refuses every such destination, and every such source. Publication renames
	// through that same primitive, so nothing could honor Replace for either on Windows.
	return destinationKind == OperationEntryKind::DirectoryLink || sourceIsDirectoryLink;
#else
	return false;
#endif
}

// False when either identity cannot be read.
bool linkResolvesToEntry(const CEntryPath& linkPath, const CEntryPath& entryPath)
{
	const auto target = readEntryIdentity(linkPath, thin_io::link_behavior::follow);
	const auto entry = readEntryIdentity(entryPath, thin_io::link_behavior::do_not_follow);
	return target && *target && entry && *entry && **target == **entry;
}

// One prompt for a collision-type issue, owning the Rename input handling: an invalid or missing new
// name re-asks rather than failing the node. Returns the respelled proposal for a usable Rename,
// or the chosen non-rename action (cancellation collapses to Cancel).
std::variant<CEntryPath, DecisionAction> promptForCollision(COperationExecutionContext& context, const OperationIssue& issue, const CEntryPath& proposed)
{
	for (;;)
	{
		const auto decision = context.resolveDecision(issue);
		if (!decision)
			return DecisionAction::Cancel;
		if (decision->action != DecisionAction::Rename)
			return decision->action;

		if (decision->newName && checkNewEntryName(*decision->newName) == NameRejection::None)
			return proposed.parent().child(*decision->newName);
	}
}

// Retry/Skip/Cancel for a resolution-time inspection failure; nullopt = Retry.
std::optional<DestinationChoice> resolveInspectionFailure(COperationExecutionContext& context, const EntrySnapshot& source,
	const CEntryPath& destination, CFileSystemError error)
{
	const auto decision = context.resolveDecision(
		OperationIssue{ IssueKind::ActionFailed, source, {}, FailureDetails{ FailedAction::InspectDestination, destination, mv(error) } });
	if (!decision || decision->action == DecisionAction::Cancel)
		return DestinationChoice{ CancelOperation{} };
	if (decision->action == DecisionAction::Skip)
		return DestinationChoice{ SkipNode{} };

	assert_debug_only(decision->action == DecisionAction::Retry);
	return {};
}

// The collision loop for a source that arrives as a single entry: a file, or with recreatedLink a link recreated as itself.
DestinationChoice resolveEntryDestination(COperationExecutionContext& context, const EntrySnapshot& source, CEntryPath proposed,
	const bool recreatedLink)
{
	for (;;)
	{
		const auto destination = inspectEntry(proposed);
		if (!destination) [[unlikely]]
		{
			if (auto verdict = resolveInspectionFailure(context, source, proposed, destination.error()))
				return mv(*verdict);
			continue;
		}

		if (!destination->has_value())
			return UseDestination{ mv(proposed), ReplacementMode::RequireAbsent };

		const auto sameEntry = checkSameEntry(source.path, proposed, thin_io::link_behavior::do_not_follow);
		if (!sameEntry) [[unlikely]]
		{
			if (auto verdict = resolveInspectionFailure(context, source, proposed, sameEntry.error()))
				return mv(*verdict);
			continue;
		}
		if (*sameEntry == SameEntryVerdict::Same || (recreatedLink && linkResolvesToEntry(source.path, proposed)))
			return AlreadySatisfied{};

		const EntrySnapshot& destinationEntry = **destination;
		const bool cannotReplace = replacementUnsupported(destinationEntry.kind, recreatedLink && source.kind == OperationEntryKind::DirectoryLink);
		const OperationIssue issue{ cannotReplace ? IssueKind::TypeMismatch : IssueKind::FileReplacement, source, destinationEntry, {}, recreatedLink };

		auto outcome = promptForCollision(context, issue, proposed);
		if (auto* respelled = std::get_if<CEntryPath>(&outcome))
		{
			proposed = mv(*respelled);
			continue;
		}

		switch (std::get<DecisionAction>(outcome))
		{
		case DecisionAction::Replace:
			assert_debug_only(!cannotReplace);
			return UseDestination{ mv(proposed), ReplacementMode::ReplaceExistingFile };
		case DecisionAction::Skip:
			return SkipNode{};
		default:
			assert_debug_only(std::get<DecisionAction>(outcome) == DecisionAction::Cancel);
			return CancelOperation{};
		}
	}
}

} // namespace

DestinationChoice resolveFileDestination(COperationExecutionContext& context, const EntrySnapshot& source, CEntryPath proposed)
{
	assert_debug_only(source.kind == OperationEntryKind::RegularFile || source.kind == OperationEntryKind::FileLink);
	return resolveEntryDestination(context, source, mv(proposed), false);
}

DestinationChoice resolveLinkDestination(COperationExecutionContext& context, const EntrySnapshot& source, CEntryPath proposed)
{
	assert_debug_only(isLinkKind(source.kind));
	return resolveEntryDestination(context, source, mv(proposed), true);
}

DestinationChoice resolveDirectoryDestination(COperationExecutionContext& context, const EntrySnapshot& source,
	CEntryPath proposed, const TransferNodePosition position)
{
	assert_debug_only(source.kind == OperationEntryKind::Directory || source.kind == OperationEntryKind::DirectoryLink);

	for (;;)
	{
		const auto destination = inspectEntry(proposed);
		if (!destination) [[unlikely]]
		{
			if (auto verdict = resolveInspectionFailure(context, source, proposed, destination.error()))
				return mv(*verdict);
			continue;
		}

		if (!destination->has_value())
			return UseDestination{ mv(proposed), ReplacementMode::RequireAbsent };

		const auto sameEntry = checkSameEntry(source.path, proposed, thin_io::link_behavior::do_not_follow);
		if (!sameEntry) [[unlikely]]
		{
			if (auto verdict = resolveInspectionFailure(context, source, proposed, sameEntry.error()))
				return mv(*verdict);
			continue;
		}
		if (*sameEntry == SameEntryVerdict::Same)
			return AlreadySatisfied{};

		const EntrySnapshot& destinationEntry = **destination;
		if (destinationEntry.kind == OperationEntryKind::Directory)
		{
			// Structurally part of an already accepted merge: descendants collide silently.
			if (position == TransferNodePosition::Descendant)
				return MergeDirectory{ mv(proposed) };

			auto outcome = promptForCollision(context, OperationIssue{ IssueKind::RootDirectoryMerge, source, destinationEntry, {} }, proposed);
			if (auto* respelled = std::get_if<CEntryPath>(&outcome))
			{
				proposed = mv(*respelled);
				continue;
			}

			switch (std::get<DecisionAction>(outcome))
			{
			case DecisionAction::Merge:
				return MergeDirectory{ mv(proposed) };
			case DecisionAction::Skip:
				return SkipNode{};
			default:
				assert_debug_only(std::get<DecisionAction>(outcome) == DecisionAction::Cancel);
				return CancelOperation{};
			}
		}

		// Any non-directory entry - including a directory link, which must never be merged through:
		// materializing into it would write inside its target.
		auto outcome = promptForCollision(context, OperationIssue{ IssueKind::TypeMismatch, source, destinationEntry, {} }, proposed);
		if (auto* respelled = std::get_if<CEntryPath>(&outcome))
		{
			proposed = mv(*respelled);
			continue;
		}

		if (std::get<DecisionAction>(outcome) == DecisionAction::Skip)
			return SkipNode{};
		assert_debug_only(std::get<DecisionAction>(outcome) == DecisionAction::Cancel);
		return CancelOperation{};
	}
}
