# The GPU library's profile

The GPU library is built with Clang and a recorded profile, `pgo/gow3gpu.profdata`: counts of which
branches and calls ran while the game was played. The compiler uses them to lay hot code together and
to inline along the paths that run. `build.sh` uses the file when it is there, on Linux and on Windows.

## Build switches

- `GOW3_GPU_COMPILER=gcc|clang` (Linux): the compiler of the GPU library. Default: Clang when the
  profile is there, otherwise the default C++ compiler. Clang needs `clang`, `lld` and `llvm`.
- `GOW3_PGO=use|generate|off`: build with the profile, build an instrumented library that records
  one, or neither. Default: `use` when the profile is there.

## Recording a new profile

The profile fits later sources: a function whose code changed is compiled without its counts and the
rest keep theirs. Record it again after larger changes to the renderer.

1. Build the instrumented library: `GOW3_GPU_COMPILER=clang GOW3_PGO=generate ./build.sh`.
2. Play with `LLVM_PROFILE_FILE=/some/folder/gow3gpu-%p.profraw` set: races on several tracks, at
   each output size you care about. The library writes its file every 30 seconds
   (`PGO: profile written` in the log).
3. Merge the files with the `llvm-profdata` of the Clang that built the library:
   `llvm-profdata merge /some/folder/*.profraw -o pgo/gow3gpu.profdata`.
4. Build again without `GOW3_PGO` and commit the new file.

Clang 18 cannot apply the profile to the command processor (`amdgpu/liverpool.cpp`): it aborts on the
counts of the packet loop, a coroutine. That file is built without the profile.
