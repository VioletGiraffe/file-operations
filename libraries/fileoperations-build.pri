# The fileoperations library, shared by its production and test-hooks projects. TARGET must be set before this is included.

include($$PWD/library.pri)

QT = core

include($$PWD/../fileoperations/fileoperations.pri)
include($$PWD/../directoryscan/directoryscan.pri)
include($$PWD/../foldercomparison/foldercomparison.pri)
