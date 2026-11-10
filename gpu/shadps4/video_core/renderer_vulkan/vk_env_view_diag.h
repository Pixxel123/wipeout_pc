// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_ENV_VIEW_DIAG, a timing diagnostic for WipEout's ship reflection cubes. Off unless
// the variable is set; it then counts the cube faces the game draws and, in the skip modes, drops
// some of their draws so the cost of drawing them can be measured. The skip modes leave the
// reflections stale or empty: they are for measuring, not for playing.
//
// A face is recognised by its targets alone: colour slot 0 B10G11R11 float 256x256, slot 1 R16
// float 256x256, no other colour output (the pixel shader's outputs, checked against the
// pipeline), depth absent or D16 256x256. The game draws the camera ship's cube (all six faces)
// every frame, first, and three faces of one other ship's cube after it. A face is a colour
// target address and first slice. Only the sky and scene draws of a face are dropped: the depth
// clear before it, the mip chain and the blur after it still run, on the cube's old contents.
//
// Modes: count (drop nothing, report), all (drop every face), cam-half (the camera cube's even or
// odd faces, alternating by frame), cam-alt (the camera cube's six faces every other frame). The
// camera cube's faces are the first six faces of the frame, taken from the previous frame and
// trusted only when the frame before that began with the same six; otherwise the cam modes drop
// nothing that frame.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace Vulkan::EnvViewDiag {

enum class Mode : std::uint8_t { Off, Count, All, CamHalf, CamAlt };

inline Mode ParseMode(const char* value) {
    if (!value || !value[0] || std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) {
        return Mode::Off;
    }
    if (std::strcmp(value, "count") == 0 || std::strcmp(value, "1") == 0) {
        return Mode::Count;
    }
    if (std::strcmp(value, "all") == 0) {
        return Mode::All;
    }
    if (std::strcmp(value, "cam-half") == 0) {
        return Mode::CamHalf;
    }
    if (std::strcmp(value, "cam-alt") == 0) {
        return Mode::CamAlt;
    }
    std::printf("Env views diagnostic: unknown GOW3_ENV_VIEW_DIAG=%s (count, all, cam-half, "
                "cam-alt); off\n",
                value);
    return Mode::Off;
}

inline const char* Name(Mode mode) {
    switch (mode) {
    case Mode::Off:
        return "off";
    case Mode::Count:
        return "count";
    case Mode::All:
        return "all";
    case Mode::CamHalf:
        return "cam-half";
    case Mode::CamAlt:
        return "cam-alt";
    }
    return "?";
}

enum class Kind : std::uint8_t { Other, B10G11R11Float, R16Float, D16 };

struct Target {
    bool present;
    Kind kind;
    std::uint32_t width;
    std::uint32_t height;
};

/// The register test, before the pipeline is known: the two colour targets and the depth.
[[nodiscard]] constexpr bool ColourPair(Target color0, std::uint32_t mask0, Target color1,
                                        std::uint32_t mask1) noexcept {
    const auto view = [](const Target& t, Kind kind) {
        return t.present && t.kind == kind && t.width == 256 && t.height == 256;
    };
    return view(color0, Kind::B10G11R11Float) && mask0 != 0 && view(color1, Kind::R16Float) &&
           mask1 != 0;
}

[[nodiscard]] constexpr bool ViewDepth(Target depth) noexcept {
    return !depth.present || (depth.kind == Kind::D16 && depth.width == 256 && depth.height == 256);
}

/// The pipeline test: the pixel shader writes colour slots 0 and 1 and nothing else (the port's
/// own motion-vector output excluded by the caller).
[[nodiscard]] constexpr bool Outputs(std::uint32_t colour_outputs) noexcept {
    return colour_outputs == 0b11;
}

class Tracker {
public:
    /// Agreements between the register test and the pipeline test before the register test alone
    /// decides (and a dropped draw also skips pipeline selection). count keeps checking.
    static constexpr std::uint32_t TrustAfter = 1024;

    explicit Tracker(Mode mode_) : mode{mode_} {}

