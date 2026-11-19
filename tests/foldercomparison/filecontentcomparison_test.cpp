#include "foldercomparison/filecontentcomparison.h"
#include "crandomdatagenerator.h"


// Submodule includes
#include "3rdparty/catch2/catch_runner.h"
#include "compiler/compiler_warnings_control.h"
#include "qtcore_helpers/qstring_helpers.hpp"
#include "timing/ctimeelapsed.h"


DISABLE_COMPILER_WARNINGS
#include "qtcore_helpers/catch_qt.hpp" // qtutils

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
RESTORE_COMPILER_WARNINGS

#include <atomic>
#include <initializer_list>
#include <iostream>
#include <stdint.h>

static uint32_t g_randomSeed = 0; // std::random seed

TEST_CASE("compareFileContents: random files compared with copies of themselves are equal", "[filecontentcomparison][randomdata]")
{
	QTemporaryDir sourceDirectory;
	if (!sourceDirectory.isValid())
	{
		FAIL();
		return;
	}

	CRandomDataGenerator gen;
	gen.setSeed(g_randomSeed);
	QFile fileA(sourceDirectory.filePath(QSL("A"))), fileB(sourceDirectory.filePath(QSL("B")));
	CTimeElapsed timer(true);
	timer.pause();
	for (int i = 0; i < 500; ++i)
	{
		const int length = gen.randomNumber<int>(10, 3 * 1024 * 1024);
		const auto data = gen.randomString(length).toLatin1();
		if (!fileA.open(QFile::WriteOnly) || !fileB.open(QFile::WriteOnly))
		{
			FAIL();
			return;
		}

		if (fileA.write(data) != data.size() || fileB.write(data) != data.size())
		{
			FAIL();
			return;
		}

		fileA.close();
		fileB.close();

		timer.resume();
		CHECK(compareFileContents(fileA.fileName(), fileB.fileName()) == ContentComparisonResult::Equal);
		timer.pause();
	}

	std::cout << "Total time taken to process 500 randomly sized files: " << (float)timer.elapsed() / 1000.0f;
}

TEST_CASE("compareFileContents: random files differ from other random files, and from copies with one byte changed", "[filecontentcomparison][randomdata]")
{
	QTemporaryDir sourceDirectory;
	CRandomDataGenerator gen;
	gen.setSeed(g_randomSeed);
	QFile fileA{sourceDirectory.filePath(QSL("A"))}, fileB{sourceDirectory.filePath(QSL("B"))};

	SECTION("Completely random data")
	{
		for (int i = 0; i < 500; ++i)
		{
			const int length = gen.randomNumber<int>(10, 3 * 1024 * 1024);
			if (!fileA.open(QFile::ReadWrite) || !fileB.open(QFile::ReadWrite))
			{
				FAIL();
				return;
			}

			const auto dataA = gen.randomString(length).toLatin1();
			const auto dataB = gen.randomString(length).toLatin1();
			if (fileA.write(dataA) != length || fileB.write(dataB) != length)
			{
				FAIL();
				return;
			}

			fileA.close();
			fileB.close();

			CHECK(compareFileContents(fileA.fileName(), fileB.fileName()) == ContentComparisonResult::Different);
		}
	}

	SECTION("Data differing in just one byte")
	{
		for (int i = 0; i < 500; ++i)
		{
			const int length = gen.randomNumber<int>(10, 3 * 1024 * 1024);
			if (!fileA.open(QFile::ReadWrite) || !fileB.open(QFile::ReadWrite))
			{
				FAIL();
				return;
			}

			const QByteArray dataA = gen.randomString(length).toLatin1();
			QByteArray dataB = dataA;
			dataB[dataB.size() - 1] = static_cast<char>(~(int)dataB[dataB.size() - 1]);
			if (fileA.write(dataA) != length || fileB.write(dataB) != length)
			{
				FAIL();
				return;
			}

			fileA.close();
			fileB.close();

			CHECK(compareFileContents(fileA.fileName(), fileB.fileName()) == ContentComparisonResult::Different);
		}
	}
}

namespace {

// compareFileContents() compares files in blocks of this size
constexpr qsizetype comparisonBlockSize = 1024 * 1024;

// Distinct byte values, so a shifted or misread block cannot pass for the expected one
[[nodiscard]] QByteArray patternedData(const qsizetype size)
{
	QByteArray data(size, Qt::Uninitialized);
	for (qsizetype i = 0; i < size; ++i)
		data[i] = static_cast<char>((i * 31 + 7) % 251);
	return data;
}

QString writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& contents)
{
	const QString path = dir.filePath(name);
	QFile file{ path };
	REQUIRE(file.open(QFile::WriteOnly));
	REQUIRE(file.write(contents) == contents.size());
	return path;
}

[[nodiscard]] QByteArray withByteFlipped(QByteArray data, const qsizetype index)
{
	data[index] = static_cast<char>(~data[index]);
	return data;
}

} // namespace

TEST_CASE("compareFileContents: two empty files are equal, with no progress reported", "[filecontentcomparison]")
{
	const QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString a = writeFile(dir, QSL("a"), {}), b = writeFile(dir, QSL("b"), {});

	int progressReports = 0;
	CHECK(compareFileContents(a, b, std::atomic<bool>{ false }, [&progressReports](uint64_t) { ++progressReports; }) == ContentComparisonResult::Equal);
	CHECK(progressReports == 0);
}

