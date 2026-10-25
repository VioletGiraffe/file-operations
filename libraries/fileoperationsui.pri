# Links the fileoperationsui library (the file-operation dialogs) and its dependencies, fileoperations.pri included.
# Must be included after the LIBS of anything that links against it: see fileoperations.pri.

LIBS += -L$$PWD/../../bin/$${OUTPUT_DIR} -lfileoperationsui -lqtutils
linux*|freebsd:QT += dbus # qtutils' revealInFileManager()

mac*|linux*|freebsd{
	PRE_TARGETDEPS += $$PWD/../../bin/$${OUTPUT_DIR}/libfileoperationsui.a $$PWD/../../bin/$${OUTPUT_DIR}/libqtutils.a
}

include($$PWD/fileoperations.pri)
