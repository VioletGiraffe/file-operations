TEMPLATE = app
TARGET   = fileoperationsui_test

include(../testproject.pri)

# The dialog drives a real job to exact points, so the engine comes with its fault hooks active.
DEFINES += FILE_OPERATIONS_TEST_HOOKS
include(../../libraries/fileoperationsui.pri)

SOURCES += \
	main.cpp \
	fileoperationprompttests.cpp \
	conflictlistdialogtests.cpp \
	fileoperationdialogtests.cpp

HEADERS += \
	fileoperationguitesthelpers.h
