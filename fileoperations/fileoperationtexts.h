#pragma once

#include "fileoperations/fileoperationtypes.h"
#include "fileoperations/inlinerename.h"
#include "fileoperations/newnamecheck.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QObject>
#include <QString>
#include <QStringBuilder>
#include <QStringList>
RESTORE_COMPILER_WARNINGS

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
// Why inlineRename did not rename, for a caller that never replaces: an occupied name is the reason. Empty for Renamed and NothingToDo.
[[nodiscard]] inline QString inlineRenameFailureText(const InlineRenameResult& result);
// Why makeTransferRequest() or makePermanentDeleteRequest() refused a request, phrased for the user.
[[nodiscard]] inline QString requestValidationErrorText(RequestValidationError error);
// The completion text: the counters, then the bounded representative diagnostics
[[nodiscard]] inline QString fileOperationSummaryText(const OperationSummary& summary, PromptOperation operation);
// Whether the finished operation left something the user must act on
[[nodiscard]] inline bool fileOperationNeedsAttention(const OperationSummary& summary) noexcept;

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
