# Common settings of the test executables. TARGET must be set before this is included.

CONFIG += console

include($$PWD/../config.pri)

# A host's LTO removed: only tests with benchmarks use LTO
# MSVC keeps /GL and /LTCG: linking the /GL-built qtutils, cpputils and thin_io forces LTCG
QMAKE_CFLAGS   -= -flto=auto -ffat-lto-objects -flto=thin
QMAKE_CXXFLAGS -= -flto=auto -ffat-lto-objects -flto=thin
QMAKE_LFLAGS   -= -flto=auto -flto=thin

DESTDIR     = $$PWD/../../bin/$${OUTPUT_DIR}
OBJECTS_DIR = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}
MOC_DIR     = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}
UI_DIR      = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}
RCC_DIR     = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}

LIBS += -L$${DESTDIR} -lqtutils -lcpputils
include($$PWD/../../cpp-template-utils/3rdparty/catch2/catch2.pri)

mac*|linux*|freebsd{
	PRE_TARGETDEPS += $${DESTDIR}/libqtutils.a $${DESTDIR}/libcpputils.a
}
