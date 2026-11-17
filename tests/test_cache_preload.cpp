// recent.txt trimming for the cache preload (vk_cache_preload.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_cache_preload.h"

using namespace Vulkan::CachePreload;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // Entries are paths inside the cache; nothing that could leave it becomes a path.
    assert(ValidEntry("pipeline-binaries/0a1b2c.bin"));
    assert(ValidEntry("0x0123456789abcdef_12.spv"));
    assert(ValidEntry("0x0123456789abcdef.meta"));
    assert(!ValidEntry(""));
    assert(!ValidEntry(std::string(129, 'f')));
    assert(!ValidEntry("/etc/passwd"));
    assert(!ValidEntry("../ab.bin"));
    assert(!ValidEntry("pipeline-binaries/../../ab"));
    assert(!ValidEntry("c:/ab.bin"));
    assert(!ValidEntry("pipeline-binaries\\ab.bin"));
    assert(!ValidEntry("AB.bin"));

    // Newest (last) first, each entry once, Windows line ends and junk skipped.
    using Entries = std::vector<std::string>;
    assert(Recent("", 10).empty());
    assert((Recent("aa\nbb\ncc\n", 10) == Entries{"cc", "bb", "aa"}));
    assert((Recent("aa\nbb\ncc", 10) == Entries{"cc", "bb", "aa"}));
    assert((Recent("aa\nbb\naa\ncc\nbb\n", 10) == Entries{"bb", "cc", "aa"}));
    assert((Recent("aa\r\n\nzz!\nbb\r\n", 10) == Entries{"bb", "aa"}));
    // The cap keeps the newest distinct entries.
    assert((Recent("aa\nbb\ncc\ndd\ncc\n", 2) == Entries{"cc", "dd"}));
    assert(Recent("aa\nbb\n", 0).empty());
    std::puts("cache-preload-test: passed");
    return 0;
}
