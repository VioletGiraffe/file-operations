# Links the fileoperations library (the engine, directory scanning, folder comparison) and its dependencies.
# A project defining FILE_OPERATIONS_TEST_HOOKS gets the variant built with the fault hooks active.
# Must be included after the LIBS of anything that links against it: GNU ld resolves static libraries in command-line order.

contains(DEFINES, FILE_OPERATIONS_TEST_HOOKS): FILE_OPERATIONS_LIBRARY = fileoperations_testhooks
else: FILE_OPERATIONS_LIBRARY = fileoperations

INCLUDEPATH += \
	$$PWD/.. \
	$$PWD/../../cpputils \
	$$PWD/../../cpp-template-utils \
	$$PWD/../../thin_io/src

LIBS += -L$$PWD/../../bin/$${OUTPUT_DIR} -l$${FILE_OPERATIONS_LIBRARY} -lcpputils -lthin_io

mac*|linux*|freebsd{
	PRE_TARGETDEPS += $$PWD/../../bin/$${OUTPUT_DIR}/lib$${FILE_OPERATIONS_LIBRARY}.a $$PWD/../../bin/$${OUTPUT_DIR}/libcpputils.a $$PWD/../../bin/$${OUTPUT_DIR}/libthin_io.a
}
