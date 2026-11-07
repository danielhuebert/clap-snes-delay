// SPC Echo — SNES SPC700 S-DSP echo/delay as a CLAP audio effect.
//
// Port of the Korg minilogue xd "spc dly" delfx unit to the CLAP desktop
// plugin format. Same DSP core (circular delay buffer + 8-tap FIR + feedback),
// adapted from the logue SDK's fixed 48 kHz / interleaved environment to CLAP's
// variable sample rate and non-interleaved (float**) buffers.
//
// Parameters (host-automatable):
//   Time      16 .. 480 ms   (SNES EDL, 16 ms per step)
//   Mix        0 .. 100 %    equal-power dry/wet crossfade (0% dry, 100% wet)
//   Feedback   0 .. 100 %    echo regeneration (EFB); stable because FIR gain < 1
//
// Build: see CMakeLists.txt or build.sh. Output is a `.clap` bundle on macOS.

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// DSP constants (identical to the minilogue xd original)
// ---------------------------------------------------------------------------

// EarthBound / DOOM SNES FIR coefficients.
// Raw signed int8 bytes FA 0A 1A 13 13 1A 0A FA, scaled by 1/128.
// tap[0] = oldest delayed sample, tap[7] = newest. Summed gain ~= 0.77.
static const float k_fir[8] = {
    -6.f / 128.f, 10.f / 128.f, 26.f / 128.f, 19.f / 128.f,
    19.f / 128.f, 26.f / 128.f, 10.f / 128.f, -6.f / 128.f};

static constexpr double kMinDelayMs = 16.0;   // SNES minimum (one EDL step)
static constexpr double kMaxDelayMs = 480.0;  // SNES maximum (EDL = 15)

// When the delay time changes, two read heads (old + new time) are crossfaded
// over this window instead of moving one head. Both heads stay at a fixed delay,
// so there is no pitch glide ("leiern"); the short fade avoids the click.
static constexpr float kFadeMs = 15.0f;

