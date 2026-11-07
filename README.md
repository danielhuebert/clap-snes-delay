# SPC Echo — CLAP audio effect

SNES SPC700 S-DSP echo as a desktop [CLAP](https://cleveraudio.org) plugin.
Port of the Korg minilogue xd `spc dly` unit: circular delay buffer, 8-tap
EarthBound/DOOM FIR, and feedback — the same DSP core, adapted to CLAP's
variable sample rate and non-interleaved `float**` buffers.

## Parameters

| Parameter | Range        | SNES analogue | Notes |
|-----------|--------------|---------------|-------|
| Time      | 16 … 480 ms  | EDL (16 ms/step) | delay length |
| Mix       | 0 … 100 %    | EVOL          | equal-power dry/wet crossfade (0 % dry, 100 % wet) |
| Feedback  | 0 … 100 %    | EFB           | echo regeneration (stable; FIR gain ≈ 0.77) |

All three are host-automatable; plugin state (the three values) is saved and
restored via the `clap.state` extension.

## Build

### Quick build (clang only, no CMake)

```sh
./build.sh            # → build/"SPC Echo.clap" (universal arm64 + x86_64)
./build.sh install    # also copies it to ~/Library/Audio/Plug-Ins/CLAP
```

The script fetches the CLAP 1.2.10 headers into `deps/clap/` on first run.

### CMake

```sh
brew install cmake           # if needed
cmake -B build -G Ninja      # or the default generator
cmake --build build
```

## Install

Copy the bundle to the user CLAP folder (or run `./build.sh install`):

```sh
cp -R "build/SPC Echo.clap" ~/Library/Audio/Plug-Ins/CLAP/
```

## Test

Load it in a CLAP host — Bitwig Studio, Reaper, or Studio One. (Logic/GarageBand
do not support CLAP.) For automated ABI checking, run
[`clap-validator`](https://github.com/free-audio/clap-validator):

```sh
clap-validator validate "build/SPC Echo.clap"
```

## Layout

```
src/spc_delay.cpp   the whole plugin: entry, factory, DSP, params, state
build.sh            clang-only bundle build
CMakeLists.txt      idiomatic alternative build
deps/clap/          CLAP headers (fetched, git-ignored)
```
