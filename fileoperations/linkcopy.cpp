#include "linkcopy.h"

#include "cfilesystemmutator.h"
#include "thiniobridge.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "lang/utils.hpp" // mv()


DISABLE_COMPILER_WARNINGS
#include <QStringBuilder>
#include <QUuid>
RESTORE_COMPILER_WARNINGS

namespace
{

// Creates the link in folder under a fresh dot-prefixed name. An entry already holding a candidate name costs another attempt.
std::expected<CEntryPath, CFileSystemError> createTemporaryLink(const CEntryPath& folder, const thin_io::link_definition& definition, const QLatin1String prefix)
{
	static constexpr int maximumAttempts = 10;
	for (int attempt = 0; attempt < maximumAttempts; ++attempt)
	{
		CEntryPath candidate = folder.child(prefix % QUuid::createUuid().toString(QUuid::WithoutBraces) % QStringLiteral(".tmp"));
		auto created = CFileSystemMutator::createLink(candidate, definition);
		if (created)
			return candidate;
		if (created.error().category != FileErrorCategory::AlreadyExists)
			return std::unexpected(mv(created.error()));
	}

	return std::unexpected(CFileSystemError{ FileErrorCategory::AlreadyExists, 0, QStringLiteral("Could not reserve a unique temporary name for the link") });
}

} // namespace

std::expected<LinkCopyOutcome, LinkCopyFailure> copyLinkAsItself(const CEntryPath& source, const CEntryPath& destination, const ReplacementMode replacement)
{
	assert_debug_only(!destination.isRoot());

	auto definition = readLinkDefinition(source);
	if (!definition)
		return std::unexpected(LinkCopyFailure{ FailureDetails{ FailedAction::ReadLink, source, mv(definition.error()) }, {} });

	auto temporary = createTemporaryLink(destination.parent(), *definition, QLatin1String(".file-commander-link-"));
	if (!temporary)
		return std::unexpected(LinkCopyFailure{ FailureDetails{ FailedAction::CreateLink, destination, mv(temporary.error()) }, {} });

	LinkCopyOutcome outcome;
	const auto sourceNative = thinIoPath(source);
	if (const auto sourceStatus = thin_io::get_entry_status(nativeCStr(sourceNative), thin_io::link_behavior::do_not_follow); !sourceStatus)
		outcome.metadataFailure = FailureDetails{ FailedAction::PreserveLinkMetadata, source, makeFileSystemError(sourceStatus.error().native_code) };
	else if (auto applied = CFileSystemMutator::applyLinkMetadata(*temporary, *sourceStatus); !applied)
		outcome.metadataFailure = FailureDetails{ FailedAction::PreserveLinkMetadata, destination, mv(applied.error()) };

	if (auto published = CFileSystemMutator::renameEntry(*temporary, destination, replacement); !published)
	{
		LinkCopyFailure failure{ FailureDetails{ FailedAction::PublishDestination, destination, mv(published.error()) }, {} };
		if (auto removed = CFileSystemMutator::removeEntry(*temporary); !removed)
			failure.cleanupFailure = FailureDetails{ FailedAction::CleanupStaging, *temporary, mv(removed.error()) };
		return std::unexpected(mv(failure));
	}

	return outcome;
}

std::expected<void, FailureDetails> probeLinkCreation(const CEntryPath& folder, const thin_io::link_type type)
{
	// Dangling targets: the link only has to exist. A junction needs an absolute one.
	thin_io::link_definition definition{ .type = type };
#ifdef _WIN32
	definition.target = type == thin_io::link_type::junction ? std::wstring{ LR"(C:\file-commander-link-probe)" } : std::wstring{ L"probe" };
#else
	definition.target = "probe";
#endif

	auto temporary = createTemporaryLink(folder, definition, QLatin1String(".file-commander-link-probe-"));
	if (!temporary)
		return std::unexpected(FailureDetails{ FailedAction::CreateLink, folder, mv(temporary.error()) });
	if (auto removed = CFileSystemMutator::removeEntry(*temporary); !removed)
		return std::unexpected(FailureDetails{ FailedAction::CleanupStaging, *temporary, mv(removed.error()) });
	return {};
}
