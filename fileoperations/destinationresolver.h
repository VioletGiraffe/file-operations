#pragma once

#include "fileoperationtypes.h"


#include <variant>

class COperationExecutionContext;

// The resolved disposition for one node. Both rename-based move and copy-based transfer consume the
// same choice; the executor decides the mechanism.

struct UseDestination
{
	CEntryPath path;
	ReplacementMode replacement;
};

struct MergeDirectory
{
	CEntryPath path;
};

struct AlreadySatisfied {}; // Internal outcome for summary accounting - never a prompt, never a mutation
struct SkipNode {};
struct CancelOperation {};

using DestinationChoice = std::variant<UseDestination, MergeDirectory, AlreadySatisfied, SkipNode, CancelOperation>;

// Selected-root position is known at the recursive call site; descendants of an accepted merge collide
// silently rather than re-asking per directory.
enum class TransferNodePosition
{
	SelectedRoot,
	Descendant
};

// Destination resolution: the single owner of the collision matrix, separate from traversal and mutation.
// Freshly inspects the proposed entry, silently detects the same-filesystem-object case, and owns the
// Rename loop (validate name, respell next to the proposal, reinspect, repeat until a usable choice,
// skip, or cancel). Only a real directory counts as a directory here: destination links of either kind
// are entries and are replaced or refused at entry level, never followed.

[[nodiscard]] DestinationChoice resolveFileDestination(COperationExecutionContext& context, const EntrySnapshot& source, CEntryPath proposed);

// For a link recreated as itself. It collides like a file: it never merges into a destination directory.
// On Windows a directory link is a directory entry, so it can neither replace nor be replaced.
// The entry the link resolves to is AlreadySatisfied: replacing it would leave a link to itself in place of its content.
[[nodiscard]] DestinationChoice resolveLinkDestination(COperationExecutionContext& context, const EntrySnapshot& source, CEntryPath proposed);

[[nodiscard]] DestinationChoice resolveDirectoryDestination(COperationExecutionContext& context, const EntrySnapshot& source,
	CEntryPath proposed, TransferNodePosition position);