static inline float clip1m1f(float x) {
  return x < -1.f ? -1.f : (x > 1.f ? 1.f : x);
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

enum {
  kParamTime = 0,      // ms
  kParamMix = 1,       // percent
  kParamFeedback = 2,  // percent
  kParamCount = 3,
};

// ---------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------

struct SpcDelay {
  clap_plugin_t plugin;  // must be first; host holds a pointer to this
  const clap_host_t *host = nullptr;

  // Parameter values in plain (displayed) units. Atomic so the main thread
  // (get_value / inactive flush) and the audio thread never tear a read.
  std::atomic<double> p_time_ms{100.0};
  std::atomic<double> p_mix{50.0};
  std::atomic<double> p_feedback{30.0};

  // Processing state (only touched on the audio thread once active).
  double sample_rate = 48000.0;
  std::vector<float> buf_l;  // delay line, sized in activate()
  std::vector<float> buf_r;
  uint32_t max_delay = 0;    // allocated length (>= 480 ms of samples)
  uint32_t wpos = 0;         // write head: always advances, so the buffer's
                             // wrap point never moves -> no click on time change

  // Crossfade-based delay-time changes (two read heads, fixed per fade).
  float d_active = 0.f;      // delay currently playing (samples)
  float d_from = 0.f;        // crossfade source delay
  float d_to = 0.f;          // crossfade destination delay
  float d_pending = 0.f;     // latest requested target (fades are serialized)
  float fade_gain = 1.f;     // 0..1 position within the current crossfade
  float fade_inc = 0.f;      // per-sample fade increment (set in activate)
  bool  fading = false;

  float fir_l[8] = {0};      // FIR history, [0]=oldest .. [7]=newest
  float fir_r[8] = {0};

  // ----- DSP helpers -----
  static inline float fir_apply(const float h[8]) {
    return h[0] * k_fir[0] + h[1] * k_fir[1] + h[2] * k_fir[2] + h[3] * k_fir[3] +
           h[4] * k_fir[4] + h[5] * k_fir[5] + h[6] * k_fir[6] + h[7] * k_fir[7];
  }
  static inline void fir_push(float h[8], float v) {
    h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = h[4];
    h[4] = h[5]; h[5] = h[6]; h[6] = h[7]; h[7] = v;
  }

  // Target delay length in samples, clamped so the interpolating read always
  // has a valid sample pair inside the buffer.
  float delayTarget() const {
    const double ms = p_time_ms.load(std::memory_order_relaxed);
    float d = (float)(ms * 0.001 * sample_rate);
    const float lo = (float)(kMinDelayMs * 0.001 * sample_rate);
    const float hi = (float)max_delay - 2.f;
    if (d < lo) d = lo;
    if (d > hi) d = hi;
    return d;
  }

  void clearState() {
    std::fill(buf_l.begin(), buf_l.end(), 0.f);
    std::fill(buf_r.begin(), buf_r.end(), 0.f);
    for (int i = 0; i < 8; ++i) fir_l[i] = fir_r[i] = 0.f;
    wpos = 0;
    d_active = d_from = d_to = d_pending = delayTarget();
    fade_gain = 1.f;
    fading = false;
  }

  // Read one delay tap (linear interpolation) at a fixed delay in samples.
  inline void readTap(float d, uint32_t maxd, float &oL, float &oR) const {
    float rp = (float)wpos - d;
    if (rp < 0.f) rp += (float)maxd;
    uint32_t i0 = (uint32_t)rp;
    const float frac = rp - (float)i0;
    uint32_t i1 = i0 + 1;
    if (i1 >= maxd) i1 -= maxd;
    oL = buf_l[i0] + frac * (buf_l[i1] - buf_l[i0]);
    oR = buf_r[i0] + frac * (buf_r[i1] - buf_r[i0]);
  }

  // Process a contiguous run of frames. A Time change starts a short crossfade
  // between the old and new delay (both fixed), so there is neither a click nor
  // a pitch glide.
  void run(float *outL, float *outR, const float *inL, const float *inR,
           uint32_t n) {
    const float target = delayTarget();
    const float mix = (float)(p_mix.load(std::memory_order_relaxed) * 0.01);
    const float fb = (float)(p_feedback.load(std::memory_order_relaxed) * 0.01);
    const uint32_t maxd = max_delay;

    // Equal-power dry/wet crossfade (standard DAW behaviour, not the SNES's
    // additive EVOL): 0% = dry only, 100% = wet only, constant perceived
    // loudness across the range. Feedback is unaffected -- Mix is output only.
    const float wetGain = std::sin(mix * 1.57079633f);  // pi/2
    const float dryGain = std::cos(mix * 1.57079633f);

    // Record the latest requested delay; begin a fade if idle and it moved.
    d_pending = target;
    if (!fading && d_pending != d_active) {
      d_from = d_active;
      d_to = d_pending;
      fade_gain = 0.f;
      fading = true;
    }

    for (uint32_t i = 0; i < n; ++i) {
      const float dryL = inL[i];
      const float dryR = inR[i];

      float rdL, rdR;
      if (fading) {
        float aL, aR, bL, bR;
        readTap(d_from, maxd, aL, aR);
        readTap(d_to, maxd, bL, bR);
        const float g = fade_gain;
        rdL = aL + g * (bL - aL);
        rdR = aR + g * (bR - aR);

        fade_gain += fade_inc;
        if (fade_gain >= 1.f) {
          fade_gain = 1.f;
          fading = false;
          d_active = d_to;
          // Chain to the next fade if Time moved again during this one.
          if (d_pending != d_active) {
            d_from = d_active;
            d_to = d_pending;
            fade_gain = 0.f;
            fading = true;
          }
        }
      } else {
        readTap(d_active, maxd, rdL, rdR);
      }

      fir_push(fir_l, rdL);
      fir_push(fir_r, rdR);
      const float filtL = fir_apply(fir_l);
      const float filtR = fir_apply(fir_r);

      // Write input + feedback at the (constant-wrap) write head.
      buf_l[wpos] = clip1m1f(dryL + fb * filtL);
      buf_r[wpos] = clip1m1f(dryR + fb * filtR);

      if (++wpos >= maxd) wpos = 0;

      outL[i] = clip1m1f(dryGain * dryL + wetGain * filtL);
      outR[i] = clip1m1f(dryGain * dryR + wetGain * filtR);
    }
  }
};

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

static const char *const s_features[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
    CLAP_PLUGIN_FEATURE_STEREO, nullptr};

static const clap_plugin_descriptor_t s_descriptor = {
    CLAP_VERSION_INIT,
    "com.danielhuebert.spc-echo",
    "SPC Echo",
    "Daniel Huebert",
    "https://github.com/danielhuebert",
    "",
    "",
    "1.0.0",
    "SNES SPC700 S-DSP echo: delay + 8-tap FIR + feedback.",
    s_features,
};

// ---------------------------------------------------------------------------
// audio-ports extension: one stereo input, one stereo output
// ---------------------------------------------------------------------------

static uint32_t ap_count(const clap_plugin_t *, bool) { return 1; }

static bool ap_get(const clap_plugin_t *, uint32_t index, bool is_input,
                   clap_audio_port_info_t *info) {
  if (index != 0) return false;
  info->id = is_input ? 0 : 1;
  std::snprintf(info->name, sizeof(info->name), "%s",
                is_input ? "Input" : "Output");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  // Allow the host to reuse one buffer for in and out (in-place).
  info->in_place_pair = is_input ? 1 : 0;
  return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {ap_count, ap_get};

// ---------------------------------------------------------------------------
// params extension
// ---------------------------------------------------------------------------

static uint32_t pr_count(const clap_plugin_t *) { return kParamCount; }

static bool pr_get_info(const clap_plugin_t *, uint32_t index,
                        clap_param_info_t *info) {
  if (index >= kParamCount) return false;
  std::memset(info, 0, sizeof(*info));
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  info->module[0] = '\0';
  info->cookie = nullptr;
  switch (index) {
    case kParamTime:
      info->id = kParamTime;
      std::snprintf(info->name, sizeof(info->name), "Time");
      info->min_value = kMinDelayMs;
      info->max_value = kMaxDelayMs;
      info->default_value = 100.0;
      return true;
    case kParamMix:
      info->id = kParamMix;
      std::snprintf(info->name, sizeof(info->name), "Mix");
      info->min_value = 0.0;
      info->max_value = 100.0;
      info->default_value = 50.0;
      return true;
    case kParamFeedback:
      info->id = kParamFeedback;
      std::snprintf(info->name, sizeof(info->name), "Feedback");
      info->min_value = 0.0;
      info->max_value = 100.0;
      info->default_value = 30.0;
      return true;
  }
  return false;
}

static bool pr_get_value(const clap_plugin_t *plugin, clap_id id,
                         double *out) {
  auto *self = (SpcDelay *)plugin->plugin_data;
  switch (id) {
    case kParamTime: *out = self->p_time_ms.load(); return true;
    case kParamMix: *out = self->p_mix.load(); return true;
    case kParamFeedback: *out = self->p_feedback.load(); return true;
  }
  return false;
}

static bool pr_value_to_text(const clap_plugin_t *, clap_id id, double value,
                             char *out, uint32_t cap) {
  switch (id) {
    case kParamTime: std::snprintf(out, cap, "%.0f ms", value); return true;
    case kParamMix:
    case kParamFeedback: std::snprintf(out, cap, "%.0f %%", value); return true;
  }
  return false;
}

static bool pr_text_to_value(const clap_plugin_t *, clap_id, const char *text,
                             double *out) {
  if (!text) return false;
  *out = std::atof(text);
  return true;
}

// Apply a single incoming parameter-value event.
static void applyParamEvent(SpcDelay *self, const clap_event_param_value_t *ev) {
  switch (ev->param_id) {
    case kParamTime: self->p_time_ms.store(ev->value); break;
    case kParamMix: self->p_mix.store(ev->value); break;
    case kParamFeedback: self->p_feedback.store(ev->value); break;
  }
}

static void pr_flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                     const clap_output_events_t *) {
  auto *self = (SpcDelay *)plugin->plugin_data;
  const uint32_t n = in->size(in);
  for (uint32_t i = 0; i < n; ++i) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
        h->type == CLAP_EVENT_PARAM_VALUE)
      applyParamEvent(self, (const clap_event_param_value_t *)h);
  }
}

