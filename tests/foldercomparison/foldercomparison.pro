TEMPLATE = app
TARGET   = foldercomparison_test

LIBS += -ltest_utils
include(../testproject.pri)

mac*|linux*|freebsd{
	PRE_TARGETDEPS += $${DESTDIR}/libtest_utils.a
}

INCLUDEPATH += ../../test-utils/src/

include(../../libraries/fileoperations.pri)

SOURCES += \
	filecontentcomparison_test.cpp \
	foldercomparison_test.cpp
