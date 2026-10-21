TARGET = fileoperations_testhooks

DEFINES += FILE_OPERATIONS_TEST_HOOKS

include(../fileoperations-build.pri)

# A host's LTO removed: only tests link this variant, and only tests with benchmarks use LTO
# MSVC keeps /GL: the tests linking this library link with LTCG
QMAKE_CFLAGS   -= -flto=auto -ffat-lto-objects -flto=thin
QMAKE_CXXFLAGS -= -flto=auto -ffat-lto-objects -flto=thin