TEST_CASE("compareFileContents: files of different sizes differ, an empty one included", "[filecontentcomparison]")
{
	const QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString empty = writeFile(dir, QSL("empty"), {});
	const QString shorter = writeFile(dir, QSL("shorter"), patternedData(100));
	const QString longer = writeFile(dir, QSL("longer"), patternedData(101)); // Starts with the same 100 bytes

	CHECK(compareFileContents(empty, shorter) == ContentComparisonResult::Different);
	CHECK(compareFileContents(shorter, empty) == ContentComparisonResult::Different);
	CHECK(compareFileContents(shorter, longer) == ContentComparisonResult::Different);
	CHECK(compareFileContents(longer, shorter) == ContentComparisonResult::Different);
}

TEST_CASE("compareFileContents: files of the same size differ by a single byte anywhere, either side of a block boundary included", "[filecontentcomparison]")
{
	const QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const qsizetype size = 2 * comparisonBlockSize + 1; // Three blocks, the last one a single byte
	const QByteArray data = patternedData(size);
	const QString a = writeFile(dir, QSL("a"), data);

	qsizetype index = 0;
	SECTION("The first byte") { index = 0; }
	SECTION("The last byte of the first block") { index = comparisonBlockSize - 1; }
	SECTION("The first byte of the second block") { index = comparisonBlockSize; }
	SECTION("The only byte of the last block") { index = size - 1; }

	CAPTURE(index);
	const QString b = writeFile(dir, QSL("b"), withByteFlipped(data, index));
	CHECK(compareFileContents(a, b) == ContentComparisonResult::Different);
	CHECK(compareFileContents(b, a) == ContentComparisonResult::Different);
}

TEST_CASE("compareFileContents: equal files of several blocks are equal, the progress adding up to their size a block at a time", "[filecontentcomparison]")
{
	const QTemporaryDir dir;
	REQUIRE(dir.isValid());

	qsizetype size = 0;
	SECTION("One byte") { size = 1; }
	SECTION("Exactly one block") { size = comparisonBlockSize; }
	SECTION("A block and a byte") { size = comparisonBlockSize + 1; }
	SECTION("Several blocks and a partial one") { size = 3 * comparisonBlockSize + 12345; }

	CAPTURE(size);
	const QByteArray data = patternedData(size);
	const QString a = writeFile(dir, QSL("a"), data), b = writeFile(dir, QSL("b"), data);

	uint64_t reportedBytes = 0;
	bool everyReportWithinABlock = true;
	CHECK(compareFileContents(a, b, std::atomic<bool>{ false }, [&](const uint64_t bytesCompared) {
		reportedBytes += bytesCompared;
		everyReportWithinABlock &= bytesCompared > 0 && bytesCompared <= static_cast<uint64_t>(comparisonBlockSize);
	}) == ContentComparisonResult::Equal);
	CHECK(reportedBytes == static_cast<uint64_t>(size));
	CHECK(everyReportWithinABlock);
}

TEST_CASE("compareFileContents: a file is equal to itself", "[filecontentcomparison]")
{
	const QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString a = writeFile(dir, QSL("a"), patternedData(comparisonBlockSize + 1));

	CHECK(compareFileContents(a, a) == ContentComparisonResult::Equal);
}

TEST_CASE("compareFileContents: a file that does not exist, or a folder, is an error on either side", "[filecontentcomparison]")
{
	const QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString file = writeFile(dir, QSL("file"), patternedData(10));
	const QString emptyFile = writeFile(dir, QSL("empty"), {});
	const QString missing = dir.filePath(QSL("missing"));
	REQUIRE(QDir{ dir.path() }.mkdir(QSL("folder")));
	const QString folder = dir.filePath(QSL("folder"));

	for (const QString& unreadable : { missing, folder })
	{
		CAPTURE(unreadable);
		CHECK(compareFileContents(file, unreadable) == ContentComparisonResult::Error);
		CHECK(compareFileContents(unreadable, file) == ContentComparisonResult::Error);
		// Not merely the size check: an empty file has the size a missing one would report
		CHECK(compareFileContents(emptyFile, unreadable) == ContentComparisonResult::Error);
		CHECK(compareFileContents(unreadable, unreadable) == ContentComparisonResult::Error);
	}
}

TEST_CASE("compareFileContents: an abort stops before the next block, and the comparison reports it rather than a result", "[filecontentcomparison]")
{
	const QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QByteArray data = patternedData(2 * comparisonBlockSize + 1);
	const QString a = writeFile(dir, QSL("a"), data);
	// Differs only in the last block, which an abort after the first block never reaches
	const QString b = writeFile(dir, QSL("b"), withByteFlipped(data, data.size() - 1));

	SECTION("Requested before the comparison starts")
	{
		int progressReports = 0;
		CHECK(compareFileContents(a, a, std::atomic<bool>{ true }, [&progressReports](uint64_t) { ++progressReports; }) == ContentComparisonResult::Aborted);
		CHECK(progressReports == 0);
	}

	SECTION("Requested once the first block is compared")
	{
		std::atomic<bool> abort = false;
		int progressReports = 0;
		CHECK(compareFileContents(a, b, abort, [&](uint64_t) {
			++progressReports;
			abort = true;
		}) == ContentComparisonResult::Aborted);
		CHECK(progressReports == 1);
	}
}

int main(int argc, char* argv[])
{
	return runCatchSession(argc, argv, { .randomSeed = &g_randomSeed });
}
