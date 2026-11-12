# Single source manifest for the shell's own file operations, included by the library projects in libraries/.

HEADERS += \
	$$PWD/cshelloperationrunner.h \
	$$PWD/shellfileoperations.h

SOURCES += \
	$$PWD/cshelloperationrunner.cpp \
	$$PWD/shellfileoperations.cpp
