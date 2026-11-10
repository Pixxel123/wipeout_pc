// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_DRAW_REPEAT_STATS=1, how often a direct draw repeats the state of the draw before it
// in the same render pass. A measurement, not an optimisation: it changes nothing that is drawn.
//
// Counted on the draw recording thread (stage B) after each draw packet, from values that thread
// already holds: the pipeline, the vertex and index bindings, the texture-set memo keys, the
// render-state memo, its register copy and the register blocks each packet carries. Stage A
// counts only the draws that never reach stage B (no pipeline). Stage A prints at the end of each
// submission, after its drain: stage B is idle then and its plain counters are complete.
// The definition of each counter is at the counter (Counter below).
//
// Output:
//   stdout every 5 s and at exit: "Draw census [5s|exit]: ms=<ms of day> name=value ...", the
//     cumulative counters (one line), then a line of this window's rates for reading by eye;
//   with GOW3_FRAME_LOG: <frame log>.census, a header naming the columns, then one line of the
//     cumulative counters per submission (ms_of_day first, the clock of the frame log).
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace DrawCensus {

/// GOW3_DRAW_REPEAT_STATS=1, read once.
inline const bool enabled = [] {
    const char* env = std::getenv("GOW3_DRAW_REPEAT_STATS");
    return env && env[0] == '1';
}();

/// Cumulative counters. "Previous draw" is the previous recorded direct draw. "Same pass": that
/// draw is in the same Vulkan render pass instance as this one (Scheduler::RenderPasses did not
/// move since it was recorded, so this draw did not begin rendering, and no indirect draw came
/// in between). The "_any" counters drop the same-pass condition.
enum Counter : std::uint32_t {
    LogDraws,   ///< Gow3Stats::draws at the report (the frame log's draws column), stage A
    NoPipeline, ///< Rasterizer::Draw calls with no pipeline (compiling, skipped), stage A
    Special,    ///< draws FilterDraw handles (fast clear elimination, resolve, depth copy, skip)
    Hidden,     ///< draw packets left out by Hide HUD (not recorded)
    Draws,      ///< direct draws recorded on stage B: the denominator of the rates below
    Indirect,   ///< indirect draws (not in LogDraws; the next direct draw has no previous)
    First,      ///< recorded draws with no previous draw in the same pass
    /// 1. Same pass, same vk::Pipeline handle (the scaled-blend squaring pipeline is ignored).
    Pipe,
    /// Same vk::Pipeline handle as the previous draw in any pass (the descriptor-reuse
    /// diagnostic's "same layout handle as the previous push", 48-53 % at the countdown).
    PipeAny,
    /// 2. Same pass and the vertex and index binds would record the same commands: the vertex
    /// input descriptions (bindings with stride and divisor, attributes with format and offset),
    /// the buffer count and per buffer the handle, offset, size and stride; both draws
    /// non-indexed, or both indexed with the same index buffer handle, offset and index type.
    Vertex,
    /// 3. Same pass and the same texture-set memo keys: per graphics stage with images, in stage
    /// order, TextureSetKey(ImageDescHash of each T#, the stage), as BindTextures keys the memo
    /// (vk_texture_set_key.h). Samplers are not part of it. A draw with a stage the memo does
    /// not take (TextureSetCandidate false) has no key and never counts here (see Unkeyed).
    Textures,
    Unkeyed, ///< recorded draws with a stage whose images have no texture-set key
    /// 4. Same pass and the render-state memo hit (BeginRendering returned the memoized state).
    RenderState,
    /// 5. Same pass and every register word pipeline selection reads holds the same value as at
    /// the previous draw (SelectionWords in vk_rasterizer.cpp; user-data words excluded): the
    /// precondition of a selection memo that compares values.
    Selection,
    SelectionAny, ///< the same values, any pass (a selection memo runs on stage A, pass-blind)
    /// 5w. Same pass, no ClearState, and no 16-word register block holding a selection word was
    /// written since the previous draw (the blocks the packets carried): a write-based memo's
    /// precondition. Blocks also hold words selection does not read, so this is a lower bound.
    SelectionUnwritten,
    /// 6. Sum, over draws with a previous draw in the same pass, of the 16-word register blocks
    /// written since that draw (the union of the blocks of every packet in between).
    Blocks,
    /// 7. Same pass and 1-4 all hold: the draws differ at most in constants (user data, constant
    /// buffers) and draw parameters. Also the joint rate 1+2+3+4.
    Same1234,
    Same12345,  ///< 1-4 and 5 (values)
    Same1234W,  ///< 1-4 and 5w (no selection block written)
    // Runs of draws chained by Same1234: a draw that is not Same1234 ends the run before it and
    // starts one. Counted when the run ends, as runs and as the draws in them, by length.
    Runs1, Runs2, Runs3to4, Runs5to8, Runs9to16, Runs17up,
    RunDraws1, RunDraws2, RunDraws3to4, RunDraws5to8, RunDraws9to16, RunDraws17up,
    CounterCount,
};

inline constexpr const char* Names[] = {
    "log_draws", "no_pipeline", "special",   "hidden",     "draws",     "indirect",
    "first",     "pipe",        "pipe_any",  "vertex",     "textures",  "unkeyed",
    "render_state", "selection", "selection_any", "selection_unwritten", "blocks",
    "same1234",  "same12345",   "same1234w",
    "runs_1",    "runs_2",      "runs_3_4",  "runs_5_8",   "runs_9_16", "runs_17up",
    "rdraws_1",  "rdraws_2",    "rdraws_3_4", "rdraws_5_8", "rdraws_9_16", "rdraws_17up",
};
static_assert(sizeof(Names) / sizeof(Names[0]) == CounterCount);

using Counters = std::array<std::uint64_t, CounterCount>;

/// Closes a run of `length` draws.
inline void EndRun(Counters& c, std::uint64_t length) {
    if (length == 0) {
        return;
    }
    const std::uint32_t bucket = length == 1 ? 0 : length == 2 ? 1 : length <= 4 ? 2
                               : length <= 8 ? 3 : length <= 16 ? 4 : 5;
    ++c[Runs1 + bucket];
    c[RunDraws1 + bucket] += length;
}

/// The draws all counters account for, to check against the frame log's draws column.
[[nodiscard]] inline std::uint64_t Counted(const Counters& c) {
    return c[NoPipeline] + c[Special] + c[Hidden] + c[Draws];
}

inline std::string Format(const Counters& c, std::uint32_t ms_of_day, bool names) {
    std::string line = names ? "ms=" + std::to_string(ms_of_day) : std::to_string(ms_of_day);
    line += names ? " counted=" : " ";
    line += std::to_string(Counted(c));
    for (std::uint32_t i = 0; i < CounterCount; ++i) {
        line += ' ';
        if (names) {
            line += Names[i];
            line += '=';
        }
        line += std::to_string(c[i]);
    }
    return line;
}

inline std::string Header() {
    std::string line = "# ms_of_day counted";
    for (const char* name : Names) {
        line += ' ';
        line += name;
    }
    return line;
}

/// The last stdout line, for the line printed at exit.
inline std::mutex last_mutex;
inline std::string last_line;
inline void PrintAtExit() {
    std::scoped_lock lock{last_mutex};
    if (!last_line.empty()) {
        std::printf("Draw census [exit]: %s\n", last_line.c_str());
        std::fflush(stdout);
    }
}

} // namespace DrawCensus
