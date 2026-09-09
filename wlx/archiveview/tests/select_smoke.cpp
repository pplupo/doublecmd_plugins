/// Checks what "extract this directory" resolves to.
///
/// Selecting a directory must yield every member beneath it, recursively.
/// This was broken in a way no listing test could catch: the Qt widget walked
/// the view's rows to find children, and Qt hands selections back in the name
/// column while only column-0 indexes have children — so the descent found
/// nothing and extracting a directory produced just the directory. The same
/// walk would also have missed children hidden by an active filter, or any
/// child at all in flat mode.
///
/// Resolution now happens in EntryTree against the whole tree, which is what
/// this exercises.
///
///   select_smoke <archive> <directory-path> [expected-count]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "core/ArchiveScanner.h"
#include "core/EntryTree.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::printf("usage: select_smoke <archive> <directory-path> [expected]\n");
        return 2;
    }

    const std::string archive = argv[1];
    const std::string root = argv[2];
    const long expected = (argc > 3) ? std::strtol(argv[3], nullptr, 10) : -1;

    archiveview::EntryTree tree;
    // Written from the scanner's worker thread.
    std::atomic<bool> done{false};

    archiveview::Scanner scanner;
    archiveview::Scanner::Callbacks callbacks;
    callbacks.entries = [&tree](const archiveview::EntryBatch &batch) {
        tree.addEntries(batch, nullptr);
    };
    callbacks.finished = [&done](bool, const std::string &, const archiveview::Summary &) {
        done.store(true, std::memory_order_release);
    };
    scanner.setCallbacks(std::move(callbacks));
    scanner.scan(archive);

    // Wait for the scan to *finish* — cancelAndWait() would cancel it, which
    // is a different thing and yields an empty tree.
    while (!done.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    scanner.cancelAndWait();   // joins a thread that has already returned

    const auto members = tree.membersUnder({root});
    std::printf("archive         : %s\n", archive.c_str());
    std::printf("selected        : %s\n", root.c_str());
    std::printf("members under   : %zu\n", members.size());
    for (size_t i = 0; i < members.size() && i < 5; ++i)
        std::printf("   %s\n", members[i].c_str());
    if (members.size() > 5)
        std::printf("   ... and %zu more\n", members.size() - 5);

    // Nothing outside the subtree may be included: a prefix match that
    // ignores the path boundary would pull "dirty.txt" in with "dir".
    int stray = 0;
    for (const std::string &member : members) {
        if (member != root
            && !(member.size() > root.size()
                 && member.compare(0, root.size(), root) == 0
                 && member[root.size()] == '/')) {
            std::printf("STRAY           : %s\n", member.c_str());
            ++stray;
        }
    }

    const bool ok = stray == 0 && (expected < 0
                                   || static_cast<long>(members.size()) == expected);
    std::printf("%s\n", ok ? "SELECTION OK" : "SELECTION WRONG");
    return ok ? 0 : 1;
}