    Mode GetMode() const {
        return mode;
    }
    bool EarlyTrusted() const {
        return mode != Mode::Count && mismatches == 0 && agreements >= TrustAfter;
    }
    void OnAgreement() {
        ++agreements;
    }
    /// The register test matched a draw the pipeline test rejects: never trust it again.
    void OnMismatch() {
        ++mismatches;
    }
    void OnNearMiss() {
        ++frame.near_misses;
    }

    /// A draw into a face (colour target `address`, first slice `slice`): true to drop it.
    bool OnViewDraw(std::uint64_t address, std::uint32_t slice, std::uint64_t depth) {
        ++frame.draws;
        if (depth) {
            NoteCubeDepth(depth);
        }
        const Face key{address, slice};
        Face* face = FindFace(key);
        if (!face) {
            ++frame.untracked;
            return false;
        }
        bool skip = false;
        const int ordinal = CameraOrdinal(key);
        switch (mode) {
        case Mode::Off:
        case Mode::Count:
            break;
        case Mode::All:
            skip = true;
            break;
        case Mode::CamHalf:
            skip = ordinal >= 0 && std::uint64_t(ordinal & 1) == (frame_index & 1u);
            break;
        case Mode::CamAlt:
            skip = ordinal >= 0 && (frame_index & 1u) != 0;
            break;
        }
        if (skip) {
            face->skipped = true;
            ++frame.skipped_draws;
        }
        return skip;
    }

    /// A draw with a depth target and no colour output. `square_d16` for a square D16 target of
    /// 64 to 512 pixels: the per-ship shadow maps and the sun visibility map, unless it is a
    /// cube's depth (its clear).
    void OnDepthOnlyDraw(bool square_d16, std::uint64_t depth) {
        ++frame.depth_only;
        if (square_d16 && !IsCubeDepth(depth)) {
            ++frame.depth_square;
        }
    }

    /// A draw into a display buffer. The first one after a frame's other draws closes the frame;
    /// returns a stats line when `report` and at least five seconds have passed since the last.
    std::optional<std::string> OnDisplayDraw(std::uint64_t address, double now, bool report) {
        if (address == last_display) {
            return std::nullopt;
        }
        last_display = address;
        CloseFrame();
        if (!report) {
            return std::nullopt;
        }
        if (window_start < 0) {
            window_start = now;
            totals = {};
            return std::nullopt;
        }
        if (now - window_start < 5.0) {
            return std::nullopt;
        }
        std::string line = Format(now - window_start);
        window_start = now;
        totals = {};
        return line;
    }

    /// For tests: whether the current frame has a camera cube.
    bool CameraKnown() const {
        return camera_valid;
    }

private:
    struct Face {
        std::uint64_t address;
        std::uint32_t slice;
        bool skipped = false;
        bool operator==(const Face& other) const {
            return address == other.address && slice == other.slice;
        }
    };
    struct Frame {
        std::uint64_t draws, skipped_draws, depth_only, depth_square, untracked, near_misses;
    };
    struct Totals {
        std::uint64_t frames, faces, cubes, camera_frames, one_address, skipped_faces, draws,
            skipped_draws, depth_only, depth_square, untracked, near_misses;
    };

    Face* FindFace(const Face& key) {
        for (std::uint32_t i = 0; i < num_faces; ++i) {
            if (faces[i] == key) {
                return &faces[i];
            }
        }
        if (num_faces == faces.size()) {
            return nullptr;
        }
        faces[num_faces] = {key.address, key.slice, false};
        return &faces[num_faces++];
    }

    /// The face's place among the camera cube's six (the first six faces of the last two frames,
    /// the same in both), or -1.
    int CameraOrdinal(const Face& key) const {
        if (!camera_valid) {
            return -1;
        }
        for (int i = 0; i < 6; ++i) {
            if (camera[i] == key) {
                return i;
            }
        }
        return -1;
    }

