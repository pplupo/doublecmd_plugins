#include "core/EntryTree.h"

#include <algorithm>
#include <unordered_set>

namespace archiveview {

EntryTree::EntryTree()
{
    m_root.isDir = true;
    m_root.attached = true;
}

EntryTree::Node *EntryTree::makeNode(const std::string &name,
                                     const std::string &fullPath,
                                     Node *parent, bool registerPath)
{
    m_storage.push_back(std::make_unique<Node>());
    Node *node = m_storage.back().get();
    node->name = name;
    node->fullPath = fullPath;
    node->parent = parent;
    if (registerPath)
        m_byPath.emplace(fullPath, node);
    return node;
}

void EntryTree::hold(Node *node, Node *parent, Sink *sink)
{
    Pending *pending = sink ? sink->pending : nullptr;

    if (!pending) {
        // Immediate mode: attach and announce now, so the node is never
        // reachable before the observer has been told about it.
        node->row = static_cast<int>(parent->children.size());
        node->attached = true;
        parent->children.push_back(node);
        if (sink && sink->listener)
            sink->listener->nodeAttached(node);
        return;
    }

    if (parent->attached) {
        // Parent is already on screen, so this node's arrival has to be
        // announced. Hold it back until the whole batch is processed, then
        // attach every sibling group in one call.
        (*pending)[parent].push_back(node);
    } else {
        // Parent is itself pending (or there is no listener), so this node
        // rides along inside its parent's insertion and needs no separate
        // notification.
        node->row = static_cast<int>(parent->children.size());
        node->attached = parent->attached;
        parent->children.push_back(node);
    }
}

void EntryTree::flush(Sink *sink)
{
    if (!sink || !sink->pending)
        return;
    Pending *pending = sink->pending;
    Listener *listener = sink->listener;

    for (auto &group : *pending) {
        Node *parent = group.first;
        std::vector<Node *> &nodes = group.second;
        if (nodes.empty())
            continue;

        const int first = static_cast<int>(parent->children.size());
        const int count = static_cast<int>(nodes.size());
        const Node *reported = (parent == &m_root) ? nullptr : parent;

        if (listener)
            listener->beforeInsert(reported, first, count);

        for (Node *node : nodes) {
            node->row = static_cast<int>(parent->children.size());
            parent->children.push_back(node);
            node->attached = true;
            // Descendants created inside this batch become visible with
            // their ancestor.
            std::vector<Node *> stack = node->children;
            while (!stack.empty()) {
                Node *descendant = stack.back();
                stack.pop_back();
                descendant->attached = true;
                stack.insert(stack.end(), descendant->children.begin(),
                             descendant->children.end());
            }
        }

        if (listener)
            listener->afterInsert(reported, first, count);
    }
    pending->clear();
}

EntryTree::Node *EntryTree::ensureNode(const std::string &path, bool isDir,
                                       Sink *sink)
{
    const auto existing = m_byPath.find(path);
    if (existing != m_byPath.end()) {
        if (isDir)
            existing->second->isDir = true;
        return existing->second;
    }

    const auto slash = path.find_last_of('/');
    Node *parent = &m_root;
    std::string name = path;
    if (slash != std::string::npos && slash > 0) {
        // Ancestors are always directories, whether or not the archive said so.
        parent = ensureNode(path.substr(0, slash), true, sink);
        name = path.substr(slash + 1);
    }
    // slash == 0 means an absolute member path such as "/absolute/file.txt".
    // Its first component keeps the leading slash and sits at the top level,
    // so an absolute path stays visibly absolute instead of being quietly
    // rehomed under a blank-named root node.

    Node *node = makeNode(name, path, parent, /*registerPath=*/true);
    node->isDir = isDir;
    hold(node, parent, sink);
    return node;
}

EntryTree::Node *EntryTree::addDuplicate(Node *original, Sink *sink)
{
    // An archive may store two members under the same path. Folding them into
    // one row would hide the discrepancy, and a duplicate path is exactly the
    // kind of thing someone previewing an untrusted archive needs to see — it
    // is a known trick for showing one file and extracting another. The
    // duplicate gets its own row and is deliberately not registered in the
    // path index, so later children still resolve to the first node.
    Node *parent = original->parent ? original->parent : &m_root;
    Node *node = makeNode(original->name, original->fullPath, parent,
                          /*registerPath=*/false);
    node->isDuplicate = true;
    ++m_duplicateCount;
    hold(node, parent, sink);
    return node;
}

void EntryTree::addEntries(const EntryBatch &batch, Listener *listener, Mode mode)
{
    if (batch.empty())
        return;

    Pending pending;
    Sink sink;
    sink.listener = listener;
    sink.pending = (mode == Mode::Grouped) ? &pending : nullptr;

    for (const Entry &entry : batch) {
        Node *node = ensureNode(entry.path, entry.isDir(), &sink);

        if (node->hasEntry)
            node = addDuplicate(node, &sink);

        node->hasEntry = true;
        node->entry = entry;
        m_flat.push_back(node);
    }

    flush(&sink);
}

std::vector<std::string>
EntryTree::membersUnder(const std::vector<std::string> &roots) const
{
    std::vector<std::string> members;
    if (roots.empty())
        return members;

    const std::unordered_set<std::string> exact(roots.begin(), roots.end());

    for (const Node *node : m_flat) {
        if (!node->hasEntry)
            continue;

        bool wanted = exact.count(node->fullPath) > 0;
        if (!wanted) {
            for (const std::string &root : roots) {
                // Prefix match on a path *boundary*, so selecting "dir" does
                // not drag in "dirty.txt".
                if (node->fullPath.size() > root.size()
                    && node->fullPath.compare(0, root.size(), root) == 0
                    && node->fullPath[root.size()] == '/') {
                    wanted = true;
                    break;
                }
            }
        }
        if (wanted)
            members.push_back(node->fullPath);
    }

    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    return members;
}

void EntryTree::clear()
{
    m_root.children.clear();
    m_byPath.clear();
    m_flat.clear();
    m_storage.clear();
    m_duplicateCount = 0;
}

} // namespace archiveview
