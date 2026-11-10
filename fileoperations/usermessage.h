#pragma once

// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QString>
RESTORE_COMPILER_WARNINGS

enum class MessageSeverity
{
	Information,
	Warning,
	Error
};

// A message worded for the user: each front end shows the title and the text in a form matching the severity
struct UserMessage
{
	MessageSeverity severity;
	QString title;
	QString text;
};
