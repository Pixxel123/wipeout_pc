// recent.txt trimming and the preloaded bytes of the cache preload (vk_cache_preload.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
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

    // Bytes are kept only for entries not yet used or saved, and a save drops them.
    using Bytes = std::vector<u8>;
    Store store;
    assert(store.Keep("a", Bytes{1, 2}));
    assert(store.Find("a") && *store.Find("a") == (Bytes{1, 2}));
    store.Used("a");
    assert(store.Find("a")); // neither a find nor a use drops them
    store.Saved("a");
    assert(!store.Find("a"));
    assert(!store.Keep("a", Bytes{3}));
    store.Used("b");
    assert(!store.Keep("b", Bytes{4}));
    store.Saved("c");
    assert(!store.Keep("c", Bytes{5}));
    assert(!store.Find("b") && !store.Find("c"));
    // Each entry is listed once, in the order of first use.
    store.Used("b");
    store.Saved("b");
    assert((store.TakePending() == Entries{"a", "b", "c"}));
    assert(store.TakePending().empty());
    store.Used("a");
    assert(store.TakePending().empty());

    // The kinds: saved shader files, then pipeline binaries, then the rest (pipeline keys).
    assert(Rank("0x0123456789abcdef_12.spv") == 0 && Rank("0x0123456789abcdef.meta") == 0);
    assert(Rank("pipeline-binaries/0a1b2c.bin") == 1);
    assert(Rank("g_0x0123456789abcdef.key") == 2 && Rank("c_0x01.key") == 2);
    assert(Rank("disk_shaders.bin") == 2 && Rank("recent.txt") == 2);
    // By kind, newest first within each; then the count cap.
    assert((LoadOrder({"k1.key", "b1.bin", "pipeline-binaries/p1.bin", "s1.spv", "k2.key",
                       "s1.meta", "pipeline-binaries/p2.bin"},
                      10) == Entries{"s1.spv", "s1.meta", "pipeline-binaries/p1.bin",
                                     "pipeline-binaries/p2.bin", "k1.key", "b1.bin", "k2.key"}));
    assert((LoadOrder({"k1.key", "pipeline-binaries/p1.bin", "s1.spv"}, 2) ==
            Entries{"s1.spv", "pipeline-binaries/p1.bin"}));
    // The byte cap: a file is read if it fits beside the bytes read before it.
    assert(Fits(0, MaxBytes) && !Fits(0, MaxBytes + 1) && !Fits(1, MaxBytes));
    assert(Fits(MaxBytes - 10, 10) && !Fits(MaxBytes - 10, 11) && !Fits(MaxBytes + 1, 0));

    // A first-start build's store, with the sizes of one on an AMD GPU (Windows, Radeon 780M):
    // 2,744 permutations (SPIR-V 14,177 bytes and meta 7,375 bytes on average), 3,414 pipeline
    // binaries (10,995 bytes) and 3,532 pipeline keys (1,603 bytes): 12,434 files, 97.6 MB. The
    // list holds them in build order (each permutation's SPIR-V then its meta, then the keys and
    // binaries of the pipeline pass, then the binaries of the optimised rebuilds that come later)
    // and then a race's reads, appended last.
    const auto size_of = [](const std::string& entry) -> size_t {
        return entry.ends_with(".spv")    ? 14177
               : entry.ends_with(".meta") ? 7375
               : entry.ends_with(".key")  ? 1603
                                          : 10995;
    };
    std::string list;
    const auto spv = [](int i) { return "0x" + std::to_string(1000000 + i) + "_0.spv"; };
    const auto meta = [](int i) { return "0x" + std::to_string(1000000 + i) + ".meta"; };
    const auto bin = [](int i) {
        return "pipeline-binaries/" + std::to_string(1000000 + i) + ".bin";
    };
    const auto key = [](int i) { return "g_0x" + std::to_string(1000000 + i) + ".key"; };
    for (int i = 0; i < 2744; ++i) {
        list += spv(i) + '\n' + meta(i) + '\n';
    }
    for (int i = 0; i < 3532; ++i) {
        list += key(i) + '\n';
    }
    for (int i = 0; i < 3414; ++i) {
        list += bin(i) + '\n';
    }
    // A later race read 250 permutations and 200 binaries, some of them early in the build.
    for (int i = 0; i < 250; ++i) {
        list += spv(i * 10) + '\n' + meta(i * 10) + '\n';
    }
    for (int i = 0; i < 200; ++i) {
        list += bin(i * 17) + '\n';
    }
    const auto order = LoadOrder(Recent(list, std::numeric_limits<size_t>::max()), MaxFiles);
    assert(order.size() == MaxFiles);
    // Every shader file is kept, the race's first, each SPIR-V beside its meta.
    for (size_t i = 0; i < 2744 * 2; ++i) {
        assert(Rank(order[i]) == 0);
    }
    assert(order[0] == meta(2490) && order[1] == spv(2490) && order[499] == spv(0));
    for (size_t i = 0; i < 2744 * 2; i += 2) {
        assert(order[i].substr(0, 9) == order[i + 1].substr(0, 9));
    }
    // Then the binaries, the race's first; no pipeline key is kept.
    assert(order[2744 * 2] == bin(199 * 17) && order[2744 * 2 + 199] == bin(0));
    for (size_t i = 2744 * 2; i < order.size(); ++i) {
        assert(Rank(order[i]) == 1);
    }
    // Under the byte cap: all shader files (56.4 MB) and the newest binaries in what is left,
    // the race's 200 among them.
    size_t loaded = 0, shaders = 0, binaries = 0;
    for (const auto& entry : order) {
        if (Fits(loaded, size_of(entry))) {
            loaded += size_of(entry);
            ++(Rank(entry) == 0 ? shaders : binaries);
        }
    }
    std::printf("first-start store: %zu entries kept, %zu shader files and %zu binaries read, "
                "%.1f MB\n",
                order.size(), shaders, binaries, loaded / 1048576.0);
    assert(shaders == 2744 * 2 && binaries >= 200 && binaries < 3414);
    std::puts("cache-preload-test: passed");
    return 0;
}
