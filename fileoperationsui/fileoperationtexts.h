#pragma once

#include "fileoperations/fileoperationtypes.h"
#include "fileoperations/newnamecheck.h"


class QString;

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
[[nodiscard]] QString fileOperationEntryKindNoun(OperationEntryKind kind);
[[nodiscard]] QString fileOperationIssueText(const OperationIssue& issue, PromptOperation operation);
[[nodiscard]] QString fileOperationActionLabel(DecisionAction action, PromptOperation operation);
[[nodiscard]] QString fileOperationFailedActionText(FailedAction action);
[[nodiscard]] QString fileSystemErrorText(const CFileSystemError& error);
// One line for a completion diagnostic: the affected entry, the attempted action, and the reason.
[[nodiscard]] QString fileOperationDiagnosticText(const OperationDiagnostic& diagnostic);
// Why a proposed name was refused, phrased for the user. Empty for NameRejection::None.
[[nodiscard]] QString newNameRejectionText(NameRejection rejection);
// Why makeTransferRequest() or makePermanentDeleteRequest() refused a request, phrased for the user.
[[nodiscard]] QString requestValidationErrorText(RequestValidationError error);
