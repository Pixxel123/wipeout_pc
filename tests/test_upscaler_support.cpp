// SPDX-License-Identifier: GPL-2.0-or-later
// Saved FSR 4 choices must be safe before the first rendered frame on older GPUs.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unistd.h>
#include <utility>
#include "gow3_settings.h"

int main() {
    using namespace Gow3Settings;
    char path[] = "/tmp/gow3-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("GOW3_CONFIG", path, 1);
    unsetenv("GOW3_UPSCALER");
    unsetenv("GOW3_UPSCALE_PRESET");
    unsetenv("GOW3_MIN_RENDER_SIZE");
    unsetenv("GOW3_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                FILE* config = std::fopen(path, "w");
                assert(config);
                std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                             UpscalerName(requested));
                std::fclose(config);
                s.fsr4_problem = nullptr;
                Load();
                assert(s.upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                const bool unsupported = (requested == UpscalerFsr4 && !fsr4) ||
                    (requested == UpscalerFsr411 && !(fsr4 && fsr411));
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
                assert(s.fsr4_supported == fsr4);
                assert(s.fsr411_supported == (fsr4 && fsr411));
                assert(bool(s.fsr4_problem.load()) == unsupported);
                assert(s.preset == Performance && s.output_res == 2);
                // Startup patch settings still describe the already applied guest patches.
                assert(s.startup_upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
            }
        }
    }
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("GOW3_RENDER_RES", "1706x960", 1);
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("GOW3_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("GOW3_RENDER_RES");
    // An old "output_res=native" (a dropped choice) is the default size.
    {
        FILE* config = std::fopen(path, "w");
        assert(config);
        std::fputs("output_res=native\n", config);
        std::fclose(config);
        Load();
        assert(s.output_res == OutputDefault);
        Save();
        config = std::fopen(path, "r");
        assert(config);
        char text[4096] = {};
        const size_t n = std::fread(text, 1, sizeof(text) - 1, config);
        std::fclose(config);
        assert(n > 0 && std::strstr(text, "output_res=1920x1080\n"));
    }
    // The guest size of the session: the startup render resolution, built-in or a mod's.
    {
        const auto guest = [&](const char* text) {
            FILE* config = std::fopen(path, "w");
            assert(config);
            std::fputs(text, config);
            std::fclose(config);
            Load();
            return std::pair{s.startup_render_width, s.startup_render_height};
        };
        assert(guest("render_resolution=native\n") == std::pair(1920, 1080));
        assert(guest("render_resolution=2560x1080\n") == std::pair(2560, 1080));
        assert(guest("render_resolution=4K\n") == std::pair(3840, 2160));
        setenv("GOW3_EXTRA_RESOLUTIONS", "3840x1080", 1);
        assert(guest("render_resolution=3840x1080\n") == std::pair(3840, 1080));
        unsetenv("GOW3_EXTRA_RESOLUTIONS");
        // A title's own render size (WipEout's profile): below it the game keeps that buffer.
        assert(guest("render_resolution=720p\n") == std::pair(1280, 720)); // no floor: God of War III
        setenv("GOW3_MIN_RENDER_SIZE", "1920x1080", 1);
        assert(guest("render_resolution=720p\n") == std::pair(1920, 1080));
        assert(guest("render_resolution=900p\n") == std::pair(1920, 1080));
        assert(guest("render_resolution=2560x1080\n") == std::pair(2560, 1080));
        unsetenv("GOW3_MIN_RENDER_SIZE");
    }
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}
