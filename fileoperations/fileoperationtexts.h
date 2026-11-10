#pragma once

#include "fileoperations/entrycreation.h"
#include "fileoperations/fileoperationtypes.h"
#include "fileoperations/inlinerename.h"
#include "fileoperations/newnamecheck.h"
#include "fileoperations/usermessage.h"


// Submodule includes
#include "assert/advanced_assert.h"
#include "compiler/compiler_warnings_control.h"
#include "qtcore_helpers/displayformatting.hpp"


DISABLE_COMPILER_WARNINGS
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QString>
#include <QStringBuilder>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

#include <optional>
#include <variant>
#include <vector>

// The operation whose issue is being presented; selects consequence wording only
// (e.g. "Make writable and move" versus "and delete"), never the available policy.
enum class PromptOperation
{
	Copy,
	Move,
	Delete
};

// Shared user-facing formatting for file-operation types, used by the dialogs and by hosts so the wording has a
// single source.
[[nodiscard]] inline QString fileOperationEntryKindNoun(OperationEntryKind kind);
[[nodiscard]] inline QString fileOperationIssueText(const OperationIssue& issue, PromptOperation operation);
// The issue kind as a column heading: "Already exists", "Type mismatch", ...
[[nodiscard]] inline QString fileOperationIssueKindText(IssueKind kind);
[[nodiscard]] inline QString fileOperationActionLabel(DecisionAction action, PromptOperation operation);
[[nodiscard]] inline QString fileOperationFailedActionText(FailedAction action);
[[nodiscard]] inline QString fileSystemErrorText(const CFileSystemError& error);
// One line for a completion diagnostic: the affected entry, the attempted action, and the reason.
[[nodiscard]] inline QString fileOperationDiagnosticText(const OperationDiagnostic& diagnostic);
// Why a proposed name was refused, phrased for the user. Empty for NameRejection::None.
[[nodiscard]] inline QString newNameRejectionText(NameRejection rejection);
// Why inlineRename did not rename, as a phrase for a longer message. Empty for Renamed and NothingToDo.
[[nodiscard]] inline QString inlineRenameFailureText(const InlineRenameResult& result);
// Why inlineRename did not rename. nullopt when it renamed, had nothing to do, or requires a replacement: the caller asks first.
[[nodiscard]] inline std::optional<UserMessage> inlineRenameMessage(const InlineRenameResult& result, const QString& oldName, const QString& newName);
// The question for a ReplacementRequired result, answered yes or no
[[nodiscard]] inline UserMessage inlineRenameReplacementQuestion(const InlineRenameResult& result, const QString& newName);
// Why createNewFolder() or createNewFile() created nothing; nullopt when it created the entry. name is the text the user typed.
[[nodiscard]] inline std::optional<UserMessage> entryCreationMessage(const EntryCreationResult& result, const QString& name);
// Why makeTransferRequest() or makePermanentDeleteRequest() refused a request, phrased for the user.
[[nodiscard]] inline QString requestValidationErrorText(RequestValidationError error);
// The completion text: the counters, then the bounded representative diagnostics
[[nodiscard]] inline QString fileOperationSummaryText(const OperationSummary& summary, PromptOperation operation);
// Whether the finished operation left something the user must act on
[[nodiscard]] inline bool fileOperationNeedsAttention(const OperationSummary& summary) noexcept;