static const clap_plugin_params_t s_params = {
    pr_count,        pr_get_info,     pr_get_value,
    pr_value_to_text, pr_text_to_value, pr_flush};

// ---------------------------------------------------------------------------
// state extension — serialize the three plain parameter values
// ---------------------------------------------------------------------------

static const uint32_t kStateMagic = 0x53504331;  // 'SPC1'

static bool writeAll(const clap_ostream_t *os, const void *p, uint64_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint64_t done = 0;
  while (done < n) {
    int64_t w = os->write(os, b + done, n - done);
    if (w <= 0) return false;
    done += (uint64_t)w;
  }
  return true;
}

static bool readAll(const clap_istream_t *is, void *p, uint64_t n) {
  uint8_t *b = (uint8_t *)p;
  uint64_t done = 0;
  while (done < n) {
    int64_t r = is->read(is, b + done, n - done);
    if (r <= 0) return false;
    done += (uint64_t)r;
  }
  return true;
}

static bool st_save(const clap_plugin_t *plugin, const clap_ostream_t *os) {
  auto *self = (SpcDelay *)plugin->plugin_data;
  uint32_t magic = kStateMagic;
  double vals[3] = {self->p_time_ms.load(), self->p_mix.load(),
                    self->p_feedback.load()};
  return writeAll(os, &magic, sizeof(magic)) &&
         writeAll(os, vals, sizeof(vals));
}