    void NoteCubeDepth(std::uint64_t depth) {
        if (IsCubeDepth(depth)) {
            return;
        }
        cube_depths[next_cube_depth++ % cube_depths.size()] = depth;
    }
    bool IsCubeDepth(std::uint64_t depth) const {
        for (const auto d : cube_depths) {
            if (d == depth) {
                return true;
            }
        }
        return false;
    }

    void CloseFrame() {
        std::array<std::uint64_t, 32> addresses{};
        std::uint32_t num_addresses = 0;
        for (std::uint32_t i = 0; i < num_faces; ++i) {
            totals.skipped_faces += faces[i].skipped;
            bool seen = false;
            for (std::uint32_t a = 0; a < num_addresses; ++a) {
                seen |= addresses[a] == faces[i].address;
            }
            if (!seen) {
                addresses[num_addresses++] = faces[i].address;
            }
        }
        ++totals.frames;
        totals.faces += num_faces;
        totals.cubes += num_addresses;
        totals.camera_frames += camera_valid;
        totals.draws += frame.draws;
        totals.skipped_draws += frame.skipped_draws;
        totals.depth_only += frame.depth_only;
        totals.depth_square += frame.depth_square;
        totals.untracked += frame.untracked;
        totals.near_misses += frame.near_misses;
        // The next frame's camera cube: this frame's first six faces, when the previous frame
        // began with the same six in the same order (dropping draws does not change which faces
        // the game issues, so this holds while a skip mode runs).
        bool same = num_faces >= 6;
        bool one_address = num_faces >= 6;
        for (std::uint32_t i = 0; i < 6 && same; ++i) {
            same = first6[i] == faces[i];
        }
        for (std::uint32_t i = 0; i < 6 && one_address; ++i) {
            one_address &= faces[i].address == faces[0].address;
        }
        totals.one_address += one_address;
        camera_valid = same;
        for (std::uint32_t i = 0; i < 6; ++i) {
            camera[i] = i < num_faces ? Face{faces[i].address, faces[i].slice, false} : Face{0, 0};
            first6[i] = camera[i];
        }
        num_faces = 0;
        frame = {};
        ++frame_index;
    }

    std::string Format(double seconds) const {
        const double n = totals.frames ? double(totals.frames) : 1.0;
        const double draws = double(totals.draws);
        char buffer[704];
        std::snprintf(
            buffer, sizeof(buffer),
            "Env views (%s): %llu frames in %.1f s; a frame: %.2f faces on %.2f targets, %.1f "
            "draws; dropped %.2f faces, %.1f draws (%.1f%%); camera cube known in %.0f%% of "
            "frames, its six faces on one target in %.0f%%; depth-only %.1f draws (square D16 "
            "64-512, not a cube's depth: %.1f); register test %s (%llu agreements, %llu "
            "mismatches); colour pair with other depth %.1f, untracked %.1f draws a frame",
            Name(mode), static_cast<unsigned long long>(totals.frames), seconds,
            totals.faces / n, totals.cubes / n, draws / n, totals.skipped_faces / n,
            totals.skipped_draws / n,
            draws > 0 ? 100.0 * double(totals.skipped_draws) / draws : 0.0,
            100.0 * totals.camera_frames / n, 100.0 * totals.one_address / n,
            totals.depth_only / n, totals.depth_square / n,
            EarlyTrusted() ? "trusted" : "checked", static_cast<unsigned long long>(agreements),
            static_cast<unsigned long long>(mismatches), totals.near_misses / n,
            totals.untracked / n);
        return buffer;
    }

    Mode mode;
    std::array<Face, 32> faces{};
    std::uint32_t num_faces = 0;
    std::array<Face, 6> first6{};
    std::array<Face, 6> camera{};
    bool camera_valid = false;
    std::array<std::uint64_t, 8> cube_depths{};
    std::uint32_t next_cube_depth = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t last_display = 0;
    Frame frame{};
    Totals totals{};
    double window_start = -1.0;
    std::uint64_t agreements = 0;
    std::uint64_t mismatches = 0;
};

} // namespace Vulkan::EnvViewDiag
