TEMPLATE = app
TARGET   = fileoperations_test

LIBS += -ltest_utils
include(../testproject.pri)

mac*|linux*|freebsd{
	PRE_TARGETDEPS += $${DESTDIR}/libtest_utils.a
}

INCLUDEPATH += ../../test-utils/src/

# The engine under test, with its fault hooks active
DEFINES += FILE_OPERATIONS_TEST_HOOKS
include(../../libraries/fileoperations.pri)

SOURCES += \
	main.cpp \
	testhooktests.cpp \
	entrypathtests.cpp \
	newnamechecktests.cpp \
	filesystemmutatortests.cpp \
	stagedfilecopytests.cpp \
	destinationresolvertests.cpp \
	sourcetreebuildertests.cpp \
	transferexecutortests.cpp \
	deleteexecutortests.cpp \
	moveexecutortests.cpp \
	crossvolumetests.cpp \
	hostilenametests.cpp \
	fileoperationjobtests.cpp \
	inlinerenametests.cpp

HEADERS += \
	fileoperationtesthelpers.h
