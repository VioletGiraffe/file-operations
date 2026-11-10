#pragma once

#include "fileoperations/usermessage.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QMessageBox>
RESTORE_COMPILER_WARNINGS

// A message box with the icon of the message's severity
inline void showUserMessage(QWidget* parent, const UserMessage& message)
{
	switch (message.severity)
	{
	case MessageSeverity::Information: QMessageBox::information(parent, message.title, message.text); break;
	case MessageSeverity::Warning: QMessageBox::warning(parent, message.title, message.text); break;
	case MessageSeverity::Error: QMessageBox::critical(parent, message.title, message.text); break;
	}
}
