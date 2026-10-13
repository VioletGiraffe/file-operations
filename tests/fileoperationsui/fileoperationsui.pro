TEMPLATE = app
TARGET   = fileoperationsui_test

include(../testproject.pri)

# The dialog drives a real job to exact points, so the engine is compiled from source with its fault hooks active.
DEFINES += FILE_OPERATIONS_TEST_HOOKS
include(../../fileoperations/fileoperations.pri)
include(../../fileoperationsui/fileoperationsui.pri)

SOURCES += \
	main.cpp \
	fileoperationprompttests.cpp \
	fileoperationdialogtests.cpp

HEADERS += \
	fileoperationguitesthelpers.h
