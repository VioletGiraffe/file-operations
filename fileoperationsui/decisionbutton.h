#pragma once

#include "fileoperations/fileoperationtexts.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <3rdparty/magic_enum/magic_enum.hpp>

#include <QPushButton>
RESTORE_COMPILER_WARNINGS

#include <string_view>

// "btn" plus the action's name: the dialogs' tests find the buttons by it
inline QString decisionButtonName(const DecisionAction action)
{
	const std::string_view name = magic_enum::enum_name(action);
	return QStringLiteral("btn") + QLatin1String{ name.data(), static_cast<qsizetype>(name.size()) };
}

// Never an auto-default button: Enter presses only the default button a dialog sets, if any
inline QPushButton* createDecisionButton(const DecisionAction action, const PromptOperation operation, QWidget* parent)
{
	auto* button = new QPushButton{ fileOperationActionLabel(action, operation), parent };
	button->setObjectName(decisionButtonName(action));
	button->setAutoDefault(false);
	return button;
}
