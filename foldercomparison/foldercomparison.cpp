#include "foldercomparison.h"

#include "filecontentcomparison.h"

#include "directoryscan/directoryscanner.h"
#include "qt_paths.hpp" // thin_io

#include "paths/pathsyntax.h"


// Submodule includes
#include "assert/advanced_assert.h"


DISABLE_COMPILER_WARNINGS
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

#include <algorithm>
#include <map>
#include <utility>

namespace {

struct SideEntry {
	QString relativePath;
	std::optional<uint64_t> fileSize; // Regular files only
	bool isDirectory = false;
};

using SideMap = std::map<QString /* relative path */, SideEntry>;

// '/'-separated, ending with '/'
QString rootPrefix(const QString& root)
{
	const QString path = normalizedAbsolutePath(root);
	assert_r(!path.isEmpty());
	return path.endsWith('/') ? path : QString{ path % '/' };
}

SideMap enumerateSide(const QString& prefix, const std::atomic<bool>& abort)
{
	SideMap entries;

	scanDirectory(prefix, [&entries, &prefix](const QString& parentPath, const thin_io::directory_entry& item, bool /*reachedThroughLink*/) {
		assert_debug_only(parentPath.startsWith(prefix));

		const thin_io::entry_status& status = followedStatus(item);

		SideEntry entry;
		entry.relativePath = parentPath.mid(prefix.length()) % thin_io::to_qstring(item.name);
		entry.isDirectory = status.attributes.kind == thin_io::entry_kind::directory;
		if (status.attributes.kind == thin_io::entry_kind::regular_file)
			entry.fileSize = status.logical_size.value_or(0);

		QString key = entry.relativePath;
		entries.emplace(std::move(key), std::move(entry));
	}, abort);

	return entries;
}

// Pairs up entries across filesystems that disagree on letter case or on Unicode normalization.
QString foldedKey(const QString& relativePath)
{
	return relativePath.normalized(QString::NormalizationForm_C).toCaseFolded();
}

FolderComparisonEntry pairedEntry(const SideEntry& left, const SideEntry& right, const EntryDifference status)
{
	FolderComparisonEntry entry;
	entry.relativePath = left.relativePath;
	entry.status = status;
	entry.leftSize = left.fileSize;
	entry.rightSize = right.fileSize;
	return entry;
}

FolderComparisonEntry singleSidedEntry(const SideEntry& side, const EntryDifference status)
{
	assert_debug_only(status == EntryDifference::OnlyInLeft || status == EntryDifference::OnlyInRight);

	FolderComparisonEntry entry;
	entry.relativePath = side.relativePath;
	entry.status = status;
	(status == EntryDifference::OnlyInLeft ? entry.leftSize : entry.rightSize) = side.fileSize;
	return entry;
}

} // namespace

FolderComparisonResult compareFolders(const QString& leftRoot, const QString& rightRoot, const PairingMode pairingMode,
	const std::atomic<bool>& abort, const std::function<void (int percent)>& onProgress)
{
	FolderComparisonResult result;

	const QString leftPrefix = rootPrefix(leftRoot), rightPrefix = rootPrefix(rightRoot);
	SideMap left = enumerateSide(leftPrefix, abort);
	SideMap right = enumerateSide(rightPrefix, abort);
	if (abort)
	{
		result.aborted = true;
		return result;
	}

	std::vector<std::pair<SideEntry, SideEntry>> contentCandidates;
	uint64_t totalCandidateBytes = 0;

	const auto classifyPair = [&](const SideEntry& l, const SideEntry& r) {
		if (l.isDirectory && r.isDirectory)
			return; // The same shape at this level; anything differing below is an entry in its own right

		// An entry that is neither a directory nor a regular file counts as empty here; the content comparison then fails on it.
		const uint64_t leftBytes = l.fileSize.value_or(0);
		if (l.isDirectory != r.isDirectory || leftBytes != r.fileSize.value_or(0))
		{
			result.differences.push_back(pairedEntry(l, r, EntryDifference::Different));
			return;
		}

		totalCandidateBytes += leftBytes;
		contentCandidates.emplace_back(l, r);
	};

	for (auto leftIt = left.begin(); leftIt != left.end(); )
	{
		const auto rightIt = right.find(leftIt->first);
		if (rightIt == right.end())
		{
			++leftIt;
			continue;
		}

		classifyPair(leftIt->second, rightIt->second);
		right.erase(rightIt);
		leftIt = left.erase(leftIt);
	}

	// Only what the exact pass left over is folded, so that two entries differing just in case on a case-sensitive
	// filesystem still pair with their true counterparts rather than with each other.
	if (pairingMode == PairingMode::FoldCaseAndNormalization && !left.empty() && !right.empty())
	{
		std::map<QString, QString> foldedRight; // Folded key -> the one exact key it stands for; emptied when several do
		for (const auto& item : right)
		{
			const auto [it, inserted] = foldedRight.emplace(foldedKey(item.first), item.first);
			if (!inserted)
				it->second.clear();
		}

		for (auto leftIt = left.begin(); leftIt != left.end(); )
		{
			const auto foldedIt = foldedRight.find(foldedKey(leftIt->first));
			if (foldedIt == foldedRight.end() || foldedIt->second.isEmpty())
			{
				++leftIt; // No counterpart, or several equally plausible ones - refuse to guess
				continue;
			}

			const auto rightIt = right.find(foldedIt->second);
			assert_debug_only(rightIt != right.end());
			classifyPair(leftIt->second, rightIt->second);
			right.erase(rightIt);
			foldedRight.erase(foldedIt);
			leftIt = left.erase(leftIt);
		}
	}

	for (const auto& item : left)
		result.differences.push_back(singleSidedEntry(item.second, EntryDifference::OnlyInLeft));

	for (const auto& item : right)
		result.differences.push_back(singleSidedEntry(item.second, EntryDifference::OnlyInRight));

	uint64_t comparedBytes = 0;
	for (const auto& candidate : contentCandidates)
	{
		if (abort)
		{
			result.aborted = true;
			break;
		}

		const auto comparison = compareFileContents(leftPrefix % candidate.first.relativePath,
			rightPrefix % candidate.second.relativePath, abort, [&](uint64_t bytesCompared) {
				comparedBytes += bytesCompared;
				if (onProgress)
					onProgress(static_cast<int>(comparedBytes * 100 / totalCandidateBytes)); // Non-zero whenever any bytes get reported
			});

		switch (comparison)
		{
		case ContentComparisonResult::Equal:
			++result.identicalFiles;
			break;
		case ContentComparisonResult::Different:
			result.differences.push_back(pairedEntry(candidate.first, candidate.second, EntryDifference::Different));
			break;
		case ContentComparisonResult::Error:
			result.differences.push_back(pairedEntry(candidate.first, candidate.second, EntryDifference::ComparisonFailed));
			break;
		case ContentComparisonResult::Aborted:
			result.aborted = true;
			break;
		}

		if (result.aborted)
			break;
	}

	std::sort(result.differences.begin(), result.differences.end(),
		[](const FolderComparisonEntry& a, const FolderComparisonEntry& b) { return a.relativePath < b.relativePath; });

	return result;
}