[[nodiscard]] inline PromptOperation promptOperationFor(const FileOperationRequest& request) noexcept;
// "Copy", "Move", "Delete"
[[nodiscard]] inline QString fileOperationName(PromptOperation operation);
// "Copying", "Moving", "Deleting"
[[nodiscard]] inline QString fileOperationProgressVerb(PromptOperation operation);
// The deepest folder holding every source; nullopt when the sources share no root
[[nodiscard]] inline std::optional<CEntryPath> commonSourceFolder(const std::vector<CEntryPath>& sources);
// "from X to Y", X being commonSourceFolder. Without a common folder: "to Y" for a transfer, empty for a deletion.
[[nodiscard]] inline QString fileOperationRouteText(const FileOperationRequest& request);
// One line: the running item count while scanning, then the verb with the speed and the time remaining
[[nodiscard]] inline QString fileOperationProgressStatusText(const ProgressSnapshot& snapshot, PromptOperation operation, PrimaryProgressUnit unit);
// The entry's kind, its size where it has one, and its modification time. Reads the time from the filesystem.
[[nodiscard]] inline QString fileOperationEntryDetailsText(const EntrySnapshot& entry);
// The failure the issue carries, and the affected entry where it is not the source. Empty for an issue without a failure.
[[nodiscard]] inline QString fileOperationIssueFailureText(const OperationIssue& issue);
// What cancelling leaves behind, for a request whose item is already published. Empty for every other request.
[[nodiscard]] inline QString fileOperationCancelConsequencesText(const DecisionRequest& request);
// The label of the RemainingMatchingIssues option in the kind's prompt
[[nodiscard]] inline QString fileOperationScopeLabel(IssueKind kind);
[[nodiscard]] inline QString sideComparisonText(SideComparison comparison);

// Why newName cannot rename an entry named currentName; nullopt when it can.
[[nodiscard]] inline std::optional<UserMessage> renameRefusal(const QString& newName, const QString& currentName);

QString fileOperationEntryKindNoun(const OperationEntryKind kind)
{
	using enum OperationEntryKind;
	switch (kind)
	{
	case RegularFile: return QObject::tr("file");
	case Directory: return QObject::tr("folder");
	case FileLink: return QObject::tr("link to a file");
	case DirectoryLink: return QObject::tr("link to a folder");
	case Other: return QObject::tr("special entry");
	case Unknown: return QObject::tr("filesystem entry of unknown type");
	}
	return {};
}

QString fileOperationIssueText(const OperationIssue& issue, const PromptOperation operation)
{
	// Kinds that require a destination snapshot are guarded anyway: this formats, it does not validate.
	const OperationEntryKind destinationKind = issue.destination ? issue.destination->kind : OperationEntryKind::RegularFile;

	using enum IssueKind;
	switch (issue.kind)
	{
	case FileReplacement:
		return QObject::tr("The destination %1 already exists.").arg(fileOperationEntryKindNoun(destinationKind));
	case RootDirectoryMerge:
		return QObject::tr("The destination folder already exists. Merge the source folder's contents into it?");
	case TypeMismatch:
		return QObject::tr("The source is a %1, but the existing destination is a %2. An entry cannot replace an entry of a different type.")
			.arg(fileOperationEntryKindNoun(issue.source.kind), fileOperationEntryKindNoun(destinationKind));
	case ActionFailed:
		return issue.failure ? QObject::tr("%1 failed.").arg(fileOperationFailedActionText(issue.failure->action)) : QObject::tr("The operation failed.");
	case ReadOnlySourceRemoval:
		return operation == PromptOperation::Move
			? QObject::tr("The source file is read-only, and moving it requires removing the source.")
			: QObject::tr("The file is read-only.");
	case UnsupportedEntry:
		return QObject::tr("This entry is not a file or a folder (a device, pipe, or socket) and cannot be %1.")
			.arg(operation == PromptOperation::Move ? QObject::tr("moved") : QObject::tr("copied"));
	}

	return {};
}

QString fileOperationIssueKindText(const IssueKind kind)
{
	using enum IssueKind;
	switch (kind)
	{
	case FileReplacement: return QObject::tr("Already exists");
	case RootDirectoryMerge: return QObject::tr("Folder exists");
	case TypeMismatch: return QObject::tr("Type mismatch");
	case ActionFailed: return QObject::tr("Failed");
	case ReadOnlySourceRemoval: return QObject::tr("Read-only source");
	case UnsupportedEntry: return QObject::tr("Unsupported entry");
	}
	return {};
}

QString fileOperationActionLabel(const DecisionAction action, const PromptOperation operation)
{
	using enum DecisionAction;
	switch (action)
	{
	case Skip: return QObject::tr("Skip");
	case Replace: return QObject::tr("Replace");
	case Merge: return QObject::tr("Merge");
	case MakeWritable:
		return operation == PromptOperation::Move ? QObject::tr("Make writable and move") : QObject::tr("Make writable and delete");
	case Rename: return QObject::tr("Rename");
	case Retry: return QObject::tr("Retry");
	case Cancel: return QObject::tr("Cancel");
	}
	return {};
}

