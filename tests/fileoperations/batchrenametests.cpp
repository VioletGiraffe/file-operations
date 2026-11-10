#include "fileoperations/batchrename.h"

#include "fileoperationtesthelpers.h"


// Submodule includes
#include "compiler/compiler_warnings_control.h"


DISABLE_COMPILER_WARNINGS
#include <QDir>
#include <QTemporaryDir>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] BatchRenameSource source(const QString& name, const QString& extension = {})
{
	return { .name = name, .extension = extension, .parentFolderName = QStringLiteral("Photos"),
		.modificationTime = QDateTime{ QDate{ 2024, 3, 7 }, QTime{ 9, 5, 2 } } };
}

[[nodiscard]] QString renamed(const BatchRenameRules& rules, const BatchRenameSource& item, const int index = 0)
{
	return CBatchRenamePattern{ rules }.newName(item, index);
}

[[nodiscard]] InlineRenameStatus statusOf(const std::optional<InlineRenameResult>& result)
{
	REQUIRE(result.has_value());
	return result->status;
}

const auto always = [] { return true; };

} // namespace

TEST_CASE("batch rename pattern: the default rules reproduce the name", "[batchrename]")
{
	const BatchRenameRules rules;
	CHECK(renamed(rules, source("photo", "JPG")) == "photo.JPG");
	CHECK(renamed(rules, source("folder.v2")) == "folder.v2");
	CHECK(renamed(rules, source(".bashrc")) == ".bashrc");
	CHECK(renamed(rules, source("trailing.")) == "trailing.");
}

TEST_CASE("batch rename pattern: mask tokens", "[batchrename]")
{
	BatchRenameRules rules;
	const BatchRenameSource photo = source("holiday", "jpg");

	SECTION("name, extension and parent folder")
	{
		rules.nameMask = QStringLiteral("[P] - [N]");
		rules.extensionMask = QStringLiteral("[E].bak");
		CHECK(renamed(rules, photo) == "Photos - holiday.jpg.bak");
	}

	SECTION("an empty extension result drops the dot")
	{
		rules.extensionMask.clear();
		CHECK(renamed(rules, photo) == "holiday");
	}

	SECTION("name ranges")
	{
		rules.extensionMask.clear();
		const std::pair<const char*, const char*> cases[]{
			{ "[N2-4]", "oli" }, { "[N3]", "l" }, { "[N5-]", "day" }, { "[N6-99]", "ay" }, { "[N4-2]", "" }, { "[N20]", "" },
		};
		for (const auto& [mask, expected] : cases)
		{
			INFO("mask: " << mask);
			rules.nameMask = QString::fromLatin1(mask);
			CHECK(renamed(rules, photo) == expected);
		}
	}

	SECTION("unknown and malformed tokens are literal")
	{
		rules.extensionMask.clear();
		for (const char* mask : { "[n]", "[N0-2]", "[X]", "[N", "[]", "[[N]" })
		{
			INFO("mask: " << mask);
			rules.nameMask = QString::fromLatin1(mask);
			const QString expected = rules.nameMask == "[[N]" ? QStringLiteral("[holiday") : rules.nameMask;
			CHECK(renamed(rules, photo) == expected);
		}
	}

	SECTION("modification time")
	{
		rules.nameMask = QStringLiteral("[Y]-[M]-[D] [h].[m].[s]");
		CHECK(renamed(rules, photo) == "2024-03-07 09.05.02.jpg");
	}

	SECTION("counter")
	{
		rules.nameMask = QStringLiteral("img[C]");
		rules.counterStart = 8;
		rules.counterStep = 2;
		rules.counterDigits = 3;
		CHECK(renamed(rules, photo, 0) == "img008.jpg");
		CHECK(renamed(rules, photo, 1) == "img010.jpg");
		CHECK(renamed(rules, photo, 500) == "img1008.jpg");
	}
}

TEST_CASE("batch rename pattern: letter case", "[batchrename]")
{
	BatchRenameRules rules;
	const BatchRenameSource item = source("my HOLIDAY_photo it's", "JPG");

	rules.letterCase = LetterCase::Lower;
	CHECK(renamed(rules, item) == "my holiday_photo it's.jpg");
	rules.letterCase = LetterCase::Upper;
	CHECK(renamed(rules, item) == "MY HOLIDAY_PHOTO IT'S.JPG");
	rules.letterCase = LetterCase::Title;
	CHECK(renamed(rules, item) == "My Holiday_Photo It's.jpg");
}

