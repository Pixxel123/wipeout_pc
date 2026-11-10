// SPDX-License-Identifier: GPL-2.0-or-later
// GOW3_ENV_VIEW_DIAG (vk_env_view_diag.h): the face tests and the drop decisions over synthetic
// frames shaped like WipEout's (the camera cube's six faces, then three of another ship's cube).
// No game or Vulkan device needed.
#include <cassert>
#include <cstdio>
#include <string>

#include "video_core/renderer_vulkan/vk_env_view_diag.h"

using namespace Vulkan::EnvViewDiag;

namespace {
constexpr std::uint64_t Camera = 0x111ad98000, Other1 = 0x1118fa0000, Other2 = 0x111b508000;
constexpr std::uint64_t Depth = 0x111b2f0000, Display1 = 0x1026330000, Display2 = 0x1025b38000;

struct Result {
    int drawn = 0, dropped = 0;
};

/// One frame: `draws` draws on each of the camera cube's six faces, then three faces of `other`
/// starting at `other_first`, then the display pass.
Result Frame(Tracker& t, int frame, std::uint64_t other, std::uint32_t other_first, int draws = 4) {
    Result r;
    const auto face = [&](std::uint64_t cube, std::uint32_t slice) {
        for (int d = 0; d < draws; ++d) {
            (t.OnViewDraw(cube, slice, Depth) ? r.dropped : r.drawn)++;
        }
    };
    for (std::uint32_t s = 0; s < 6; ++s) {
        face(Camera, s);
    }
    for (std::uint32_t s = other_first; s < other_first + 3; ++s) {
        face(other, s);
    }
    t.OnDepthOnlyDraw(true, 0x111cf60000);
    t.OnDepthOnlyDraw(true, Depth); // a cube's depth clear: not a ship map
    t.OnDisplayDraw(frame & 1 ? Display2 : Display1, frame * 0.02, false);
    return r;
}
} // namespace