QString fileOperationFailedActionText(const FailedAction action)
{
	using enum FailedAction;
	switch (action)
	{
	case InspectSource: return QObject::tr("Reading the source entry's properties");
	case InspectDestination: return QObject::tr("Reading the destination entry's properties");
	case ReadSource: return QObject::tr("Reading the source file");
	case CreateDestinationDirectory: return QObject::tr("Creating the destination folder");
	case PrepareStagingFile: return QObject::tr("Creating the temporary destination file");
	case WriteDestination: return QObject::tr("Writing the destination file");
	case PreserveFileMetadata: return QObject::tr("Preserving the file's attributes");
	case PublishDestination: return QObject::tr("Finalizing the destination file");
	case RenameEntry: return QObject::tr("Moving the entry into place");
	case MakeWritable: return QObject::tr("Making the entry writable");
	case RemoveEntry: return QObject::tr("Removing the entry");
	case RemovePublishedMoveSource: return QObject::tr("Removing the moved entry's source");
	case CleanupStaging: return QObject::tr("Removing the temporary destination file");
	case PreserveDirectoryTimestamps: return QObject::tr("Preserving the folder's timestamps");
	}
	return {};
}

QString fileSystemErrorText(const CFileSystemError& error)
{
	using enum FileErrorCategory;
	QString reason;
	switch (error.category)
	{
	case NotFound: reason = QObject::tr("the entry does not exist"); break;
	case AlreadyExists: reason = QObject::tr("an entry with this name already exists"); break;
	case CrossDevice: reason = QObject::tr("the destination is on a different volume"); break;
	case ReadOnly: reason = QObject::tr("the entry is read-only"); break;
	case PermissionDenied: reason = QObject::tr("access denied"); break;
	case NotEnoughSpace: reason = QObject::tr("not enough free space"); break;
	case Unsupported: reason = QObject::tr("the operation is not supported here"); break;
	case IoFailure: reason = QObject::tr("an input/output error occurred"); break;
	}

	if (!error.diagnostic.isEmpty())
		reason += QLatin1String(" (") % error.diagnostic % QLatin1Char(')');
	else if (error.nativeCode != 0)
		reason += QObject::tr(" (error code %1)").arg(error.nativeCode);
	return reason;
}

QString fileOperationDiagnosticText(const OperationDiagnostic& diagnostic)
{
	// The path goes last because the summary label elides these lines in the middle: the file name survives there.
	return fileOperationFailedActionText(diagnostic.failure.action) % QLatin1String(" - ")
		% fileSystemErrorText(diagnostic.failure.filesystemError) % QLatin1String(": ")
		% QDir::toNativeSeparators(diagnostic.failure.path.value());
}

QString newNameRejectionText(const NameRejection rejection)
{
	using enum NameRejection;
	switch (rejection)
	{
	case None: return {};
	case Empty: return QObject::tr("The name is empty.");
	case WhitespaceOnly: return QObject::tr("The name consists only of spaces.");
	case Separator: return QObject::tr("A name cannot contain a path separator.");
	case DotComponent: return QObject::tr("\".\" and \"..\" refer to existing folders and cannot be used as names.");
	case InvalidCharacter: return QObject::tr("The name contains a character that cannot be used in a file or folder name.");
	case ReservedDeviceName: return QObject::tr("This name is reserved by Windows.");
	case TrailingDotOrSpace:
		return QObject::tr("A name cannot end with a dot or a space: Windows would not find the entry afterwards.");
	}
	return {};
}

QString inlineRenameFailureText(const InlineRenameResult& result)
{
	using enum InlineRenameStatus;
	switch (result.status)
	{
	case Renamed:
	case NothingToDo:
		return {};
	case ReplacementRequired:
	case Rejected:
		return fileSystemErrorText(CFileSystemError{ FileErrorCategory::AlreadyExists, 0, {} });
	case RejectedInvalidName:
		return newNameRejectionText(result.nameRejection);
	case Failed:
		return fileSystemErrorText(result.failure->filesystemError);
	}
	return {};
}

