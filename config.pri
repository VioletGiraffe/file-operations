# Shared by every project in this repository.
# The dependencies are checked out beside it: qtutils, cpputils, cpp-template-utils, thin_io. Binaries go to ../bin.

QT = core gui widgets

# Don't override app targets with staticlib, only actual libraries
contains(TEMPLATE, ".*lib"): CONFIG += staticlib

# A host repository's build settings win where it has them
exists($$PWD/../global.pri) {
	include($$PWD/../global.pri)
} else {
	CONFIG += strict_c++ c++2b

	mac* | linux* | freebsd {
		CONFIG(release, debug|release):CONFIG *= Release optimize_full
		CONFIG(debug, debug|release):CONFIG *= Debug
	}

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG

	win* {
		DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX
		QMAKE_CXXFLAGS += /MP /Zi /FS /utf-8 /std:c++latest /permissive- /Zc:__cplusplus
		QMAKE_CXXFLAGS_WARN_ON = /W4
		QMAKE_LFLAGS += /DEBUG
	}

	mac* {
		QMAKE_MACOSX_DEPLOYMENT_TARGET = 13.3
		# Qt 6.9 headers use ARM ACLE intrinsics without including arm_acle.h
		contains(QMAKE_HOST.arch, arm64)|contains(QMAKE_APPLE_DEVICE_ARCHS, arm64): QMAKE_CXXFLAGS += -include arm_acle.h
	}
}

Release:OUTPUT_DIR=release
Debug:OUTPUT_DIR=debug

INCLUDEPATH += \
	$$PWD \
	$$PWD/../qtutils \
	$$PWD/../cpputils \
	$$PWD/../cpp-template-utils \
	$$PWD/../thin_io/src

LIBS += -L$$PWD/../bin/$${OUTPUT_DIR} -lthin_io
