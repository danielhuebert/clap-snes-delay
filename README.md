# SPC Echo — CLAP audio effect

SNES SPC700 S-DSP echo as a desktop [CLAP](https://cleveraudio.org) plugin.
Port of the Korg minilogue xd `spc dly` unit: circular delay buffer, an 8-tap
FIR, and feedback — the same DSP core, adapted to CLAP's variable sample rate
and non-interleaved `float**` buffers, with a selectable/editable FIR filter.

## Parameters

| Parameter | Range        | SNES analogue | Notes |
|-----------|--------------|---------------|-------|
| Time      | 16 … 480 ms  | EDL (16 ms/step) | delay length; changes crossfade (click- and glide-free) |
| Mix       | 0 … 100 %    | EVOL          | equal-power dry/wet crossfade (0 % dry, 100 % wet) |
| Feedback  | 0 … 100 %    | EFB           | echo regeneration |
| Echo L    | −100 … 100 % | EVOL(L)       | left echo level; negative inverts that side |
| Echo R    | −100 … 100 % | EVOL(R)       | right echo level; negative inverts that side |
| Filter    | preset list  | FIR preset    | selects an 8-tap SNES FIR, loading it into Tap 1…8 |
| Tap 1…8   | −128 … 127   | FIR[0]…FIR[7] | the FIR coefficients (1.7 fixed point, byte ÷ 128) |

All parameters are host-automatable; plugin state (all values, including the
eight taps) is saved and restored via the `clap.state` extension.

**Stereo (Echo L / Echo R):** the S-DSP had a separate, signed echo volume per
side (EVOL L/R), so the echo could be panned or placed in the stereo field, and
inverting one side (negative volume) produces a wide, anti-phase echo. Echo L/R
reproduce this; they affect the output only, never the feedback loop. At the
default +100 / +100 both sides are equal (mono-compatible); set one side
negative for the classic wide trick. Delay time, feedback and the FIR are shared
across both channels, exactly as on the hardware.

## FIR filter & presets

The echo path runs an 8-tap FIR over the delayed signal — the main tone-shaper
of the SNES echo. It is fully exposed:

- **Tap 1…8** are the raw SNES coefficients as signed 1.7 fixed-point bytes
  (`0x00`…`0x7F` = 0…+127, `0x80`…`0xFF` = −128…−1; coefficient = byte ÷ 128,
  so `0x7F` ≈ +1.0, `0x80` = −1.0). `Tap 1` = FIR[0] multiplies the *oldest*
  delayed sample, `Tap 8` = FIR[7] the newest.
- **Filter** is a preset selector: choosing one loads its eight bytes into the
  Tap parameters (and reports them back to the host), after which you can edit
  the taps freely to build a custom FIR.

**Gain/stability:** per the SnesLab wiki, for click-free echo the absolute sum
of the taps should stay ≤ 128 (0 dB); above that, loud feedback can make the
echo overflow. Here the feedback write is hard-clipped to [−1, 1], so a hot
filter saturates (SNES-style) rather than blowing up — but several presets have
gain > 0 dB, so pair them with moderate Feedback.

All coefficient sets are from
[sneslab.net/wiki/FIR_Filter](https://sneslab.net/wiki/FIR_Filter); the exact
bytes live in `kPresets[]` in [`src/spc_delay.cpp`](src/spc_delay.cpp). Default
is **Doom** (the original minilogue sound). Included presets:

- **N-SPC built-ins:** Identity (echo unchanged), HP 3 kHz, LP 5 kHz, BP 1.5–8.5 kHz.
- **Low-pass:** Bio Metal, Clue, Dragon Quest / III / VI (several cutoffs),
  Earthworm Jim 2, Justice League, Kyuuyaku Megami Tensei, Magic Boy,
  Chrono Trigger / "many games" 5 kHz, Momotarou Dentetsu, Solid Runner,
  Strike Gunner, Super Bomberman 5, Treasure Hunter G, UFO Kamen, Zenki.
- **High-pass:** Bobby's World, Battle Cross, Donkey Kong Country 2, G.O.D
  (`01 02 04 08 10 20 40 80`, very effective), "most used" 3 kHz, Terranigma.
- **Band-pass:** "many games" 1.5–8.5 kHz, Popful Mail 3–10 kHz.
- **Band-stop:** Chaos Seed, Do-Re-Mi Fantasy, "many games" 6–12 kHz,
  Popful Mail, Super Bomberman 3 (×2), Tengai Makyou Zero.
- **Unusual / FX:** Alien vs. Predator, Angelique, B.O.B, DKC2 W-shape,
  DKC mute (echo almost silent), **Doom**, Dragon Quest VI (×3), Jim Power,
  Kamaitachi no Yoru, Kamen Rider SD, Mario is Missing (all `0A`), Monster
  Maker 3, Prince of Persia 2, Seiken Densetsu 3, Sengoku Denshou,
  Star Ocean / Tales of Phantasia, Super Genjin 2, Super Tekkyuu Fight,
  Tactics Ogre, The Flintstones, The Mask (×2), The Simpsons (×2), WWF, and more.

See the wiki for each filter's frequency-response description.

## Download (prebuilt, macOS)

Grab `SPC-Echo-<version>-macOS-universal.clap.zip` from the
[latest release](https://github.com/danielhuebert/clap-snes-delay/releases),
then:

```sh
unzip SPC-Echo-*-macOS-universal.clap.zip
xattr -dr com.apple.quarantine "SPC Echo.clap"      # clear the download quarantine
cp -R "SPC Echo.clap" ~/Library/Audio/Plug-Ins/CLAP/
```

Restart your DAW (or rescan plugins) afterwards. The bundle is a universal
(arm64 + x86_64) build, **ad-hoc signed but not notarized** — notarization needs
a paid Apple Developer account. The one-time `xattr` step tells macOS Gatekeeper
to allow the plugin; on Apple Silicon the ad-hoc signature is enough for the DAW
to load it once the quarantine flag is gone. Building from source (below) avoids
the quarantine altogether.

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
test/test_host.cpp  standalone CLAP host harness (dlopen) — regression checks
build.sh            clang-only bundle build
CMakeLists.txt      idiomatic alternative build
ROADMAP.md          project status, design decisions, next steps
deps/clap/          CLAP headers (fetched, git-ignored)
```

Run the harness:

```sh
clang++ -std=c++17 -O2 -Ideps/clap/include test/test_host.cpp -o /tmp/spc_test
/tmp/spc_test "build/SPC Echo.clap/Contents/MacOS/SPC Echo"
```