std::optional<UserMessage> inlineRenameMessage(const InlineRenameResult& result, const QString& oldName, const QString& newName)
{
	using enum InlineRenameStatus;
	switch (result.status)
	{
	case Renamed:
	case NothingToDo:
	case ReplacementRequired:
		return {};
	case Rejected:
		return UserMessage{ MessageSeverity::Information, QObject::tr("Item already exists"),
			QObject::tr("Cannot rename this %1 to \"%2\": a %3 with that name already exists.")
				.arg(fileOperationEntryKindNoun(*result.sourceKind), newName, fileOperationEntryKindNoun(*result.destinationKind)) };
	case RejectedInvalidName:
		return UserMessage{ MessageSeverity::Warning, QObject::tr("Cannot use this name"), newNameRejectionText(result.nameRejection) };
	case Failed:
	{
		QString text = QObject::tr("Failed to rename %1 to %2").arg(oldName, newName);
		if (result.failure)
			text.append(":\n" % fileSystemErrorText(result.failure->filesystemError) % '.');
		return UserMessage{ MessageSeverity::Error, QObject::tr("Renaming failed"), text };
	}
	}
	return {};
}

UserMessage inlineRenameReplacementQuestion(const InlineRenameResult& result, const QString& newName)
{
	assert_debug_only(result.status == InlineRenameStatus::ReplacementRequired);
	return { MessageSeverity::Warning, QObject::tr("Item already exists"),
		QObject::tr("The %1 %2 already exists, do you want to overwrite it?").arg(fileOperationEntryKindNoun(*result.destinationKind), newName) };
}

std::optional<UserMessage> entryCreationMessage(const EntryCreationResult& result, const QString& name)
{
	const QString noun = fileOperationEntryKindNoun(result.kind);
	using enum EntryCreationStatus;
	switch (result.status)
	{
	case Created:
		return {};
	case AlreadyExists:
		return UserMessage{ MessageSeverity::Warning, QObject::tr("Item already exists"), QObject::tr("The %1 %2 already exists.").arg(noun, name) };
	case RejectedInvalidName:
		return UserMessage{ MessageSeverity::Warning, QObject::tr("Cannot use this name"), newNameRejectionText(result.nameRejection) };
	case ParentMissing:
		return UserMessage{ MessageSeverity::Error, QObject::tr("Failed to create item"),
			QObject::tr("Cannot create the %1 %2: the folder it was to go in no longer exists.").arg(noun, name) };
	case Failed:
		return UserMessage{ MessageSeverity::Error, QObject::tr("Failed to create item"),
			QObject::tr("Failed to create the %1 %2: %3.").arg(noun, name, fileSystemErrorText(*result.error)) };
	}
	return {};
}

QString requestValidationErrorText(const RequestValidationError error)
{
	using enum RequestValidationError;
	switch (error)
	{
	case NoSources: return QObject::tr("No valid items are selected for the operation.");
	case InvalidPath: return QObject::tr("The source or destination path is not a valid absolute path.");
	case RootSource: return QObject::tr("A drive root cannot be copied, moved, or deleted.");
	case ExactEntryRequiresSingleSource: return QObject::tr("An exact destination entry can only be chosen for a single source.");
	case DestinationInsideSource: return QObject::tr("The destination cannot be inside a selected source.");
	}
	return {};
}

bool fileOperationNeedsAttention(const OperationSummary& summary) noexcept
{
	// Only diagnostics are worth a window the user has to dismiss. A clean run needs no acknowledgement,
	// and a cancelled one was their own doing. Failed without a failed item is a job that never got that
	// far - a scan failure - and must not vanish silently.
	return summary.status == CompletionStatus::Failed || summary.failedItems > 0 || summary.warningCount > 0;
}

