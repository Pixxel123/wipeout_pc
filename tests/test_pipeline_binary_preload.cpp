// recent.txt trimming for the pipeline binary preload (vk_pipeline_binary_preload.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_pipeline_binary_preload.h"

using namespace Vulkan::PipelineBinaryPreload;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // Names are lowercase hex driver keys of at most 32 bytes; nothing else becomes a path.
    assert(ValidName("0a1b2c"));
    assert(ValidName(std::string(64, 'f')));
    assert(!ValidName(""));
    assert(!ValidName(std::string(65, 'f')));
    assert(!ValidName("0A1B"));
    assert(!ValidName("../ab"));
    assert(!ValidName("ab.bin"));

    // Newest (last) first, each name once, Windows line ends and junk skipped.
    using Names = std::vector<std::string>;
    assert(Recent("", 10).empty());
    assert((Recent("aa\nbb\ncc\n", 10) == Names{"cc", "bb", "aa"}));
    assert((Recent("aa\nbb\ncc", 10) == Names{"cc", "bb", "aa"}));
    assert((Recent("aa\nbb\naa\ncc\nbb\n", 10) == Names{"bb", "cc", "aa"}));
    assert((Recent("aa\r\n\nzz!\nbb\r\n", 10) == Names{"bb", "aa"}));
    // The cap keeps the newest distinct names.
    assert((Recent("aa\nbb\ncc\ndd\ncc\n", 2) == Names{"cc", "dd"}));
    assert(Recent("aa\nbb\n", 0).empty());
    std::puts("pipeline-binary-preload-test: passed");
    return 0;
}
