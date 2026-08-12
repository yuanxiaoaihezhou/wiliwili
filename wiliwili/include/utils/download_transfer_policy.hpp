#pragma once

#include <cstdint>
#include <string>

namespace wiliwili::download_policy {

// Parse the first byte position from an HTTP Content-Range value such as
// "bytes 1048576-2097151/8388608". Return -1 for malformed/unsatisfied ranges.
inline int64_t contentRangeStart(const std::string& value) {
    const auto bytes = value.find("bytes ");
    if (bytes == std::string::npos) return -1;
    const auto begin = bytes + 6;
    const auto dash = value.find('-', begin);
    if (dash == std::string::npos || dash == begin) return -1;
    try {
        size_t consumed = 0;
        const auto parsed = std::stoll(value.substr(begin, dash - begin), &consumed);
        if (consumed != dash - begin || parsed < 0) return -1;
        return parsed;
    } catch (...) {
        return -1;
    }
}

inline bool rangeResponseMatches(long httpStatus, const std::string& contentRange, int64_t expectedStart) {
    return httpStatus == 206 && expectedStart >= 0 && contentRangeStart(contentRange) == expectedStart;
}

}  // namespace wiliwili::download_policy
