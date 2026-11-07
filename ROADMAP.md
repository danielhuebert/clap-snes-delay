# SPC Echo — project notes & roadmap

Context to resume work later without losing the "why". For the user-facing
feature description see [README.md](README.md).

## Status — v1.0.0 (feature-complete, no GUI)

A CLAP audio-effect port of the Korg minilogue xd `snes-delay` ("spc dly")
delfx unit: SNES SPC700 S-DSP echo. Builds as a universal (arm64 + x86_64)
`.clap` bundle; verified in Bitwig and by an automated dlopen host harness.

Parameters (14, all host-automatable, saved via `clap.state`, format `SPC3`):
Time, Mix, Feedback, Echo L, Echo R, Filter (preset selector), Tap 1..8.

## Key files

- `src/spc_delay.cpp` — the entire plugin (entry, factory, DSP, params, state).
- `build.sh` — clang-only universal `.clap` build (+ `install`); fetches CLAP
  headers to `deps/clap/` (gitignored). No CMake needed.
- `CMakeLists.txt` — idiomatic alternative build.
- `test/test_host.cpp` — standalone minimal CLAP host that `dlopen`s the bundle
  and checks the whole ABI + DSP (echo timing, click-free time change, state
  roundtrip, FIR preset load, stereo inversion). This is the regression harness.

## Design decisions (and why) — read before changing DSP

1. **Delay-time change = crossfade between two fixed read heads** (`kFadeMs`,
   15 ms), *not* a moving read pointer. This is THE central fix.
   - Rejected: the minilogue single-pointer design (buffer length = delay time)
     → clicks when the time changes (the buffer wrap point jumps).
   - Rejected: slew-limiting the delay length → click-free but strong pitch
     glide ("leiern"), because the read pointer moves at a variable rate.
   - The crossfade keeps both heads at a fixed delay (no glide) and fades ~15 ms
     (no click). Fades are serialized so fast automation stays artifact-free.
   - Write head `wpos` advances continuously (constant wrap point); reads are
     `wpos - delay` with linear interpolation.
2. **Mix = equal-power dry/wet crossfade** (0 % dry, 100 % wet), deliberately
   NOT the SNES additive EVOL. Usability/DAW-familiarity was chosen over
   authenticity here (a mix knob is not part of the S-DSP sound). Also makes it
   usable on a send/aux bus.
3. **FIR fully exposed:** 8 `Tap` params are the raw SNES coefficients as signed
   1.7 fixed-point bytes (-128..127, coeff = byte/128). `Filter` is a selector
   over ~70 presets from sneslab.net/wiki/FIR_Filter; choosing one loads its
   bytes into the taps AND emits them as output param events so the host stays in
   sync. Taps stay editable afterwards (custom FIR). Default preset = Doom (the
   original minilogue sound). Live float coeffs cached in `fir_coeff[]`.
4. **Stereo = authentic EVOL L/R:** `Echo L`/`Echo R` are signed per-side echo
   levels (-100..+100 %, = S-DSP EVOL/128). Output only, never the feedback loop.
   A negative side inverts the echo → classic wide/anti-phase trick. Time,
   feedback and the FIR stay shared across channels, exactly as on the hardware.
   No ping-pong (the S-DSP had none — intentionally omitted).
5. **Safety:** the feedback write is hard-clipped to [-1, 1], so hot FIR presets
   (gain > 0 dB) saturate (SNES-style) instead of overflowing/exploding.
6. **Param ids are stable/append-only;** display order is set by `kParamOrder[]`
   so new params (Echo L/R, ids 12/13) list next to Mix without renumbering.
7. **State** = magic `SPC3` + 14 little-endian doubles (time, mix, feedback,
   filter, tap0..7, echoL, echoR). Bump the magic if the layout changes.

## Build / test / release

```sh
./build.sh install                 # build + copy to ~/Library/Audio/Plug-Ins/CLAP

# run the regression harness:
clang++ -std=c++17 -O2 -Ideps/clap/include test/test_host.cpp -o /tmp/spc_test
/tmp/spc_test "build/SPC Echo.clap/Contents/MacOS/SPC Echo"

# (optional) official ABI check:
clap-validator validate "build/SPC Echo.clap"

# release a prebuilt bundle (zip preserves the bundle + ad-hoc signature):
ditto -c -k --keepParent "build/SPC Echo.clap" SPC-Echo-vX.Y.Z-macOS-universal.clap.zip
gh release create vX.Y.Z SPC-Echo-*.zip --title "..." --notes "..."
```

Distribution reality: notarization needs the paid ($99/yr) Apple Developer
Program. Without it we ship an ad-hoc-signed universal bundle; downloaders run
`xattr -dr com.apple.quarantine "SPC Echo.clap"` once (documented in the README),
or build from source (locally built files aren't quarantined). A free Apple ID
can't notarize or make a distributable Developer ID cert.

## Next steps (planned)

1. **GUI** (`clap.gui` extension) — the big one. An 8-bar FIR editor + preset
   dropdown + frequency-response plot + Time/Mix/Feedback/Echo L-R controls.
   This is what makes custom-FIR editing actually pleasant (8 signed bytes via
   generic host knobs is clumsy). Natural home for the per-filter sneslab notes.
2. **AU / VST3 via clap-wrapper** — so it loads in Logic/GarageBand and other
   non-CLAP hosts.
3. **Notarization** — only once download volume justifies the paid account.
4. Nice-to-haves: optional authentic additive-EVOL mode; tempo-sync for Time;
   add `test/` to CI.

## Reference

- SNES S-DSP FIR filters & coefficients: https://sneslab.net/wiki/FIR_Filter
- CLAP: https://cleveraudio.org — headers pinned at 1.2.10.
- Sibling projects: `spu-reverb` (PSX reverb, has the comb+allpass diffusion a
  single-delay echo like this one cannot do), `amrm`, minilogue `snes-delay`.