static bool st_load(const clap_plugin_t *plugin, const clap_istream_t *is) {
  auto *self = (SpcDelay *)plugin->plugin_data;
  uint32_t magic = 0;
  double vals[3] = {0, 0, 0};
  if (!readAll(is, &magic, sizeof(magic)) || magic != kStateMagic) return false;
  if (!readAll(is, vals, sizeof(vals))) return false;
  self->p_time_ms.store(std::clamp(vals[0], kMinDelayMs, kMaxDelayMs));
  self->p_mix.store(std::clamp(vals[1], 0.0, 100.0));
  self->p_feedback.store(std::clamp(vals[2], 0.0, 100.0));
  return true;
}

static const clap_plugin_state_t s_state = {st_save, st_load};

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

static bool plug_init(const clap_plugin_t *) { return true; }

static void plug_destroy(const clap_plugin_t *plugin) {
  delete (SpcDelay *)plugin->plugin_data;
}

static bool plug_activate(const clap_plugin_t *plugin, double sample_rate,
                          uint32_t, uint32_t) {
  auto *self = (SpcDelay *)plugin->plugin_data;
  self->sample_rate = sample_rate;
  // 480 ms of headroom plus a guard; allocate once here, never in process().
  self->max_delay =
      (uint32_t)std::ceil(kMaxDelayMs * 0.001 * sample_rate) + 8;
  self->buf_l.assign(self->max_delay, 0.f);
  self->buf_r.assign(self->max_delay, 0.f);
  const float fade_len = (float)(kFadeMs * 0.001 * sample_rate);
  self->fade_inc = fade_len > 1.f ? 1.f / fade_len : 1.f;
  self->clearState();
  return true;
}

