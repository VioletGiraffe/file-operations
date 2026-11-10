// The synchronous commands that create a folder chain or an empty file from a name the user typed.

#include "fileoperations/entrycreation.h"

#include "fileoperationtesthelpers.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
RESTORE_COMPILER_WARNINGS

namespace
{

bool isDirectory(const QString& path)
{
	return QFileInfo{ path }.isDir();
}

} // namespace

TEST_CASE("createNewFolder: a name, a chain, and an existing folder", "[entrycreation]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	SECTION("a single name is created")
	{
		const auto result = createNewFolder(ep(base), QStringLiteral("new"));
		CHECK(result.status == EntryCreationStatus::Created);
		CHECK(result.kind == OperationEntryKind::Directory);
		CHECK(result.createdInParent == ep(base % "/new"));
		CHECK(isDirectory(base % "/new"));
	}

	SECTION("a chain creates every folder along it and reports its first")
	{
		const auto result = createNewFolder(ep(base), QStringLiteral("a/b/c"));
		CHECK(result.status == EntryCreationStatus::Created);
		CHECK(result.createdInParent == ep(base % "/a"));
		CHECK(isDirectory(base % "/a/b/c"));
	}

	SECTION("a chain through an existing folder creates the rest")
	{
		REQUIRE(QDir{ base }.mkdir(QStringLiteral("a")));
		const auto result = createNewFolder(ep(base), QStringLiteral("a/b"));
		CHECK(result.status == EntryCreationStatus::Created);
		CHECK(isDirectory(base % "/a/b"));
	}

	SECTION("an existing final folder already exists")
	{
		REQUIRE(QDir{ base }.mkpath(QStringLiteral("a/b")));
		CHECK(createNewFolder(ep(base), QStringLiteral("a/b")).status == EntryCreationStatus::AlreadyExists);
		CHECK(createNewFolder(ep(base), QStringLiteral("a")).status == EntryCreationStatus::AlreadyExists);
	}
}

TEST_CASE("createNewFolder: unusable names and a missing parent create nothing", "[entrycreation]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	SECTION("an unusable component is rejected")
	{
		const auto result = createNewFolder(ep(base), QStringLiteral("a/../b"));
		CHECK(result.status == EntryCreationStatus::RejectedInvalidName);
		CHECK(result.nameRejection == NameRejection::DotComponent);
		CHECK(entryAbsent(base % "/a"));
	}

	SECTION("a parent that is gone is reported, not recreated")
	{
		const auto result = createNewFolder(ep(base % "/gone"), QStringLiteral("new"));
		CHECK(result.status == EntryCreationStatus::ParentMissing);
		CHECK(entryAbsent(base % "/gone"));
	}

	SECTION("a parent that is a file is reported")
	{
		writeTestFile(base % "/file.bin", patternedContents(10));
		CHECK(createNewFolder(ep(base % "/file.bin"), QStringLiteral("new")).status == EntryCreationStatus::ParentMissing);
	}
}

#ifdef _WIN32
TEST_CASE("createNewFolder: Windows separates a chain on a backslash too", "[entrycreation]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	const auto result = createNewFolder(ep(base), QStringLiteral("a\\b"));
	CHECK(result.status == EntryCreationStatus::Created);
	CHECK(result.createdInParent == ep(base % "/a"));
	CHECK(isDirectory(base % "/a/b"));
}
#endif

TEST_CASE("createNewFile: a new file, an existing entry, an unusable name", "[entrycreation]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();

	SECTION("a new name creates an empty file")
	{
		const auto result = createNewFile(ep(base), QStringLiteral("new.txt"));
		CHECK(result.status == EntryCreationStatus::Created);
		CHECK(result.kind == OperationEntryKind::RegularFile);
		CHECK(result.createdInParent == ep(base % "/new.txt"));
		CHECK(readFileContents(base % "/new.txt").isEmpty());
	}

	SECTION("an existing file already exists and keeps its contents")
	{
		writeTestFile(base % "/file.bin", patternedContents(100));
		CHECK(createNewFile(ep(base), QStringLiteral("file.bin")).status == EntryCreationStatus::AlreadyExists);
		CHECK(readFileContents(base % "/file.bin") == patternedContents(100));
	}

	SECTION("an existing folder already exists")
	{
		REQUIRE(QDir{ base }.mkdir(QStringLiteral("folder")));
		CHECK(createNewFile(ep(base), QStringLiteral("folder")).status == EntryCreationStatus::AlreadyExists);
	}

	SECTION("a path is not a file name")
	{
		const auto result = createNewFile(ep(base), QStringLiteral("a/b.txt"));
		CHECK(result.status == EntryCreationStatus::RejectedInvalidName);
		CHECK(result.nameRejection == NameRejection::Separator);
	}

	SECTION("a missing parent fails with the native reason")
	{
		const auto result = createNewFile(ep(base % "/gone"), QStringLiteral("new.txt"));
		CHECK(result.status == EntryCreationStatus::Failed);
		REQUIRE(result.error);
		CHECK(result.error->category == FileErrorCategory::NotFound);
	}
}
