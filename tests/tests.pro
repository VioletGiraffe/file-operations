TEMPLATE = subdirs

SUBDIRS = fileoperations fileoperationsui foldercomparison
SUBDIRS += fileoperations-lib fileoperations-testhooks-lib fileoperationsui-lib
SUBDIRS += qtutils cpputils cpp-template-utils catch2-runner test-utils thin_io

fileoperations-lib.subdir = ../libraries/fileoperations
fileoperations-testhooks-lib.subdir = ../libraries/fileoperations-testhooks
fileoperationsui-lib.subdir = ../libraries/fileoperationsui

cpp-template-utils.subdir = ../../cpp-template-utils
catch2-runner.subdir = ../../cpp-template-utils/3rdparty/catch2
cpputils.subdir = ../../cpputils
thin_io.subdir = ../../thin_io
test-utils.subdir = ../test-utils

qtutils.subdir = ../../qtutils
qtutils.depends = cpputils

test-utils.depends = qtutils

fileoperations.depends = fileoperations-testhooks-lib test-utils thin_io catch2-runner
fileoperationsui.depends = fileoperations-testhooks-lib fileoperationsui-lib qtutils cpputils thin_io catch2-runner
foldercomparison.depends = fileoperations-lib test-utils thin_io catch2-runner