int main() {
    // Mode names.
    assert(ParseMode(nullptr) == Mode::Off && ParseMode("") == Mode::Off);
    assert(ParseMode("0") == Mode::Off && ParseMode("off") == Mode::Off);
    assert(ParseMode("count") == Mode::Count && ParseMode("1") == Mode::Count);
    assert(ParseMode("all") == Mode::All && ParseMode("cam-half") == Mode::CamHalf);
    assert(ParseMode("cam-alt") == Mode::CamAlt && ParseMode("bogus") == Mode::Off);

    // The register test.
    constexpr Target C0{true, Kind::B10G11R11Float, 256, 256}, C1{true, Kind::R16Float, 256, 256};
    assert(ColourPair(C0, 0xf, C1, 0xf));
    assert(!ColourPair(C0, 0xf, C1, 0));                                     // slot 1 masked
    assert(!ColourPair(C0, 0xf, {false, Kind::R16Float, 256, 256}, 0xf));    // no slot 1
    assert(!ColourPair(C0, 0xf, {true, Kind::R16Float, 128, 128}, 0xf));     // a mip level
    assert(!ColourPair({true, Kind::R16Float, 256, 256}, 0xf, C1, 0xf));     // exposure chain
    assert(!ColourPair({true, Kind::B10G11R11Float, 1024, 512}, 0xf, C1, 0xf)); // billboard
    assert(ViewDepth({false, Kind::Other, 0, 0}));
    assert(ViewDepth({true, Kind::D16, 256, 256}));
    assert(!ViewDepth({true, Kind::D16, 512, 512}));
    assert(!ViewDepth({true, Kind::Other, 256, 256}));
    // The pipeline test: the exports that reach a bound target are slots 0 and 1.
    assert(Outputs(0b11, 0b11));
    // The faces' material shaders also export slot 2, with no target bound there: a face.
    assert(Outputs(0b111, 0b11));
    // Slot 2 bound (a stale target with a non-zero mask) and written: not a face.
    assert(!Outputs(0b111, 0b111));
    // Slot 1 not written (a single-target pass with a stale slot 1 target): not a face.
    assert(!Outputs(0b1, 0b11));
    assert(!Outputs(0, 0b11) && !Outputs(0b11, 0b1));

    // count: nothing dropped; the camera cube is known from the third frame on.
    {
        Tracker t{Mode::Count};
        for (int f = 0; f < 6; ++f) {
            const auto r = Frame(t, f, f & 2 ? Other2 : Other1, f & 1 ? 3 : 0);
            assert(r.dropped == 0 && r.drawn == 36);
            assert(t.CameraKnown() == (f >= 1));
        }
    }
    // all: every face, from the first frame.
    {
        Tracker t{Mode::All};
        for (int f = 0; f < 4; ++f) {
            const auto r = Frame(t, f, Other1, 0);
            assert(r.dropped == 36 && r.drawn == 0);
        }
    }
    // cam-half: three camera faces a frame, alternating; the other cube untouched.
    {
        Tracker t{Mode::CamHalf};
        Frame(t, 0, Other1, 0);
        Frame(t, 1, Other1, 3);
        for (int f = 2; f < 8; ++f) {
            const auto r = Frame(t, f, f & 2 ? Other2 : Other1, f & 1 ? 3 : 0);
            assert(r.dropped == 12 && r.drawn == 24);
        }
        // Even frame: faces 0, 2, 4 dropped; odd frame: 1, 3, 5.
        assert(!t.OnViewDraw(Camera, 1, Depth) && t.OnViewDraw(Camera, 0, Depth));
    }
    // cam-alt: the camera cube's six faces every other frame.
    {
        Tracker t{Mode::CamAlt};
        Frame(t, 0, Other1, 0);
        Frame(t, 1, Other1, 3);
        int dropped = 0;
        for (int f = 2; f < 8; ++f) {
            const auto r = Frame(t, f, Other1, f & 1 ? 3 : 0);
            assert(r.dropped == (f & 1 ? 24 : 0));
            dropped += r.dropped;
        }
        assert(dropped == 72);
    }
    // A frame that does not begin with the same six faces: the cam modes drop nothing next frame.
    {
        Tracker t{Mode::CamHalf};
        Frame(t, 0, Other1, 0);
        Frame(t, 1, Other1, 3);
        assert(t.CameraKnown());
        // Photo mode or a menu: no faces at all.
        t.OnDisplayDraw(Display1, 1.0, false);
        assert(!t.CameraKnown());
        const auto r = Frame(t, 3, Other1, 0);
        assert(r.dropped == 0);
    }
    // The HUD draws into the display buffer after the copy: only the first draw closes a frame.
    {
        Tracker t{Mode::Count};
        Frame(t, 0, Other1, 0);
        Frame(t, 1, Other1, 3);
        assert(t.CameraKnown());
        t.OnDisplayDraw(Display2, 0.5, false); // same display buffer as frame 1: no new frame
        assert(t.CameraKnown());
    }
    // The stats line, every five seconds when asked for.
    {
        Tracker t{Mode::Count};
        assert(!t.OnDisplayDraw(Display1, 0.0, true));
        for (int f = 0; f < 10; ++f) {
            Frame(t, f + 1, Other1, 0, 4);
        }
        const auto line = t.OnDisplayDraw(Display2, 6.0, true);
        assert(line && line->find("Env views (count)") == 0);
        std::printf("%s\n", line->c_str());
    }
    // Trust in the register test: after TrustAfter agreements, unless a mismatch was ever seen.
    {
        Tracker t{Mode::All};
        for (std::uint32_t i = 0; i < Tracker::TrustAfter; ++i) {
            assert(!t.EarlyTrusted());
            t.OnAgreement();
        }
        assert(t.EarlyTrusted());
        t.OnMismatch();
        assert(!t.EarlyTrusted());
        Tracker c{Mode::Count};
        for (std::uint32_t i = 0; i < 2 * Tracker::TrustAfter; ++i) {
            c.OnAgreement();
        }
        assert(!c.EarlyTrusted()); // count checks every draw
    }
    std::puts("env view diag: ok");
}
