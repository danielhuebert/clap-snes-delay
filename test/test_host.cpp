// Minimal CLAP host harness to validate the SPC Echo plugin end-to-end.
#include <clap/clap.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static const void *host_get_ext(const clap_host_t *, const char *) { return nullptr; }
static void host_noop(const clap_host_t *) {}
static void host_req_restart(const clap_host_t *) {}
static void host_req_process(const clap_host_t *) {}
static void host_req_cb(const clap_host_t *) {}

static clap_host_t g_host = {
    CLAP_VERSION_INIT, nullptr, "test-host", "claude", "", "1.0",
    host_get_ext, host_req_restart, host_req_process, host_req_cb};

// ---- event list carrying param-value events ----
struct EvList {
  clap_input_events_t in;
  std::vector<clap_event_param_value_t> evs;
};
static uint32_t ev_size(const clap_input_events_t *l) {
  return (uint32_t)((EvList *)l->ctx)->evs.size();
}
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) {
  return &((EvList *)l->ctx)->evs[i].header;
}
static bool out_try_push(const clap_output_events_t *, const clap_event_header_t *) { return true; }

// Output-event list that counts how many events the plugin pushes (for the
// Filter-preset -> taps host-sync check).
struct OutCount { clap_output_events_t oe; int n; };
static bool out_count_push(const clap_output_events_t *o, const clap_event_header_t *) {
  ((OutCount *)o->ctx)->n++;
  return true;
}

static clap_event_param_value_t mkparam(clap_id id, double v, uint32_t time) {
  clap_event_param_value_t e;
  std::memset(&e, 0, sizeof(e));
  e.header.size = sizeof(e);
  e.header.time = time;
  e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  e.header.type = CLAP_EVENT_PARAM_VALUE;
  e.header.flags = 0;
  e.param_id = id;
  e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
  e.value = v;
  return e;
}

// ---- in-memory streams for state roundtrip ----
struct MemBuf { std::vector<uint8_t> data; size_t rpos = 0; };
static int64_t mem_write(const clap_ostream_t *s, const void *buf, uint64_t n) {
  auto *m = (MemBuf *)s->ctx;
  const uint8_t *b = (const uint8_t *)buf;
  m->data.insert(m->data.end(), b, b + n);
  return (int64_t)n;
}
static int64_t mem_read(const clap_istream_t *s, void *buf, uint64_t n) {
  auto *m = (MemBuf *)s->ctx;
  uint64_t avail = m->data.size() - m->rpos;
  uint64_t k = n < avail ? n : avail;
  std::memcpy(buf, m->data.data() + m->rpos, k);
  m->rpos += k;
  return (int64_t)k;
}

#define CHECK(c) do { if (!(c)) { std::printf("FAIL: %s\n", #c); return 1; } } while (0)

