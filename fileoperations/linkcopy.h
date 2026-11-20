#pragma once

#include "fileoperationtypes.h"


#include <expected>
#include <optional>

struct LinkCopyFailure
{
	FailureDetails primaryFailure;
	std::optional<FailureDetails> cleanupFailure; // The temporary link could not be removed after the primary failure
};

struct LinkCopyOutcome
{
	std::optional<FailureDetails> metadataFailure; // The link was published without its times or attributes
};

// Copies the link at source as itself: a link storing the same target, created under a hidden temporary sibling name of
// destination, given source's own times and attributes, then published by one rename. A PublishDestination failure keeps
// the rename's own category, so an AlreadyExists there reports a destination entry that appeared after resolution.
[[nodiscard]] std::expected<LinkCopyOutcome, LinkCopyFailure> copyLinkAsItself(const CEntryPath& source, const CEntryPath& destination,
	ReplacementMode replacement);

// Creates a link of this type in folder under a temporary name and removes it again: a CreateLink failure of category
// Unsupported means folder cannot hold such links. A CleanupStaging failure means the probe link was left behind.
[[nodiscard]] std::expected<void, FailureDetails> probeLinkCreation(const CEntryPath& folder, thin_io::link_type type);
