#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string>

#include "utils/download_transfer_policy.hpp"
#include "utils/download_path_policy.hpp"

using wiliwili::download_policy::contentRangeStart;
using wiliwili::download_policy::rangeResponseMatches;

int main() {
    assert(contentRangeStart("bytes 0-99/1000") == 0);
    assert(contentRangeStart("bytes 1048576-2097151/8388608") == 1048576);
    assert(contentRangeStart("bytes */1000") == -1);
    assert(contentRangeStart("garbage") == -1);
    assert(contentRangeStart("bytes -99/1000") == -1);

    assert(rangeResponseMatches(206, "bytes 100-199/1000", 100));
    assert(!rangeResponseMatches(206, "bytes 0-99/1000", 100));
    assert(!rangeResponseMatches(200, "bytes 100-199/1000", 100));
    assert(!rangeResponseMatches(206, "garbage", 100));

    using wiliwili::download_policy::isStrictDescendant;
    const std::filesystem::path root{"/home/deck/Videos/wiliwili"};
    assert(isStrictDescendant(std::filesystem::path{"/home/deck/Videos/wiliwili/Series/EP01"}, root));
    assert(!isStrictDescendant(root, root));
    assert(!isStrictDescendant(std::filesystem::path{"/home/deck/Videos"}, root));
    assert(!isStrictDescendant(std::filesystem::path{"/home/deck/Videos/wiliwili-evil/EP01"}, root));
    return 0;
}