QString fileOperationSummaryText(const OperationSummary& summary, const PromptOperation operation)
{
	const QString verb = operation == PromptOperation::Delete ? QObject::tr("deleted")
		: operation == PromptOperation::Move ? QObject::tr("moved") : QObject::tr("copied");

	QString headline;
	switch (summary.status)
	{
	case CompletionStatus::Completed: headline = QObject::tr("Operation finished."); break;
	case CompletionStatus::Cancelled: headline = QObject::tr("Operation cancelled."); break;
	case CompletionStatus::Failed: headline = QObject::tr("Operation failed."); break;
	}

	QStringList facts;
	if (summary.completedItems > 0)
		facts += QObject::tr("%1 items %2").arg(summary.completedItems).arg(verb);
	if (summary.skippedItems > 0)
		facts += QObject::tr("%1 skipped").arg(summary.skippedItems);
	if (summary.failedItems > 0)
		facts += QObject::tr("%1 failed").arg(summary.failedItems);
	if (summary.alreadySatisfiedItems > 0)
		facts += QObject::tr("%1 already up to date").arg(summary.alreadySatisfiedItems);
	if (summary.warningCount > 0)
		facts += QObject::tr("%1 warnings").arg(summary.warningCount);

	QString text = facts.isEmpty() ? headline : headline % QLatin1Char('\n') % facts.join(QLatin1String(", "));

	// The bounded representative diagnostics: failures carry the incomplete work, warnings the recoverable
	// conditions. Show failures first; they are what the user must act on.
	for (const OperationDiagnostic& failure : summary.representativeFailures)
		text += QLatin1Char('\n') % fileOperationDiagnosticText(failure);
	for (const OperationDiagnostic& warning : summary.representativeWarnings)
		text += QLatin1Char('\n') % fileOperationDiagnosticText(warning);

	return text;
}

PromptOperation promptOperationFor(const FileOperationRequest& request) noexcept
{
	if (const auto* transfer = std::get_if<TransferRequest>(&request))
		return transfer->kind == TransferKind::Move ? PromptOperation::Move : PromptOperation::Copy;
	return PromptOperation::Delete;
}

QString fileOperationName(const PromptOperation operation)
{
	switch (operation)
	{
	case PromptOperation::Copy: return QObject::tr("Copy");
	case PromptOperation::Move: return QObject::tr("Move");
	case PromptOperation::Delete: return QObject::tr("Delete");
	}
	return {};
}

QString fileOperationProgressVerb(const PromptOperation operation)
{
	switch (operation)
	{
	case PromptOperation::Copy: return QObject::tr("Copying");
	case PromptOperation::Move: return QObject::tr("Moving");
	case PromptOperation::Delete: return QObject::tr("Deleting");
	}
	return {};
}

std::optional<CEntryPath> commonSourceFolder(const std::vector<CEntryPath>& sources)
{
	assert_and_return_r(!sources.empty(), std::nullopt);

	CEntryPath folder = sources.front().parent();
	for (const CEntryPath& source : sources)
	{
		while (!source.isStrictDescendantOf(folder))
		{
			if (folder.isRoot())
				return std::nullopt;
			folder = folder.parent();
		}
	}
	return folder;
}

QString fileOperationRouteText(const FileOperationRequest& request)
{
	const auto nativePath = [](const CEntryPath& path) { return QDir::toNativeSeparators(path.value()); };

	const auto* transfer = std::get_if<TransferRequest>(&request);
	const std::optional<CEntryPath> sourceFolder = commonSourceFolder(transfer ? transfer->sources : std::get<PermanentDeleteRequest>(request).sources);
	if (!transfer)
		return sourceFolder ? QObject::tr("from %1").arg(nativePath(*sourceFolder)) : QString{};

	const QString destination = nativePath(transfer->destination.path);
	return sourceFolder ? QObject::tr("from %1 to %2").arg(nativePath(*sourceFolder), destination) : QObject::tr("to %1").arg(destination);
}

