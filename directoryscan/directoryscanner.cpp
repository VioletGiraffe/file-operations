#include "directoryscanner.h"

#include "thinioqstring.h"

DISABLE_COMPILER_WARNINGS
#include <QStringBuilder>
RESTORE_COMPILER_WARNINGS

#include <vector>

const thin_io::entry_status& followedStatus(const thin_io::directory_entry& entry) noexcept
{
	if (entry.attributes.is_link && entry.link_target)
		return *entry.link_target;

	return entry;
}

namespace {

struct Scan {
	const std::function<void (const QString&, const thin_io::directory_entry&, bool)>& observer;
	const std::atomic<bool>& abort;
	const bool followDirLinks;
	std::vector<QString> dirsBeingScanned;
};

// Cycle guard: a link is only traversed if its target is not a directory already being scanned higher up this branch;
// that covers links to their own ancestors as well as loops between links. Comparing filesystem identities rather than
// paths sidesteps path normalization pitfalls (letter case, junction targets that Qt does not canonicalize).
// Only links pay for the identity lookups; plain directory trees don't reach this code.
bool mayTraverseLink(const QString& linkPath, const Scan& scan)
{
	if (!scan.followDirLinks)
		return false;

	const auto targetId = resolvedObjectId(linkPath);
	if (!targetId) // Broken link, or a filesystem exposing no identity: refuse to follow rather than risk looping
		return false;

	for (const QString& dirPath : scan.dirsBeingScanned)
	{
		if (resolvedObjectId(dirPath) == targetId)
			return false;
	}

	return true;
}

// dirPath ends with '/'.
void scanContents(const QString& dirPath, const bool reachedThroughLink, Scan& scan)
{
	const auto entries = listDirectoryWithDetails(dirPath);
	if (!entries)
		return;

	scan.dirsBeingScanned.push_back(dirPath);

	for (const auto& entry : *entries)
	{
		if (scan.abort)
			break;

		if (scan.observer)
			scan.observer(dirPath, entry, reachedThroughLink);

		if (followedStatus(entry).attributes.kind != thin_io::entry_kind::directory)
			continue;

		const QString childPath = dirPath % nativeNameToQString(entry.name) % '/';
		if (!entry.attributes.is_link)
			scanContents(childPath, reachedThroughLink, scan);
		else if (mayTraverseLink(childPath, scan))
			scanContents(childPath, true, scan);
	}

	scan.dirsBeingScanned.pop_back();
}

} // namespace

void scanDirectory(const QString& rootPath,
	const std::function<void (const QString&, const thin_io::directory_entry&, bool)>& observer,
	const std::atomic<bool>& abort,
	const bool followDirLinks)
{
	const QString dirPath = rootPath.endsWith('/') ? rootPath : QString{ rootPath % '/' };

	const auto rootEntry = getDirectoryEntry(withoutTrailingSeparator(dirPath));
	if (!rootEntry || followedStatus(*rootEntry).attributes.kind != thin_io::entry_kind::directory)
		return;

	Scan scan{ observer, abort, followDirLinks, {} };
	const bool rootIsLink = rootEntry->attributes.is_link;
	if (rootIsLink && !mayTraverseLink(dirPath, scan))
		return;

	scanContents(dirPath, rootIsLink, scan);
}
