# Single source manifest for the file-operation dialogs, included by the library project in libraries/.

HEADERS += \
	$$PWD/cconflictlistdialog.h \
	$$PWD/cconflictlistmodel.h \
	$$PWD/cfileoperationdialog.h \
	$$PWD/cfileoperationprompt.h \
	$$PWD/fileoperationtexts.h

SOURCES += \
	$$PWD/cconflictlistdialog.cpp \
	$$PWD/cconflictlistmodel.cpp \
	$$PWD/cfileoperationdialog.cpp \
	$$PWD/cfileoperationprompt.cpp \
	$$PWD/fileoperationtexts.cpp

FORMS += \
	$$PWD/cconflictlistdialog.ui \
	$$PWD/cfileoperationdialog.ui \
	$$PWD/cfileoperationprompt.ui