QString fileOperationProgressStatusText(const ProgressSnapshot& snapshot, const PromptOperation operation, const PrimaryProgressUnit unit)
{
	if (snapshot.phase == OperationPhase::Scanning)
		return QObject::tr("Scanning... %1 items found").arg(snapshot.itemsProcessed);

	const QString speed = unit == PrimaryProgressUnit::Bytes
		? QObject::tr("%1/s").arg(fileSizeToString(snapshot.primaryUnitsPerSecond))
		: QObject::tr("%1 items/s").arg(snapshot.primaryUnitsPerSecond);
	QString status = fileOperationProgressVerb(operation) % QLatin1String("  ") % speed;
	if (snapshot.secondsRemaining)
		status += QLatin1String(", ") % QObject::tr("%1 remaining").arg(secondsToTimeIntervalString(*snapshot.secondsRemaining));
	return status;
}

QString fileOperationEntryDetailsText(const EntrySnapshot& entry)
{
	QString details = fileOperationEntryKindNoun(entry.kind);
	if (entry.kind == OperationEntryKind::RegularFile || entry.kind == OperationEntryKind::FileLink)
		details += QLatin1String(", ") % fileSizeToString(entry.size);

	// The snapshot deliberately carries no timestamps
	const QFileInfo info{ entry.path.value() };
	if (info.exists())
		details += QObject::tr(", modified %1").arg(info.lastModified().toString(QStringLiteral("dd.MM.yyyy hh:mm")));
	return details;
}

QString fileOperationIssueFailureText(const OperationIssue& issue)
{
	if (!issue.failure)
		return {};

	// For ActionFailed fileOperationIssueText already names the attempted action; other kinds carry a raced failure and present it in full.
	QString text = issue.kind == IssueKind::ActionFailed
		? QObject::tr("Reason: %1").arg(fileSystemErrorText(issue.failure->filesystemError))
		: QObject::tr("%1 failed: %2").arg(fileOperationFailedActionText(issue.failure->action), fileSystemErrorText(issue.failure->filesystemError));
	if (issue.failure->path != issue.source.path)
		text += QLatin1Char('\n') % QObject::tr("Affected entry: %1").arg(QDir::toNativeSeparators(issue.failure->path.value()));
	return text;
}

QString fileOperationCancelConsequencesText(const DecisionRequest& request)
{
	// A request that disallows the remaining-matching scope is a committed-cleanup prompt: publication has already succeeded.
	if (request.remainingMatchingScopeAllowed)
		return {};
	return QObject::tr("Cancelling stops the operation: items already moved keep their new location, "
		"this item keeps both its source and its published destination, and the remaining items are left untouched.");
}

QString fileOperationScopeLabel(const IssueKind kind)
{
	using enum IssueKind;
	switch (kind)
	{
	case FileReplacement: return QObject::tr("Apply to all remaining file collisions");
	case RootDirectoryMerge: return QObject::tr("Apply to all remaining folder collisions");
	case TypeMismatch: return QObject::tr("Apply to all remaining type mismatches");
	// The remembered Skip covers any later failure, not only the currently named action - say so.
	case ActionFailed: return QObject::tr("Apply to any further failures, not only this one");
	case ReadOnlySourceRemoval: return QObject::tr("Apply to all remaining read-only items");
	case UnsupportedEntry: return QObject::tr("Apply to all remaining unsupported entries");
	}
	return {};
}

QString sideComparisonText(const SideComparison comparison)
{
	using enum SideComparison;
	switch (comparison)
	{
	case DestinationOlder: return QObject::tr("Destination older");
	case DestinationNewer: return QObject::tr("Destination newer");
	case SameSizeAndTime: return QObject::tr("Same size and time");
	case SizeDiffers: return QObject::tr("Same time, different size");
	case TimeUnknown: return QObject::tr("Already exists, time unknown");
	}
	return {};
}

std::optional<UserMessage> renameRefusal(const QString& newName, const QString& currentName)
{
	if (const NameRejection rejection = checkNewEntryName(newName); rejection != NameRejection::None)
		return UserMessage{ MessageSeverity::Warning, QObject::tr("Cannot use this name"), newNameRejectionText(rejection) };

	// An unchanged name is not a rename; an exact-case respell of the same name is, hence the case-sensitive compare.
	if (newName == currentName)
		return UserMessage{ MessageSeverity::Warning, QObject::tr("Nothing to rename"), QObject::tr("This is already the entry's name. Enter a different one to rename it.") };

	return {};
}
