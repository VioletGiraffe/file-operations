// The path text grammar: which spellings are absolute, where their root ends, and how they normalize.

#include "paths/pathsyntax.h"

#include "fileoperationtesthelpers.h"

static void requireNormalizesTo(const QString& input, const QString& expected)
{
	INFO("input: " << input.toStdString());
	CHECK(normalizedAbsolutePath(input) == expected);
	CHECK(normalizedAbsolutePath(expected) == expected); // Idempotent
}

static void requireNotAbsolute(const QString& input)
{
	INFO("input: " << input.toStdString());
	CHECK(absoluteRootLength(input) == 0);
	CHECK(normalizedAbsolutePath(input).isEmpty());
	CHECK_FALSE(isRootPath(input));
}

TEST_CASE("pathsyntax: what is not an absolute path", "[pathsyntax]")
{
	requireNotAbsolute({});
	requireNotAbsolute(QStringLiteral("relative/path"));
	requireNotAbsolute(QStringLiteral("."));
	requireNotAbsolute(QStringLiteral(".."));
	requireNotAbsolute(QStringLiteral(" /leading-space"));
#ifdef _WIN32
	requireNotAbsolute(QStringLiteral("C:")); // Drive-relative
	requireNotAbsolute(QStringLiteral("C:dir"));
	requireNotAbsolute(QStringLiteral("/rootless"));
	requireNotAbsolute(QStringLiteral("\\rootless"));
	requireNotAbsolute(QStringLiteral("1:/x"));
	requireNotAbsolute(QStringLiteral("\\\\server"));
	requireNotAbsolute(QStringLiteral("\\\\server\\"));
	requireNotAbsolute(QStringLiteral("\\\\\\x"));
	requireNotAbsolute(QStringLiteral("\\\\.\\pipe")); // Device namespace
	requireNotAbsolute(QStringLiteral("\\\\?\\C:\\dir")); // Extended namespace
	requireNotAbsolute(QStringLiteral("//?/UNC/server/share"));
	requireNotAbsolute(QStringLiteral("\\\\..\\share"));
	requireNotAbsolute(QStringLiteral("\\\\server\\.."));
#endif
}

#ifdef _WIN32

TEST_CASE("pathsyntax: Windows roots", "[pathsyntax]")
{
	CHECK(absoluteRootLength(QStringLiteral("C:/")) == 3);
	CHECK(absoluteRootLength(QStringLiteral("c:\\dir")) == 3);
	CHECK(absoluteRootLength(QStringLiteral("//server/share")) == 14);
	CHECK(absoluteRootLength(QStringLiteral("\\\\server\\share\\dir")) == 14);

	CHECK(isRootPath(QStringLiteral("C:/")));
	CHECK(isRootPath(QStringLiteral("C:\\")));
	CHECK(isRootPath(QStringLiteral("//server/share")));
	CHECK(isRootPath(QStringLiteral("\\\\server\\share\\")));
	CHECK_FALSE(isRootPath(QStringLiteral("C:/dir")));
	CHECK_FALSE(isRootPath(QStringLiteral("//server/share/dir")));
}

TEST_CASE("pathsyntax: Windows normalization", "[pathsyntax]")
{
	requireNormalizesTo(QStringLiteral("c:\\foo\\bar.txt"), QStringLiteral("C:/foo/bar.txt"));
	requireNormalizesTo(QStringLiteral("C:\\"), QStringLiteral("C:/"));
	requireNormalizesTo(QStringLiteral("C:/foo/"), QStringLiteral("C:/foo"));
	requireNormalizesTo(QStringLiteral("C://foo\\\\bar"), QStringLiteral("C:/foo/bar"));
	requireNormalizesTo(QStringLiteral("C:/a/./b/../c"), QStringLiteral("C:/a/c"));
	requireNormalizesTo(QStringLiteral("C:/../../x"), QStringLiteral("C:/x")); // Never above the root
	requireNormalizesTo(QStringLiteral("C:/a/.."), QStringLiteral("C:/"));
	requireNormalizesTo(QStringLiteral("C:/foo. /bar "), QStringLiteral("C:/foo. /bar ")); // Trailing dots and spaces are part of a name
	requireNormalizesTo(QStringLiteral("\\\\server\\share\\"), QStringLiteral("//server/share"));
	requireNormalizesTo(QStringLiteral("\\\\server\\share\\a\\..\\..\\b"), QStringLiteral("//server/share/b"));
}

#else

TEST_CASE("pathsyntax: POSIX roots", "[pathsyntax]")
{
	CHECK(absoluteRootLength(QStringLiteral("/")) == 1);
	CHECK(absoluteRootLength(QStringLiteral("/usr")) == 1);
	CHECK(isRootPath(QStringLiteral("/")));
	CHECK_FALSE(isRootPath(QStringLiteral("/usr")));
	requireNotAbsolute(QStringLiteral("C:/x")); // No drives here
}

TEST_CASE("pathsyntax: POSIX normalization", "[pathsyntax]")
{
	requireNormalizesTo(QStringLiteral("/usr/"), QStringLiteral("/usr"));
	requireNormalizesTo(QStringLiteral("//usr///bin"), QStringLiteral("/usr/bin"));
	requireNormalizesTo(QStringLiteral("/a/./b/../c"), QStringLiteral("/a/c"));
	requireNormalizesTo(QStringLiteral("/../x"), QStringLiteral("/x")); // Never above the root
	requireNormalizesTo(QStringLiteral("/a/.."), QStringLiteral("/"));
	requireNormalizesTo(QStringLiteral("/tmp/x "), QStringLiteral("/tmp/x "));
	requireNormalizesTo(QStringLiteral("/name\\with/backslash"), QStringLiteral("/name\\with/backslash")); // '\' is a name character
}

#endif
