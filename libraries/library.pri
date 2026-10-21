# Common settings of the library projects. TARGET must be set before this is included.

TEMPLATE = lib

include($$PWD/../config.pri)

DESTDIR     = $$PWD/../../bin/$${OUTPUT_DIR}
OBJECTS_DIR = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}
MOC_DIR     = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}
UI_DIR      = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}
RCC_DIR     = $$PWD/../../build/$${OUTPUT_DIR}/$${TARGET}