static void plug_deactivate(const clap_plugin_t *) {}
static bool plug_start_processing(const clap_plugin_t *) { return true; }
static void plug_stop_processing(const clap_plugin_t *) {}

static void plug_reset(const clap_plugin_t *plugin) {
  ((SpcDelay *)plugin->plugin_data)->clearState();
}

static clap_process_status plug_process(const clap_plugin_t *plugin,
                                        const clap_process_t *process) {
  auto *self = (SpcDelay *)plugin->plugin_data;
  const uint32_t nframes = process->frames_count;

  if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1)
    return CLAP_PROCESS_CONTINUE;

  const clap_audio_buffer_t &in = process->audio_inputs[0];
  clap_audio_buffer_t &out = process->audio_outputs[0];

  // This example handles 32-bit non-interleaved stereo.
  if (!in.data32 || !out.data32 || in.channel_count < 1 ||
      out.channel_count < 1)
    return CLAP_PROCESS_CONTINUE;

  const float *inL = in.data32[0];
  const float *inR = in.channel_count > 1 ? in.data32[1] : in.data32[0];
  float *outL = out.data32[0];
  float *outR = out.channel_count > 1 ? out.data32[1] : out.data32[0];

  // Walk events sample-accurately, processing the gaps between them.
  const clap_input_events_t *evs = process->in_events;
  const uint32_t nev = evs ? evs->size(evs) : 0;
  uint32_t ev_i = 0;
  uint32_t cur = 0;

  while (cur < nframes) {
    // Apply every event scheduled at or before the current frame.
    while (ev_i < nev) {
      const clap_event_header_t *h = evs->get(evs, ev_i);
      if (h->time > cur) break;
      if (h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
          h->type == CLAP_EVENT_PARAM_VALUE)
        applyParamEvent(self, (const clap_event_param_value_t *)h);
      ++ev_i;
    }

    uint32_t next = nframes;
    if (ev_i < nev) {
      const clap_event_header_t *h = evs->get(evs, ev_i);
      if (h->time < nframes) next = h->time;
    }

    self->run(outL + cur, outR + cur, inL + cur, inR + cur, next - cur);
    cur = next;
  }

  return CLAP_PROCESS_CONTINUE;
}

static const void *plug_get_extension(const clap_plugin_t *, const char *id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &s_state;
  return nullptr;
}

static void plug_on_main_thread(const clap_plugin_t *) {}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *) {
  return 1;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(
    const clap_plugin_factory_t *, uint32_t index) {
  return index == 0 ? &s_descriptor : nullptr;
}

static const clap_plugin_t *factory_create_plugin(
    const clap_plugin_factory_t *, const clap_host_t *host,
    const char *plugin_id) {
  if (!plugin_id || std::strcmp(plugin_id, s_descriptor.id) != 0) return nullptr;

  auto *self = new SpcDelay();
  self->host = host;
  self->plugin.desc = &s_descriptor;
  self->plugin.plugin_data = self;
  self->plugin.init = plug_init;
  self->plugin.destroy = plug_destroy;
  self->plugin.activate = plug_activate;
  self->plugin.deactivate = plug_deactivate;
  self->plugin.start_processing = plug_start_processing;
  self->plugin.stop_processing = plug_stop_processing;
  self->plugin.reset = plug_reset;
  self->plugin.process = plug_process;
  self->plugin.get_extension = plug_get_extension;
  self->plugin.on_main_thread = plug_on_main_thread;
  return &self->plugin;
}

static const clap_plugin_factory_t s_factory = {
    factory_get_plugin_count, factory_get_plugin_descriptor,
    factory_create_plugin};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static bool entry_init(const char *) { return true; }
static void entry_deinit(void) {}

static const void *entry_get_factory(const char *factory_id) {
  if (!std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &s_factory;
  return nullptr;
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
