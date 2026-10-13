# Single source manifest for the file-operation dialogs. The including project also needs fileoperations.pri or a library built from it.

HEADERS += \
	$$PWD/cfileoperationdialog.h \
	$$PWD/cfileoperationprompt.h \
	$$PWD/fileoperationtexts.h

SOURCES += \
	$$PWD/cfileoperationdialog.cpp \
	$$PWD/cfileoperationprompt.cpp \
	$$PWD/fileoperationtexts.cpp

FORMS += \
	$$PWD/cfileoperationdialog.ui \
	$$PWD/cfileoperationprompt.ui
