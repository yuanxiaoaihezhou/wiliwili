#pragma once

namespace wiliwili::download_policy {

// Works with std::filesystem::path and cpr::fs::path. Callers should canonicalize both paths first
// so symlinks and ".." cannot bypass the ownership-root check.
template <typename Path>
inline bool isStrictDescendant(const Path& child, const Path& root) {
    if (child.empty() || root.empty() || child == root) return false;
    auto childIt = child.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++childIt) {
        if (childIt == child.end() || *childIt != *rootIt) return false;
    }
    return childIt != child.end();
}

}  // namespace wiliwili::download_policy