TEST_CASE("batch rename pattern: find and replace", "[batchrename]")
{
	BatchRenameRules rules;
	const BatchRenameSource item = source("IMG_1234 copy", "jpeg");

	SECTION("plain text, case-insensitive by default, on the whole name")
	{
		rules.find = QStringLiteral("JPEG");
		rules.replaceWith = QStringLiteral("jpg");
		CHECK(renamed(rules, item) == "IMG_1234 copy.jpg");

		rules.findIsCaseSensitive = true;
		CHECK(renamed(rules, item) == "IMG_1234 copy.jpeg");
	}

	SECTION("the replacement follows the letter case conversion")
	{
		rules.letterCase = LetterCase::Lower;
		rules.find = QStringLiteral("img");
		rules.replaceWith = QStringLiteral("PIC");
		CHECK(renamed(rules, item) == "PIC_1234 copy.jpeg");
	}

	SECTION("regex with captures")
	{
		rules.findIsRegex = true;
		rules.find = QStringLiteral("^img_(\\d+) copy");
		rules.replaceWith = QStringLiteral("\\1");
		CHECK(CBatchRenamePattern{ rules }.findPatternError().isEmpty());
		CHECK(renamed(rules, item) == "1234.jpeg");
	}

	SECTION("an invalid regex is reported and skipped")
	{
		rules.findIsRegex = true;
		rules.find = QStringLiteral("(unclosed");
		rules.replaceWith = QStringLiteral("x");
		CHECK(!CBatchRenamePattern{ rules }.findPatternError().isEmpty());
		CHECK(renamed(rules, item) == "IMG_1234 copy.jpeg");
	}
}

TEST_CASE("batch rename: never replaces an existing entry", "[batchrename]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();
	writeTestFile(base % "/a.txt", "a");
	writeTestFile(base % "/b.txt", "b");
	writeTestFile(base % "/c.txt", "c");
	writeTestFile(base % "/existing.txt", "existing");
	REQUIRE(QDir{ base }.mkdir(QStringLiteral("folder")));

	const auto results = batchRename({
		{ ep(base % "/a.txt"), QStringLiteral("renamed.txt") },
		{ ep(base % "/b.txt"), QStringLiteral("existing.txt") },
		{ ep(base % "/c.txt"), QStringLiteral("folder") },
	}, always);

	REQUIRE(results.size() == 3);
	CHECK(statusOf(results[0]) == InlineRenameStatus::Renamed);
	CHECK(statusOf(results[1]) == InlineRenameStatus::Rejected);
	CHECK(statusOf(results[2]) == InlineRenameStatus::Rejected);

	CHECK(readFileContents(base % "/renamed.txt") == "a");
	CHECK(readFileContents(base % "/b.txt") == "b");
	CHECK(readFileContents(base % "/c.txt") == "c");
	CHECK(readFileContents(base % "/existing.txt") == "existing");
}

TEST_CASE("batch rename: a chain completes in any order, a cycle stays as it is", "[batchrename]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();
	for (const char* name : { "1", "2", "3" })
		writeTestFile(base % '/' % QLatin1StringView{ name }, name);

	SECTION("a chain")
	{
		std::vector<BatchRenameItem> items{
			{ ep(base % "/1"), QStringLiteral("2") },
			{ ep(base % "/2"), QStringLiteral("3") },
			{ ep(base % "/3"), QStringLiteral("4") },
		};

		SECTION("each item before the one holding its new name") {}
		SECTION("each item after the one holding its new name") { std::ranges::reverse(items); }

		const auto results = batchRename(items, always);
		for (const auto& result : results)
			CHECK(statusOf(result) == InlineRenameStatus::Renamed);

		CHECK(entryAbsent(base % "/1"));
		CHECK(readFileContents(base % "/2") == "1");
		CHECK(readFileContents(base % "/3") == "2");
		CHECK(readFileContents(base % "/4") == "3");
	}

	SECTION("a cycle")
	{
		const auto results = batchRename({
			{ ep(base % "/1"), QStringLiteral("2") },
			{ ep(base % "/2"), QStringLiteral("1") },
		}, always);

		CHECK(statusOf(results[0]) == InlineRenameStatus::Rejected);
		CHECK(statusOf(results[1]) == InlineRenameStatus::Rejected);
		CHECK(readFileContents(base % "/1") == "1");
		CHECK(readFileContents(base % "/2") == "2");
	}
}

TEST_CASE("batch rename: stopping leaves the rest unattempted", "[batchrename]")
{
	QTemporaryDir tempDir;
	REQUIRE(tempDir.isValid());
	const QString base = tempDir.path();
	for (const char* name : { "a", "b", "c" })
		writeTestFile(base % '/' % QLatin1StringView{ name }, name);

	int attemptsAllowed = 1;
	const auto results = batchRename({
		{ ep(base % "/a"), QStringLiteral("x") },
		{ ep(base % "/b"), QStringLiteral("y") },
		{ ep(base % "/c"), QStringLiteral("z") },
	}, [&attemptsAllowed] { return attemptsAllowed-- > 0; });

	CHECK(statusOf(results[0]) == InlineRenameStatus::Renamed);
	CHECK(!results[1].has_value());
	CHECK(!results[2].has_value());
	CHECK(readFileContents(base % "/b") == "b");
	CHECK(readFileContents(base % "/c") == "c");
}