int main(int argc, char **argv) {
  if (argc < 2) { std::printf("usage: %s <binary-in-bundle>\n", argv[0]); return 2; }

  void *dso = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(dso);
  auto *entry = (const clap_plugin_entry_t *)dlsym(dso, "clap_entry");
  CHECK(entry);
  CHECK(entry->init(argv[1]));

  auto *fac = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  CHECK(fac);
  CHECK(fac->get_plugin_count(fac) == 1);
  const auto *desc = fac->get_plugin_descriptor(fac, 0);
  CHECK(desc);
  std::printf("plugin: %s (%s) v%s\n", desc->name, desc->id, desc->version);

  const clap_plugin_t *plug = fac->create_plugin(fac, &g_host, desc->id);
  CHECK(plug);
  CHECK(plug->init(plug));

  auto *params = (const clap_plugin_params_t *)plug->get_extension(plug, CLAP_EXT_PARAMS);
  auto *ports = (const clap_plugin_audio_ports_t *)plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS);
  auto *state = (const clap_plugin_state_t *)plug->get_extension(plug, CLAP_EXT_STATE);
  CHECK(params && ports && state);
  CHECK(params->count(plug) == 14);
  CHECK(ports->count(plug, true) == 1 && ports->count(plug, false) == 1);

  clap_audio_port_info_t pi;
  CHECK(ports->get(plug, 0, true, &pi) && pi.channel_count == 2);

  clap_output_events_t oe = {nullptr, out_try_push};
  const double sr = 48000.0;

  // Set Time = 16 ms, Mix = 100 %, Feedback = 0 % BEFORE activate, so the
  // smoothed delay starts exactly on target (no glide) for the timing test.
  {
    EvList pre; pre.in.ctx = &pre; pre.in.size = ev_size; pre.in.get = ev_get;
    pre.evs.push_back(mkparam(0, 16.0, 0));
    pre.evs.push_back(mkparam(1, 100.0, 0));
    pre.evs.push_back(mkparam(2, 0.0, 0));
    params->flush(plug, &pre.in, &oe);  // inactive flush = main-thread, allowed
  }

  CHECK(plug->activate(plug, sr, 1, 8192));
  CHECK(plug->start_processing(plug));

  // ---- process an impulse; expect a filtered echo near the delay length ----
  const uint32_t N = 2000;
  std::vector<float> inL(N, 0), inR(N, 0), outL(N, 0), outR(N, 0);
  inL[0] = 1.0f;  // unit impulse on the left channel

  float *inCh[2] = {inL.data(), inR.data()};
  float *outCh[2] = {outL.data(), outR.data()};
  clap_audio_buffer_t ab_in = {inCh, nullptr, 2, 0, 0};
  clap_audio_buffer_t ab_out = {outCh, nullptr, 2, 0, 0};

  EvList none; none.in.ctx = &none; none.in.size = ev_size; none.in.get = ev_get;

  clap_process_t proc;
  std::memset(&proc, 0, sizeof(proc));
  proc.steady_time = 0;
  proc.frames_count = N;
  proc.audio_inputs = &ab_in;
  proc.audio_outputs = &ab_out;
  proc.audio_inputs_count = 1;
  proc.audio_outputs_count = 1;
  proc.in_events = &none.in;
  proc.out_events = &oe;

  CHECK(plug->process(plug, &proc) == CLAP_PROCESS_CONTINUE);

  // Mix is an equal-power crossfade: at 100 % the dry is fully removed, so
  // frame 0 (echo still zero there) must be ~silent.
  CHECK(std::fabs(outL[0]) < 1e-3f);

  // A filtered echo of the impulse must appear around sample 768.
  float peak = 0.f; uint32_t peakAt = 0;
  for (uint32_t i = 760; i < 782; ++i)
    if (std::fabs(outL[i]) > peak) { peak = std::fabs(outL[i]); peakAt = i; }
  std::printf("echo peak |%.4f| at sample %u (expected ~768)\n", peak, peakAt);
  CHECK(peak > 0.05f);
  CHECK(peakAt >= 762 && peakAt <= 776);

  // ---- click test: change Time mid-block, assert no output discontinuity ----
  // The whole delay buffer (up to 480 ms) must be primed with signal first, so
  // the time jump makes the read pointer land on LOUD material -- that is what
  // produces an audible click. The switch point is well past one full buffer.
  {
    const uint32_t M = 60000;          // > max_delay (~23k) so wpos wraps fully
    const uint32_t SW = 50000;         // switch after the buffer is primed
    std::vector<float> sinL(M), sinR(M), oL(M, 0), oR(M, 0);
    for (uint32_t i = 0; i < M; ++i)
      sinL[i] = sinR[i] = 0.5f * std::sin(2.0 * M_PI * 220.0 * i / sr);

    float *ic[2] = {sinL.data(), sinR.data()};
    float *oc[2] = {oL.data(), oR.data()};
    clap_audio_buffer_t bi = {ic, nullptr, 2, 0, 0};
    clap_audio_buffer_t bo = {oc, nullptr, 2, 0, 0};

    // Feedback up so there is real echo content, and jump Time 16 -> 480 ms
    // once the buffer is fully primed.
    EvList ev; ev.in.ctx = &ev; ev.in.size = ev_size; ev.in.get = ev_get;
    ev.evs.push_back(mkparam(2, 60.0, 0));      // Feedback = 60 % at frame 0
    ev.evs.push_back(mkparam(0, 480.0, SW));    // Time jump after priming

    clap_process_t p2 = proc;
    p2.frames_count = M;
    p2.audio_inputs = &bi; p2.audio_outputs = &bo;
    p2.in_events = &ev.in;
    CHECK(plug->process(plug, &p2) == CLAP_PROCESS_CONTINUE);

    // Largest single-sample jump across the whole block. A hard delay-time
    // switch would produce a step of order ~0.5+; the smoothed delay must not.
    auto maxStepIn = [&](uint32_t lo, uint32_t hi, uint32_t *at) {
      float m = 0.f; *at = lo;
      for (uint32_t i = lo; i < hi; ++i) {
        float d = std::fabs(oL[i] - oL[i - 1]);
        if (d > m) { m = d; *at = i; }
      }
      return m;
    };
    uint32_t aBefore, aAfter, aEdge;
    const float sBefore = maxStepIn(1, SW, &aBefore);           // steady 16 ms
    const float sAfter = maxStepIn(SW, M, &aAfter);             // during glide
    const float sEdge = maxStepIn(SW - 2, SW + 4, &aEdge);      // at the switch
    std::printf("max step: before=%.4f@%u  afterGlide=%.4f@%u  atSwitch=%.4f@%u\n",
                sBefore, aBefore, sAfter, aAfter, sEdge, aEdge);
    // The switch itself must not create a discontinuity above the signal's
    // own dynamics, and nothing in the block may show a hard-jump click.
    CHECK(sEdge <= sBefore * 2.0f + 0.01f);
    CHECK(sAfter < 0.30f);
  }

  // ---- state save / load roundtrip ----
  double tBefore = 0; params->get_value(plug, 0, &tBefore);
  MemBuf mb; clap_ostream_t os = {&mb, mem_write};
  CHECK(state->save(plug, &os));
  // Clobber a param, then reload and confirm it is restored.
  EvList el2; el2.in.ctx = &el2; el2.in.size = ev_size; el2.in.get = ev_get;
  el2.evs.push_back(mkparam(0, 16.0, 0));
  params->flush(plug, &el2.in, &oe);
  double tClob = 0; params->get_value(plug, 0, &tClob);
  CHECK(std::fabs(tClob - 16.0) < 1e-6);
  clap_istream_t is = {&mb, mem_read};
  CHECK(state->load(plug, &is));
  double tAfter = 0; params->get_value(plug, 0, &tAfter);
  std::printf("state roundtrip: before=%.1f clobbered=%.1f restored=%.1f\n",
              tBefore, tClob, tAfter);
  CHECK(std::fabs(tAfter - tBefore) < 1e-6);

  // ---- FIR preset: selecting one loads its 8 tap values and tells the host ----
  {
    // Default (Doom) tap 0 must be -6 (0xFA).
    double tap0 = 0; params->get_value(plug, 4, &tap0);
    CHECK(std::fabs(tap0 - (-6.0)) < 1e-6);

    // Select preset 1 = "NSPC Identity" {0x7F,0,0,...}. Expect 8 output events
    // (one per tap) and the taps updated to 127, 0, 0, ...
    OutCount cap; cap.oe.ctx = &cap; cap.oe.try_push = out_count_push; cap.n = 0;
    EvList fe; fe.in.ctx = &fe; fe.in.size = ev_size; fe.in.get = ev_get;
    fe.evs.push_back(mkparam(3 /*Filter*/, 1.0 /*NSPC Identity*/, 0));
    params->flush(plug, &fe.in, &cap.oe);

    double t0 = 0, t1 = 0, filt = 0;
    params->get_value(plug, 4, &t0);
    params->get_value(plug, 5, &t1);
    params->get_value(plug, 3, &filt);
    std::printf("preset load: emitted %d tap events, tap0=%.0f tap1=%.0f filter=%.0f\n",
                cap.n, t0, t1, filt);
    CHECK(cap.n == 8);
    CHECK(std::fabs(t0 - 127.0) < 1e-6 && std::fabs(t1) < 1e-6);
    CHECK(std::fabs(filt - 1.0) < 1e-6);

    // value_to_text for the Filter param should return the preset name.
    char name[64] = {0};
    params->value_to_text(plug, 3, 1.0, name, sizeof(name));
    std::printf("filter #1 name = \"%s\"\n", name);
    CHECK(name[0] != '\0');
  }

  // ---- stereo: Echo R = -100 must give out R = -(out L) (anti-phase wide) ----
  {
    plug->reset(plug);  // clear the delay line from earlier tests
    EvList se; se.in.ctx = &se; se.in.size = ev_size; se.in.get = ev_get;
    se.evs.push_back(mkparam(0, 16.0, 0));     // Time
    se.evs.push_back(mkparam(1, 100.0, 0));    // Mix = 100 % (wet only)
    se.evs.push_back(mkparam(2, 0.0, 0));      // Feedback 0
    se.evs.push_back(mkparam(12, 100.0, 0));   // Echo L = +100 %
    se.evs.push_back(mkparam(13, -100.0, 0));  // Echo R = -100 %
    params->flush(plug, &se.in, &oe);

    const uint32_t N2 = 2000;
    std::vector<float> iL(N2, 0), iR(N2, 0), oL2(N2, 0), oR2(N2, 0);
    iL[0] = iR[0] = 1.0f;  // impulse on both channels
    float *ic[2] = {iL.data(), iR.data()};
    float *oc[2] = {oL2.data(), oR2.data()};
    clap_audio_buffer_t bi = {ic, nullptr, 2, 0, 0};
    clap_audio_buffer_t bo = {oc, nullptr, 2, 0, 0};
    EvList nn; nn.in.ctx = &nn; nn.in.size = ev_size; nn.in.get = ev_get;
    clap_process_t p3 = proc;
    p3.frames_count = N2; p3.audio_inputs = &bi; p3.audio_outputs = &bo;
    p3.in_events = &nn.in;
    CHECK(plug->process(plug, &p3) == CLAP_PROCESS_CONTINUE);

    float maxSum = 0.f, maxAbs = 0.f;
    for (uint32_t i = 0; i < N2; ++i) {
      maxSum = std::fmax(maxSum, std::fabs(oL2[i] + oR2[i]));
      maxAbs = std::fmax(maxAbs, std::fabs(oL2[i]));
    }
    std::printf("stereo invert: max|L| = %.4f  max|L+R| = %.6f\n", maxAbs, maxSum);
    CHECK(maxAbs > 0.05f);  // there is real echo signal
    CHECK(maxSum < 1e-4f);  // R is the exact inverse of L
  }

  plug->stop_processing(plug);
  plug->deactivate(plug);
  plug->destroy(plug);
  entry->deinit();
  dlclose(dso);
  std::printf("ALL CHECKS PASSED\n");
  return 0;
}
