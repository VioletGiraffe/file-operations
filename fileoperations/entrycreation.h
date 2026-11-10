#pragma once

#include "centrypath.h"
#include "cfilesystemmutator.h"
#include "fileoperationtypes.h"
#include "newnamecheck.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <utility>

// The synchronous commands that create one new entry from a name the user typed, the counterparts of inlineRename()

enum class EntryCreationStatus
{
	Created,
	AlreadyExists,       // An entry is already at the path; for a chain, at its last folder
	RejectedInvalidName, // The name, or a component of the chain, is not a usable entry name
	ParentMissing,       // The parent is gone or is no longer a folder
	Failed               // A native inspection or creation failed
};

struct EntryCreationResult
{
	EntryCreationStatus status;
	OperationEntryKind kind;                           // What was to be created: Directory or RegularFile
	std::optional<CEntryPath> createdInParent;         // Created: the new entry directly inside the parent, a chain's first folder
	std::optional<CFileSystemError> error;             // Failed
	NameRejection nameRejection = NameRejection::None; // RejectedInvalidName
};

// Creates the folder relativePath in parent, with any missing folders along it: "a/b/c" is a chain. Separators as in checkNewEntryPath().
[[nodiscard]] inline EntryCreationResult createNewFolder(const CEntryPath& parent, const QString& relativePath);
// Creates an empty regular file in parent. Never opens or truncates an existing entry.
[[nodiscard]] inline EntryCreationResult createNewFile(const CEntryPath& parent, const QString& name);

EntryCreationResult createNewFolder(const CEntryPath& parent, const QString& relativePath)
{
	using enum EntryCreationStatus;
	static constexpr OperationEntryKind kind = OperationEntryKind::Directory;

	if (const NameRejection rejection = checkNewEntryPath(relativePath); rejection != NameRejection::None)
		return { .status = RejectedInvalidName, .kind = kind, .nameRejection = rejection };

	// createDirectories() would recreate a missing parent
	const auto parentEntry = inspectEntry(parent);
	if (!parentEntry)
		return { .status = Failed, .kind = kind, .error = parentEntry.error() };
	if (!parentEntry->has_value()
		|| ((*parentEntry)->kind != OperationEntryKind::Directory && (*parentEntry)->kind != OperationEntryKind::DirectoryLink))
		return { .status = ParentMissing, .kind = kind };

	// Not empty: checkNewEntryPath() rejects a path without a name
	const QStringList components = QDir::fromNativeSeparators(relativePath).split('/', Qt::SkipEmptyParts);
	CEntryPath path = parent;
	for (const QString& component : components)
		path = path.child(component);

	const auto outcome = CFileSystemMutator::createDirectories(path);
	if (!outcome)
		return { .status = Failed, .kind = kind, .error = outcome.error() };
	if (*outcome == DirectoryCreationOutcome::FinalEntryAlreadyExisted)
		return { .status = AlreadyExists, .kind = kind };
	return { .status = Created, .kind = kind, .createdInParent = parent.child(components.front()) };
}

EntryCreationResult createNewFile(const CEntryPath& parent, const QString& name)
{
	using enum EntryCreationStatus;
	static constexpr OperationEntryKind kind = OperationEntryKind::RegularFile;

	if (const NameRejection rejection = checkNewEntryName(name); rejection != NameRejection::None)
		return { .status = RejectedInvalidName, .kind = kind, .nameRejection = rejection };

	const CEntryPath path = parent.child(name);
	if (auto created = CFileSystemMutator::createEmptyFile(path); !created)
	{
		if (created.error().category == FileErrorCategory::AlreadyExists)
			return { .status = AlreadyExists, .kind = kind };
		return { .status = Failed, .kind = kind, .error = std::move(created.error()) };
	}
	return { .status = Created, .kind = kind, .createdInParent = path };
}
