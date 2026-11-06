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
    // "Native (window)": the keyword round-trips, and a fixed size turns it off again.
    {
        const auto write_config = [&](const char* text) {
            FILE* config = std::fopen(path, "w");
            assert(config);
            std::fputs(text, config);
            std::fclose(config);
        };
        write_config("output_res=native\n");
        Load();
        assert(s.output_native);
        Save();
        Load();
        assert(s.output_native);
        assert(s.output_res >= 0 && s.output_res < OutputCount);
        write_config("output_res=2560x1440\n");
        Load();
        assert(!s.output_native && s.output_res == 2);
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
    }
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}
