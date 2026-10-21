# Single source manifest for the file-operation dialogs, included by the library project in libraries/.

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
