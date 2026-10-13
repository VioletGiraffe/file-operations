TEMPLATE = subdirs

SUBDIRS = fileoperations fileoperationsui foldercomparison
SUBDIRS += qtutils cpputils cpp-template-utils catch2-runner test-utils thin_io

cpp-template-utils.subdir = ../../cpp-template-utils
catch2-runner.subdir = ../../cpp-template-utils/3rdparty/catch2
cpputils.subdir = ../../cpputils
thin_io.subdir = ../../thin_io
test-utils.subdir = ../test-utils

qtutils.subdir = ../../qtutils
qtutils.depends = cpputils

test-utils.depends = qtutils

fileoperations.depends = test-utils thin_io catch2-runner
fileoperationsui.depends = qtutils cpputils thin_io catch2-runner
foldercomparison.depends = test-utils thin_io catch2-runner
