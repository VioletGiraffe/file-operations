#pragma once

#include "fileoperationtypes.h"


#include <variant>

class COperationExecutionContext;

enum class SourceOwnership : uint8_t
{
	Owned,
	BorrowedThroughDirectoryLink // Reached via a directory link: materialized at the destination, never removed by move cleanup
};

// What a transfer does with a link node.
enum class LinkTreatment : uint8_t
{
	None,        // Not a link, or a permanent-delete manifest
	Recreate,    // A link storing the same target; no bytes and no children
	Materialize, // What it points to: a file link's bytes, or a directory link's content as its children
	Skip         // Left at the source: the destination cannot hold it, and the user chose to skip such links
};

// One node of the source manifest. Planning and presentation data only, never permission to assume the
// filesystem is unchanged: every mutation revalidates what it needs immediately before acting.
struct SourceNode
{
	EntrySnapshot entry;
	std::vector<SourceNode> children;
	uint64_t subtreeBytes = 0; // The bytes the transfer streams for this node plus all descendants
	size_t subtreeItems = 0;   // This node plus all descendants
	SourceOwnership ownership = SourceOwnership::Owned;
	LinkTreatment linkTreatment = LinkTreatment::None;
};

// The manifest is O(number of entries), so per-node size multiplies into every large tree. Accidental
// growth must fail the build; a deliberate layout change updates these consciously. The vector's size is
// measured, not assumed: MSVC debug builds grow it by an iterator-debugging pointer.
static_assert(sizeof(void*) != 8 || sizeof(EntrySnapshot) == 40);
static_assert(sizeof(void*) != 8 || sizeof(SourceNode) == sizeof(EntrySnapshot) + sizeof(std::vector<SourceNode>) + 24);

// Tree construction is operation-specific.
enum class SourceTreeBuildMode
{
	MaterializingTransfer,  // Copy / copy-based move: directory links are traversed (their content borrowed), file links carry followed target sizes
	LinkPreservingTransfer, // Copy / move that recreates links: every link is a leaf of no bytes, whatever it points to
	PermanentDelete         // Links of both kinds are leaf entries; nothing is ever followed
};

[[nodiscard]] SourceTreeBuildMode transferBuildMode(LinkTransferMode linkMode) noexcept;

struct ScanCancelled {};

// SourceNode: the complete manifest for the root; OperationDiagnostic: the entry that could not be
// scanned plus the structured error (the build never returns a partial tree); ScanCancelled: cancellation
// won at a checkpoint.
using SourceTreeResult = std::variant<SourceNode, OperationDiagnostic, ScanCancelled>;

// Builds the manifest for one root whose fresh snapshot the caller already holds. Publishes Scanning
// progress through the context - the current root's discovered count with all totals absent - and honors
// its cancellation checkpoints. Directory-link cycles normally terminate via entry-plus-mounted-view identities
// held for the active recursion branch only. This lets a Linux bind-mounted view of the same directory be traversed
// once when its descendant mount topology may differ; returning to the same view terminates the cycle. When stable
// entry identity is unavailable, the fixed traversal limits provide the fallback. No identity is stored in the
// result. Trees deeper than 300 levels or requiring more than 2,000,000 visited nodes fail before a manifest is returned.
[[nodiscard]] SourceTreeResult buildSourceTree(COperationExecutionContext& context, EntrySnapshot root, SourceTreeBuildMode mode);

#ifdef FILE_OPERATIONS_TEST_HOOKS
enum class DirectoryLinkIdentityModeForTesting
{
	Native,
	Unavailable
};

// Exercises production limit handling with small fixtures; production callers always use the fixed safety limits.
[[nodiscard]] SourceTreeResult buildSourceTreeForTesting(COperationExecutionContext& context, EntrySnapshot root, SourceTreeBuildMode mode,
	size_t maximumDepth, size_t maximumNodeCount, DirectoryLinkIdentityModeForTesting directoryLinkIdentityMode = DirectoryLinkIdentityModeForTesting::Native);
#endif
