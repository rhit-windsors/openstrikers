// Host stand-in for MusyX's GameCube hardware layer (hw_dolphin.c).
//
// On the GameCube the audio interface DMAs a 5 ms buffer (160 stereo samples
// at 32 kHz) to the DAC and raises an interrupt; MusyX's handler
// (snd_handle_irq in hardware.c) runs the synth, builds a DSP command list for
// the next buffer (salCtrlDsp) and the DSP mixes it in the background.
//
// Here there is no interrupt and no DSP. The game thread calls
// musyxHostPump() once per host frame and runs as many 5 ms ticks as the
// output needs, so MusyX and the game never run concurrently and its
// interrupt-masking calls can be no-ops. The DSP's work -- mixing the voices
// MusyX describes -- is done by the host mixer in salCtrlDsp (see
// docs/PLAN-audio.md).

#include "musyx/musyx.h"
#include "musyx/hardware.h"
#include "musyx/sal.h"
#include "musyx/dspvoice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMA_BUFFER_LEN 0x280 // bytes: 160 stereo s16 frames, 5 ms at 32 kHz

static SND_SOME_CALLBACK userCallback;
u8 salAIBufferIndex;
static s16* salAIBufferBase;
static int salAiRunning;
static unsigned long salTicks;
static int salPeakVoices;

bool salInitAi(SND_SOME_CALLBACK callback, u32 unk, u32* outFreq) {
  (void)unk;
  salAIBufferBase = (s16*)salMallocPhysical(DMA_BUFFER_LEN * 4);
  if (salAIBufferBase == NULL) {
    return FALSE;
  }
  memset(salAIBufferBase, 0, DMA_BUFFER_LEN * 4);
  salAIBufferIndex = 1;
  userCallback = callback;
  synthInfo.numSamples = 0x20;
  *outFreq = 32000;
  fprintf(stderr, "openstrikers: musyx: AI initialised (32000 Hz, host-driven)\n");
  return TRUE;
}

bool salStartAi() {
  salAiRunning = 1;
  return TRUE;
}

bool salExitAi() {
  salAiRunning = 0;
  userCallback = NULL;
  salFree(salAIBufferBase);
  salAIBufferBase = NULL;
  return TRUE;
}

void* salAiGetDest() {
  u8 index = (salAIBufferIndex + 2) % 4;
  return (u8*)salAIBufferBase + index * DMA_BUFFER_LEN;
}

static void initSrcTaps(void);

bool salInitDsp(u32 flags) {
  (void)flags;
  initSrcTaps();
  return TRUE;
}

bool salExitDsp() { return TRUE; }

u32 salGetStartDelay() { return 0; }

// ---------------------------------------------------------------------------
// The host "DSP": mixes what MusyX's parameter blocks describe.
//
// A 5 ms frame is five 1 ms sub-frames of 32 samples. For each sub-frame
// MusyX queues (field index, value) pairs in the voice's patchData (pitch,
// envelope, the running flag at field 7), counted by pb->update.updNum[s];
// the DSP applied them before rendering that sub-frame. Then per output
// sample: step through the sample at the 16.16 ratio in pb->src, decode
// DSP-ADPCM / PCM16 / PCM8 from ARAM, apply the envelope (pb->ve, delta per
// sample) and the main left/right volumes (pb->mix, deltas per sample over
// the frame). Positions are written back to the block, which is where MusyX
// reads them (dsp_vptr->currentAddr, hwGetPos) on the next frame. A sample
// that ends "loops" to its loop address -- for one-shots, ARAM's zero buffer;
// MusyX ends those voices itself from its own play-position count.
//
// Voices are mixed per studio into MusyX's own bus buffers (main, aux A,
// aux B), so its effect callbacks run on them as on the hardware; see
// hostMixFrame, which also does ITD (the interaural delay of 3D voices) and
// encodes a Dolby Pro Logic II studio's rear channels into the stereo pair.

#include <dolphin/ar.h>

#define SUBFRAMES 5
#define SUBFRAME_SAMPLES 32
#define FRAME_SAMPLES (SUBFRAMES * SUBFRAME_SAMPLES)

static s16 sMixOut[FRAME_SAMPLES * 2];

static s16 clamp16(s32 v) { return (s16)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

static u32 pbAddr(u16 hi, u16 lo) { return ((u32)hi << 16) | lo; }

// Fetch the next source sample at the voice's current address and advance.
static s16 fetchSample(_PB* pb, const u8* aram, u32 aramSize) {
  u32 addr = pbAddr(pb->addr.currentAddressHi, pb->addr.currentAddressLo);
  u32 end = pbAddr(pb->addr.endAddressHi, pb->addr.endAddressLo);
  s16 out = 0;

  switch (pb->addr.format) {
  case 0x0A: // PCM16, address in samples, big-endian in ARAM
    if (addr * 2 + 1 < aramSize) {
      out = (s16)((aram[addr * 2] << 8) | aram[addr * 2 + 1]);
    }
    break;
  case 0x19: // PCM8, address in bytes
    if (addr < aramSize) {
      out = (s16)((s8)aram[addr] << 8);
    }
    break;
  default: { // DSP-ADPCM, address in nibbles; 8-byte frames of header + 14 samples
    s32 nib, s, c1, c2;
    u32 ps;
    if ((addr & 15) == 0) {
      if ((addr >> 1) < aramSize) {
        pb->adpcm.pred_scale = aram[addr >> 1];
      }
      addr += 2;
    }
    if ((addr >> 1) >= aramSize) {
      break;
    }
    nib = (addr & 1) ? (aram[addr >> 1] & 0xF) : (aram[addr >> 1] >> 4);
    if (nib >= 8) {
      nib -= 16;
    }
    ps = pb->adpcm.pred_scale;
    c1 = (s16)pb->adpcm.a[(ps >> 4) & 7][0];
    c2 = (s16)pb->adpcm.a[(ps >> 4) & 7][1];
    s = ((nib << (ps & 0xF)) << 11) + c1 * (s16)pb->adpcm.yn1 + c2 * (s16)pb->adpcm.yn2;
    out = clamp16((s + 1024) >> 11);
    pb->adpcm.yn2 = pb->adpcm.yn1;
    pb->adpcm.yn1 = (u16)out;
  } break;
  }

  if (addr >= end) {
    addr = pbAddr(pb->addr.loopAddressHi, pb->addr.loopAddressLo);
    if (pb->addr.format == 0 && pb->loopType == 0) {
      pb->adpcm.pred_scale = pb->adpcmLoop.loop_pred_scale;
      pb->adpcm.yn1 = pb->adpcmLoop.loop_yn1;
      pb->adpcm.yn2 = pb->adpcmLoop.loop_yn2;
    }
  } else {
    ++addr;
  }
  pb->addr.currentAddressHi = (u16)(addr >> 16);
  pb->addr.currentAddressLo = (u16)addr;
  return out;
}

// Sample-rate conversion. pb->srcSelect picks the DSP's converter: 0 is a
// 4-tap polyphase filter over the last four source samples, 1 linear
// interpolation, 2 none. MusyX's default is 0 with coefficient set 1
// (hwSetPolyPhaseFilter); the sets are read here as low-pass cutoffs of 8, 12
// and 16 kHz on the 32 kHz output. The DSP's own tables are not used: these
// are Hann-windowed sinc filters designed here, 128 phases, each phase
// normalised to unity gain at DC (1.14 fixed point).
#define SRC_PHASES 128

static s16 sSrcTaps[3][SRC_PHASES][4];

static void initSrcTaps(void) {
  static const double cutoff[3] = {0.5, 0.75, 1.0}; // of the Nyquist frequency
  const double pi = 3.14159265358979323846;
  int set, p, k;

  for (set = 0; set < 3; ++set) {
    for (p = 0; p < SRC_PHASES; ++p) {
      double t = (double)p / SRC_PHASES;
      double h[4], sum = 0.0;
      int acc = 0;
      for (k = 0; k < 4; ++k) {
        // Tap k sits at source position k; the output point is at 1 + t.
        double d = k - 1 - t;
        double x = pi * cutoff[set] * d;
        double sinc = x == 0.0 ? 1.0 : sin(x) / x;
        double w = 0.5 * (1.0 + cos(pi * d / 2.0));
        h[k] = sinc * w;
        sum += h[k];
      }
      for (k = 0; k < 4; ++k) {
        sSrcTaps[set][p][k] = (s16)lrint(h[k] / sum * 16384.0);
        acc += sSrcTaps[set][p][k];
      }
      // Put the rounding error on the largest tap so DC passes exactly.
      sSrcTaps[set][p][t < 0.5 ? 1 : 2] += (s16)(16384 - acc);
    }
  }
}

static s32 resample(const _PB* pb, u32 frac) {
  s32 s0 = (s16)pb->src.last_samples[0];
  s32 s1 = (s16)pb->src.last_samples[1];
  s32 s2 = (s16)pb->src.last_samples[2];
  s32 s3 = (s16)pb->src.last_samples[3];
  const s16* taps;

  switch (pb->srcSelect) {
  case 2:
    return s3;
  case 1:
    return s2 + (((s3 - s2) * (s32)frac) >> 16);
  default:
    taps = sSrcTaps[pb->coefSelect < 3 ? pb->coefSelect : 1][(frac >> 9) & (SRC_PHASES - 1)];
    return clamp16((s0 * taps[0] + s1 * taps[1] + s2 * taps[2] + s3 * taps[3]) >> 14);
  }
}

static u8 sVoiceSeen[256];
static unsigned long sVoiceStart[256];

// hardware.c: which main buffer (of 2) and aux buffer (of 3) this frame uses.
extern u8 salFrame;
extern u8 salAuxFrame;

// Bus layout of a studio buffer (main[], auxA[], auxB[]): left, right and
// surround, 160 s32 samples each.
#define BUS_L 0
#define BUS_R FRAME_SAMPLES
#define BUS_S (FRAME_SAMPLES * 2)

typedef struct HostVoiceBuses {
  s32* main;
  s32* auxA;
  s32* auxB;
} HostVoiceBuses;

// Mix a voice's 160 samples into the studio's main, aux A and aux B buses,
// with the volumes in pb->mix (each ramped per sample by its delta).
static void mixVoice(DSPvoice* dv, const HostVoiceBuses* bus, const u8* aram, u32 aramSize) {
  _PB* pb = dv->pb;
  u16* fields = (u16*)pb;
  const u16* patch = (const u16*)dv->patchData;
  int sub, i;
  s32 last = 0, smpL, smpR;
  static int sTrace = -1;
  // ITD delay line: MusyX's own per-voice itdBuffer (32 s16, cleared when an
  // ITD voice starts), with the write position kept here.
  static u8 sItdPos[256];
  s16* itd = NULL;
  u8* itdPos = &sItdPos[(dv - dspVoice) & 255];

  if (sTrace < 0) {
    const char* t = getenv("OPENSTRIKERS_AUDIO_TRACE");
    sTrace = t != NULL && t[0] >= '2';
  }
  if (sTrace && !sVoiceSeen[dv - dspVoice]) {
    sVoiceSeen[dv - dspVoice] = 1;
    sVoiceStart[dv - dspVoice] = salTicks;
    fprintf(stderr,
            "openstrikers: [%lu] voice %d start: smp %u comp %u fmt 0x%x addr 0x%x loop 0x%x end 0x%x loopType %u "
            "ratio 0x%x src %u ps 0x%x coef %d,%d vol %d/%d/%d auxA %d/%d auxB %d/%d mixer 0x%x lpf %u itd %u %u/%u\n",
            salTicks, (int)(dv - dspVoice), dv->smp_id, dv->smp_info.compType, pb->addr.format,
            pbAddr(pb->addr.currentAddressHi, pb->addr.currentAddressLo),
            pbAddr(pb->addr.loopAddressHi, pb->addr.loopAddressLo),
            pbAddr(pb->addr.endAddressHi, pb->addr.endAddressLo), pb->loopType,
            pbAddr(pb->src.ratioHi, pb->src.ratioLo), pb->srcSelect, pb->adpcm.pred_scale,
            (s16)pb->adpcm.a[0][0], (s16)pb->adpcm.a[0][1], (s16)pb->mix.vL, (s16)pb->mix.vR,
            (s16)pb->mix.vS, (s16)pb->mix.vAuxAL, (s16)pb->mix.vAuxAR, (s16)pb->mix.vAuxBL,
            (s16)pb->mix.vAuxBR, pb->mixerCtrl, pb->lpf.flag, pb->itd.flag, pb->itd.shiftL, pb->itd.shiftR);
  }

  for (sub = 0; sub < SUBFRAMES; ++sub) {
    u32 ratio;
    for (i = 0; i < pb->update.updNum[sub]; ++i, patch += 2) {
      if (patch[0] < sizeof(_PB) / 2) {
        fields[patch[0]] = patch[1];
      }
    }
    itd = pb->itd.flag && dv->itdBuffer != NULL ? (s16*)dv->itdBuffer : NULL;
    if (pb->state != 1) {
      continue;
    }
    ratio = pb->srcSelect == 2 ? 0x10000 : pbAddr(pb->src.ratioHi, pb->src.ratioLo);
    if (itd != NULL) {
      // Shifts move to their targets a sample per millisecond, so a moving
      // source does not click.
      if (pb->itd.shiftL != pb->itd.targetShiftL) {
        pb->itd.shiftL += pb->itd.shiftL < pb->itd.targetShiftL ? 1 : -1;
      }
      if (pb->itd.shiftR != pb->itd.targetShiftR) {
        pb->itd.shiftR += pb->itd.shiftR < pb->itd.targetShiftR ? 1 : -1;
      }
    }
    for (i = 0; i < SUBFRAME_SAMPLES; ++i) {
      int n = sub * SUBFRAME_SAMPLES + i;
      u32 frac = pb->src.currentAddressFrac;
      s32 smp = resample(pb, frac);
      s32 env = pb->ve.currentVolume;
      u32 steps;

      if (pb->lpf.flag) {
        // One-pole low-pass, coefficients in 1.15.
        smp = (smp * (s32)(s16)pb->lpf.a0 + (s32)(s16)pb->lpf.yn1 * (s32)(s16)pb->lpf.b0) >> 15;
        pb->lpf.yn1 = (u16)clamp16(smp);
      }
      smp = (smp * env) >> 15;
      last = smp;
      smpL = smpR = smp;
      if (itd != NULL) {
        // Interaural delay: the far ear hears the voice up to 32 samples
        // (1 ms) late. itd[] holds the last 32 samples, oldest at itdPos.
        smpL = pb->itd.shiftL == 0 ? smp : itd[(*itdPos - pb->itd.shiftL) & 31];
        smpR = pb->itd.shiftR == 0 ? smp : itd[(*itdPos - pb->itd.shiftR) & 31];
        itd[*itdPos & 31] = (s16)clamp16(smp);
        *itdPos = (*itdPos + 1) & 31;
      }
      bus->main[BUS_L + n] += (smpL * (s32)(s16)pb->mix.vL) >> 15;
      bus->main[BUS_R + n] += (smpR * (s32)(s16)pb->mix.vR) >> 15;
      bus->main[BUS_S + n] += (smp * (s32)(s16)pb->mix.vS) >> 15;
      bus->auxA[BUS_L + n] += (smpL * (s32)(s16)pb->mix.vAuxAL) >> 15;
      bus->auxA[BUS_R + n] += (smpR * (s32)(s16)pb->mix.vAuxAR) >> 15;
      bus->auxA[BUS_S + n] += (smp * (s32)(s16)pb->mix.vAuxAS) >> 15;
      bus->auxB[BUS_L + n] += (smpL * (s32)(s16)pb->mix.vAuxBL) >> 15;
      bus->auxB[BUS_R + n] += (smpR * (s32)(s16)pb->mix.vAuxBR) >> 15;
      bus->auxB[BUS_S + n] += (smp * (s32)(s16)pb->mix.vAuxBS) >> 15;

      env += (s16)pb->ve.currentDelta;
      pb->ve.currentVolume = (u16)(env < 0 ? 0 : (env > 0x7FFF ? 0x7FFF : env));
      pb->mix.vL += pb->mix.vDeltaL;
      pb->mix.vR += pb->mix.vDeltaR;
      pb->mix.vS += pb->mix.vDeltaS;
      pb->mix.vAuxAL += pb->mix.vDeltaAuxAL;
      pb->mix.vAuxAR += pb->mix.vDeltaAuxAR;
      pb->mix.vAuxAS += pb->mix.vDeltaAuxAS;
      pb->mix.vAuxBL += pb->mix.vDeltaAuxBL;
      pb->mix.vAuxBR += pb->mix.vDeltaAuxBR;
      pb->mix.vAuxBS += pb->mix.vDeltaAuxBS;

      frac += ratio;
      steps = frac >> 16;
      pb->src.currentAddressFrac = (u16)frac;
      while (steps--) {
        pb->src.last_samples[0] = pb->src.last_samples[1];
        pb->src.last_samples[1] = pb->src.last_samples[2];
        pb->src.last_samples[2] = pb->src.last_samples[3];
        pb->src.last_samples[3] = (u16)fetchSample(pb, aram, aramSize);
      }
    }
  }

  // What the voice last contributed to each bus. When MusyX stops a voice
  // (HandleDepopVoice) it adds these to the studio's depop sums, which fade
  // out over the following frames instead of the voice cutting to zero.
  pb->dpop.aL = (u16)clamp16((last * (s32)(s16)pb->mix.vL) >> 15);
  pb->dpop.aR = (u16)clamp16((last * (s32)(s16)pb->mix.vR) >> 15);
  pb->dpop.aS = (u16)clamp16((last * (s32)(s16)pb->mix.vS) >> 15);
  pb->dpop.aAuxAL = (u16)clamp16((last * (s32)(s16)pb->mix.vAuxAL) >> 15);
  pb->dpop.aAuxAR = (u16)clamp16((last * (s32)(s16)pb->mix.vAuxAR) >> 15);
  pb->dpop.aAuxAS = (u16)clamp16((last * (s32)(s16)pb->mix.vAuxAS) >> 15);
  pb->dpop.aAuxBL = (u16)clamp16((last * (s32)(s16)pb->mix.vAuxBL) >> 15);
  pb->dpop.aAuxBR = (u16)clamp16((last * (s32)(s16)pb->mix.vAuxBR) >> 15);
  pb->dpop.aAuxBS = (u16)clamp16((last * (s32)(s16)pb->mix.vAuxBS) >> 15);
}

// Add a fading depop value: the studio parameter block holds, per bus, a
// native s32 start (written over the Hi/Lo pair by DoDepopFade) and an s16
// per-sample delta.
static void addDepop(s32* dst, const u16* triple) {
  s32 v;
  s16 delta;
  int n;
  memcpy(&v, triple, sizeof(v));
  delta = (s16)triple[2];
  if (v == 0 && delta == 0) {
    return;
  }
  for (n = 0; n < FRAME_SAMPLES; ++n, v += delta) {
    dst[n] += v;
  }
}

static void addScaled(s32* dst, const s32* src, u16 vol) {
  int n;
  if (vol == 0) {
    return;
  }
  for (n = 0; n < FRAME_SAMPLES * 3; ++n) {
    dst[n] += (s32)(((s64)src[n] * vol) >> 15);
  }
}

static void addBus(s32* dst, const s32* src) {
  int n;
  for (n = 0; n < FRAME_SAMPLES * 3; ++n) {
    dst[n] += src[n];
  }
}

// One 5 ms frame, following the command list salBuildCommandList built for
// the DSP: per active studio, clear its main bus for this frame (main[salFrame])
// and its aux send buffers (auxA/auxB[salAuxFrame]); mix in other studios'
// previous frame (studio inputs); mix its voices; add the aux returns that
// the CPU effect callbacks processed (auxA/auxB[(salAuxFrame + 1) % 3]: the
// callbacks in salHandleAuxProcessing run after this on buffer
// (salAuxFrame + 2) % 3, so a send comes back two frames later, as on the
// hardware); add the depop fades. Then the master studios are summed to the
// output, the surround channel matrixed into left and right.
// A 90-degree phase-difference pair for the Pro Logic II encode: two chains of
// four second-order all-pass sections (Olli Niemitalo's public-domain
// coefficients). With path A delayed by one sample, B leads it by 90 +/- 1
// degrees from 50 Hz to 14 kHz at 32 kHz, both at unity gain.
static const float kHilbertA[4] = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
static const float kHilbertB[4] = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};

typedef struct HilbertPath {
  float x1[4], x2[4], y1[4], y2[4];
} HilbertPath;

static float hilbertStep(HilbertPath* h, const float* coef, float x) {
  int k;
  for (k = 0; k < 4; ++k) {
    float a2 = coef[k] * coef[k];
    float y = a2 * (x + h->y2[k]) - h->x2[k];
    if (y > -1e-10f && y < 1e-10f) {
      y = 0.0f; // decaying into denormals in silence is slow on x86
    }
    h->x2[k] = h->x1[k];
    h->x1[k] = x;
    h->y2[k] = h->y1[k];
    h->y1[k] = y;
    x = y;
  }
  return x;
}

static void hostMixFrame(void) {
  static s32 out[FRAME_SAMPLES * 3];
  static s32 rear[FRAME_SAMPLES * 2];
  int anyRear = 0;
  const u8* aram = (const u8*)ARGetStorageAddress();
  u32 aramSize = ARGetSize();
  u8 aIn = (u8)((salAuxFrame + 1) % 3);
  int st, v, n;

  for (v = 0; v < salNumVoices; ++v) {
    if (dspVoice[v].state == 0) {
      if (sVoiceSeen[v]) {
        DSPvoice* dv = &dspVoice[v];
        fprintf(stderr,
                "openstrikers: [%lu] voice %d end: smp %u after %lu ticks, len %u loopLen %u posHi %u pitch 0x%x\n",
                salTicks, v, dv->smp_id, salTicks - sVoiceStart[v], dv->smp_info.length, dv->smp_info.loopLength,
                dv->playInfo.posHi, dv->playInfo.pitch);
      }
      sVoiceSeen[v] = 0;
    }
  }
  memset(out, 0, sizeof(out));
  memset(rear, 0, sizeof(rear));
  for (st = 0; st < salMaxStudioNum; ++st) {
    DSPstudioinfo* stp = &dspStudio[st];
    HostVoiceBuses bus;
    DSPvoice* dv;
    const u16* spb;
    u32 in;

    if (stp->state != 1) {
      continue;
    }
    {
      static SND_AUX_CALLBACK sLastA[8], sLastB[8];
      static int sLastType[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
      if (st < 8 && (stp->auxAHandler != sLastA[st] || stp->auxBHandler != sLastB[st] ||
                     (int)stp->type != sLastType[st])) {
        sLastA[st] = stp->auxAHandler;
        sLastB[st] = stp->auxBHandler;
        sLastType[st] = (int)stp->type;
        fprintf(stderr, "openstrikers: musyx: studio %d (%s) effects: aux A %s, aux B %s\n", st,
                stp->type == SND_STUDIO_TYPE_DPL2 ? "Pro Logic II" : "standard",
                stp->auxAHandler != NULL ? "on" : "off", stp->auxBHandler != NULL ? "on" : "off");
      }
    }
    bus.main = stp->main[salFrame];
    bus.auxA = stp->auxA[salAuxFrame];
    bus.auxB = stp->auxB[salAuxFrame];
    memset(bus.main, 0, FRAME_SAMPLES * 3 * sizeof(s32));
    memset(bus.auxA, 0, FRAME_SAMPLES * 3 * sizeof(s32));
    memset(bus.auxB, 0, FRAME_SAMPLES * 3 * sizeof(s32));

    for (in = 0; in < stp->numInputs; ++in) {
      const s32* src = dspStudio[stp->in[in].studio].main[salFrame ^ 1];
      addScaled(bus.main, src, stp->in[in].vol);
      addScaled(bus.auxA, src, stp->in[in].volA);
      addScaled(bus.auxB, src, stp->in[in].volB);
    }

    if (aram != NULL) {
      for (dv = stp->voiceRoot; dv != NULL; dv = dv->next) {
        if (dv->state != 0 && dv->pb != NULL && dv->patchData != NULL) {
          mixVoice(dv, &bus, aram, aramSize);
        }
      }
    }

    if (stp->auxAHandler != NULL) {
      addBus(bus.main, stp->auxA[aIn]);
    }
    if (stp->type == SND_STUDIO_TYPE_STD && stp->auxBHandler != NULL) {
      addBus(bus.main, stp->auxB[aIn]);
    }

    spb = (const u16*)stp->spb;
    addDepop(bus.main + BUS_L, spb + 0);
    addDepop(bus.main + BUS_R, spb + 3);
    addDepop(bus.main + BUS_S, spb + 6);
    addDepop(bus.auxA + BUS_L, spb + 9);
    addDepop(bus.auxA + BUS_R, spb + 12);
    addDepop(bus.auxA + BUS_S, spb + 15);
    addDepop(bus.auxB + BUS_L, spb + 18);
    addDepop(bus.auxB + BUS_R, spb + 21);
    addDepop(bus.auxB + BUS_S, spb + 24);

    if (stp->isMaster) {
      addBus(out, bus.main);
      if (stp->type == SND_STUDIO_TYPE_DPL2) {
        // In a Dolby Pro Logic II studio aux B is not an effect bus: its
        // left/right are the rear speakers (salCalcVolume, CalcBusDPL2).
        for (n = 0; n < FRAME_SAMPLES; ++n) {
          rear[BUS_L + n] += bus.auxB[BUS_L + n];
          rear[BUS_R + n] += bus.auxB[BUS_R + n];
        }
        anyRear = 1;
      }
    }
  }
  if (anyRear) {
    // Pro Logic II encode to a stereo pair (Lt/Rt): each rear goes to both
    // sides, mostly to its own and with opposite signs, so a decoder steers it
    // behind. The rears are also shifted 90 degrees against the fronts (two
    // all-pass chains, see hilbertStep); without that, a sound sent to front
    // and rear at once -- anything panned mid-field -- cancels on the left and
    // doubles on the right. The surround channel (aux A's, from the reverb)
    // is a centre rear.
    static HilbertPath sFrontL, sFrontR, sRearL, sRearR;
    static float sDelayL, sDelayR;
    for (n = 0; n < FRAME_SAMPLES; ++n) {
      float s = 0.7071f * (float)out[BUS_S + n];
      float rl = (float)rear[BUS_L + n] + s, rr = (float)rear[BUS_R + n] + s;
      float fl = hilbertStep(&sFrontL, kHilbertA, (float)out[BUS_L + n]);
      float fr = hilbertStep(&sFrontR, kHilbertA, (float)out[BUS_R + n]);
      float bl = hilbertStep(&sRearL, kHilbertB, -0.8718f * rl - 0.4899f * rr);
      float br = hilbertStep(&sRearR, kHilbertB, 0.4899f * rl + 0.8718f * rr);
      // Path A runs one sample late, which is what puts the two paths 90
      // degrees apart.
      out[BUS_L + n] = (s32)(sDelayL + bl);
      out[BUS_R + n] = (s32)(sDelayR + br);
      out[BUS_S + n] = 0;
      sDelayL = fl;
      sDelayR = fr;
    }
  }
  for (n = 0; n < FRAME_SAMPLES; ++n) {
    // Dolby Surround matrix: surround goes to both sides, out of phase, at
    // -3 dB. In stereo output mode MusyX leaves it empty.
    s32 s = (out[BUS_S + n] * 23170) >> 15;
    sMixOut[n * 2] = clamp16(out[BUS_L + n] - s);
    sMixOut[n * 2 + 1] = clamp16(out[BUS_R + n] + s);
  }
  {
    // OPENSTRIKERS_AUDIO_TRACE=3: what is playing when the output saturates.
    static int sClipTrace = -1;
    static unsigned long sLastClip;
    s32 peak = 0;
    if (sClipTrace < 0) {
      const char* t = getenv("OPENSTRIKERS_AUDIO_TRACE");
      sClipTrace = t != NULL && t[0] >= '3';
    }
    for (n = 0; sClipTrace && n < FRAME_SAMPLES * 3; ++n) {
      s32 a = out[n] < 0 ? -out[n] : out[n];
      if (a > peak) peak = a;
    }
    if (sClipTrace && peak > 32767 && salTicks - sLastClip >= 200) {
      sLastClip = salTicks;
      fprintf(stderr, "openstrikers: [%lu] output clips: bus peak %d\n", salTicks, peak);
      for (v = 0; v < salNumVoices; ++v) {
        DSPvoice* dv = &dspVoice[v];
        _PB* pb = dv->pb;
        if (dv->state == 0 || pb == NULL || pb->state != 1) continue;
        fprintf(stderr, "openstrikers:   voice %d smp %u comp %u env %d vol %d/%d/%d auxA %d/%d last %d,%d\n", v, dv->smp_id,
                dv->smp_info.compType, (s16)pb->ve.currentVolume, (s16)pb->mix.vL, (s16)pb->mix.vR, (s16)pb->mix.vS,
                (s16)pb->mix.vAuxAL, (s16)pb->mix.vAuxAR, (s16)pb->src.last_samples[2], (s16)pb->src.last_samples[3]);
      }
    }
  }
}

void salCtrlDsp(s16* dest) {
  // Keep MusyX's per-frame DSP bookkeeping exactly as on hardware; only the
  // execution of the list is replaced.
  salBuildCommandList(dest, salGetStartDelay());
  hostMixFrame();
}

// Single-threaded: MusyX only ever runs inside musyxHostPump on the game
// thread, so there is nothing to mask.
void hwInitIrq() {}
void hwExitIrq() {}
void hwEnableIrq() {}
void hwDisableIrq() {}
void hwIRQEnterCritical() {}
void hwIRQLeaveCritical() {}

// Run `ticks` 5 ms audio frames, appending each frame's 160 stereo s16
// samples to `out` (room for ticks * 320 values) when it is not NULL.
// Returns how many ran.
// On the GameCube, the THP movie player chains itself onto the audio
// interface's DMA callback: after MusyX has mixed a buffer it adds the movie's
// sound and points the DMA at the result. Here it registers a function that
// gets each finished 5 ms frame (160 stereo samples, left first) to mix into.
static void (*sPostMix)(s16* frame, u32 samples);

void musyxHostSetPostMix(void (*fn)(s16* frame, u32 samples)) { sPostMix = fn; }

int musyxHostPump(int ticks, s16* out) {
  int n = 0;
  if (!salAiRunning || userCallback == NULL) {
    return 0;
  }
  for (; n < ticks; ++n) {
    salAIBufferIndex = (salAIBufferIndex + 1) % 4;
    userCallback();
    ++salTicks;
    if (sPostMix != NULL) {
      sPostMix(sMixOut, FRAME_SAMPLES);
    }
    if (out != NULL) {
      memcpy(out + n * FRAME_SAMPLES * 2, sMixOut, sizeof(sMixOut));
    }
    {
      int v, active = 0;
      for (v = 0; v < salNumVoices; ++v) {
        active += dspVoice[v].state != 0;
      }
      if (active > salPeakVoices) {
        salPeakVoices = active;
      }
    }
  }
  return n;
}

unsigned long musyxHostTicks() { return salTicks; }

// Most voices playing at once since the last call.
int musyxHostTakePeakVoices() {
  int peak = salPeakVoices;
  salPeakVoices = 0;
  return peak;
}

// ---------------------------------------------------------------------------
// Sound data off the disc.
//
// MusyX reads its project, pool and sample directory in place, as native
// structs. On the GameCube that was big-endian, and the sample directory's
// entries hold a 4-byte pointer. PlatAudio's SetupSoundBuffers hands each
// file here once, after loading it and before MusyX sees it: the project and
// pool are byte-swapped in place, the sample directory is rebuilt with host
// entries. Formats are those in synthdata.h / s_data.c.

#include "musyx/synthdata.h"
#include "musyx/stream.h"

static u16 be16(const void* p) {
  const u8* b = (const u8*)p;
  return (u16)((b[0] << 8) | b[1]);
}
static u32 be32(const void* p) {
  const u8* b = (const u8*)p;
  return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static void swap16(void* p) {
  u16 v = be16(p);
  memcpy(p, &v, 2);
}
static void swap32(void* p) {
  u32 v = be32(p);
  memcpy(p, &v, 4);
}

// Swap a 0xFFFF-terminated u16 ID list (ranges are two entries, both u16).
static void swapIdList(u8* base, u32 len, u32 off) {
  u8* p;
  if (off >= len) {
    return;
  }
  for (p = base + off; p + 2 <= base + len; p += 2) {
    u16 v = be16(p);
    swap16(p);
    if (v == 0xFFFF) {
      break;
    }
  }
}

// Offsets already swapped, so ID lists or tables shared between groups are
// not swapped back.
#define MAX_SEEN 4096
static u32 sSeen[MAX_SEEN];
static int sSeenNum;
static int seenOnce(u32 off) {
  int i;
  for (i = 0; i < sSeenNum; ++i) {
    if (sSeen[i] == off) {
      return 0;
    }
  }
  if (sSeenNum < MAX_SEEN) {
    sSeen[sSeenNum++] = off;
  }
  return 1;
}

void musyxHostSwapProject(void* data, u32 len) {
  u8* base = (u8*)data;
  u32 off = 0;
  int groups = 0;
  sSeenNum = 0;
  while (off + sizeof(GROUP_DATA) <= len) {
    GROUP_DATA* g = (GROUP_DATA*)(base + off);
    u32 next = be32(&g->nextOff);
    u32 i;
    swap32(&g->nextOff);
    if (next == 0xFFFFFFFF) {
      break;
    }
    swap16(&g->id);
    swap16(&g->type);
    swap32(&g->macroOff);
    swap32(&g->sampleOff);
    swap32(&g->curveOff);
    swap32(&g->keymapOff);
    swap32(&g->layerOff);
    swap32(&g->data.song.normpageOff);
    swap32(&g->data.song.drumpageOff);
    swap32(&g->data.song.midiSetupOff);
    {
      u32 lists[5] = {g->macroOff, g->sampleOff, g->curveOff, g->keymapOff, g->layerOff};
      for (i = 0; i < 5; ++i) {
        if (seenOnce(lists[i])) {
          swapIdList(base, len, lists[i]);
        }
      }
    }
    if (g->type == 1 && g->data.fx.tableOff + 4 <= len && seenOnce(g->data.fx.tableOff)) {
      FX_DATA* fd = (FX_DATA*)(base + g->data.fx.tableOff);
      u16 n;
      swap16(&fd->num);
      swap16(&fd->reserverd);
      n = fd->num;
      for (i = 0; i < n && (u8*)&fd->fx[i + 1] <= base + len; ++i) {
        swap16(&fd->fx[i].id);
        swap16(&fd->fx[i].macro);
      }
    } else if (g->type != 1) {
      // Song groups: the game streams its music and never starts a sequence,
      // so their pages and MIDI setups are left as they are.
      fprintf(stderr, "openstrikers: musyx: group %u is type %u (song); its pages are not swapped\n", g->id,
              g->type);
    }
    ++groups;
    if (next <= off) {
      break;
    }
    off = next;
  }
  fprintf(stderr, "openstrikers: musyx: project swapped (%d groups, %u bytes)\n", groups, len);
}

enum { POOL_MACRO, POOL_CURVE, POOL_KEYMAP, POOL_LAYER };

static int swapPoolSection(u8* base, u32 len, u32 off, int kind) {
  int count = 0;
  u32 start = off;
  if (off == 0) {
    // An empty section has offset 0, which is the pool header itself; walking
    // it would swap the header a second time.
    return 0;
  }
  while (off + 8 <= len) {
    MEM_DATA* m = (MEM_DATA*)(base + off);
    u32 next = be32(&m->nextOff);
    if (getenv("OPENSTRIKERS_AUDIO_TRACE") && count < 3) {
      fprintf(stderr, "openstrikers: musyx: pool kind %d entry %d at 0x%x: next 0x%x id 0x%x\n", kind, count, off,
              next, be16(&m->id));
    }
    u8* body = (u8*)&m->data;
    u32 size;
    u32 i;
    swap32(&m->nextOff);
    if (next == 0xFFFFFFFF) {
      break;
    }
    swap16(&m->id);
    swap16(&m->reserved);
    size = next > 8 ? next - 8 : 0;
    if (off + 8 + size > len) {
      size = len - off - 8;
    }
    switch (kind) {
    case POOL_MACRO: // MSTEP: pairs of u32 command words
      for (i = 0; i + 4 <= size; i += 4) {
        swap32(body + i);
      }
      break;
    case POOL_KEYMAP: // KEYMAP[128]
      for (i = 0; i + sizeof(KEYMAP) <= size; i += sizeof(KEYMAP)) {
        KEYMAP* k = (KEYMAP*)(body + i);
        swap16(&k->id);
        swap16(&k->prioOffset);
      }
      break;
    case POOL_LAYER: { // u32 num, LAYER[num]
      u32 n = be32(body);
      swap32(body);
      for (i = 0; i < n && 4 + (i + 1) * sizeof(LAYER) <= size; ++i) {
        LAYER* l = (LAYER*)(body + 4 + i * sizeof(LAYER));
        swap16(&l->id);
        swap16(&l->prioOffset);
      }
    } break;
    default: // curves are byte tables
      break;
    }
    ++count;
    if (next == 0) {
      break;
    }
    off += next;
  }
  if (getenv("OPENSTRIKERS_AUDIO_TRACE")) {
    fprintf(stderr, "openstrikers: musyx: pool kind %d: 0x%x..0x%x of 0x%x\n", kind, start, off, len);
  }
  return count;
}

void musyxHostSwapPool(void* data, u32 len) {
  u8* base = (u8*)data;
  POOL_DATA* pool = (POOL_DATA*)base;
  int n[4];
  if (len < sizeof(POOL_DATA)) {
    return;
  }
  swap32(&pool->macroOff);
  swap32(&pool->curveOff);
  swap32(&pool->keymapOff);
  swap32(&pool->layerOff);
  n[0] = swapPoolSection(base, len, pool->macroOff, POOL_MACRO);
  n[1] = swapPoolSection(base, len, pool->curveOff, POOL_CURVE);
  n[2] = swapPoolSection(base, len, pool->keymapOff, POOL_KEYMAP);
  n[3] = swapPoolSection(base, len, pool->layerOff, POOL_LAYER);
  fprintf(stderr, "openstrikers: musyx: pool swapped (%d macros, %d curves, %d keymaps, %d layers)\n", n[0], n[1],
          n[2], n[3]);
}

// The disc's sample directory entry: SDIR_DATA with a 4-byte addr.
#define GC_SDIR_SIZE 0x20

static void swapAdpcmInfo(u8* p, u32 room, u8 compType, u32 length) {
  // SNDADPCMinfo / DSPADPCMplusInfo: u16 numCoef, u8 initialPS, u8 loopPS,
  // s16 loopY0, s16 loopY1, s16 coefTab[8][2]; the "plus" form adds one
  // {s16 Y0, s16 Y1, u8 PS, u8 pad} per 14-sample ADPCM frame.
  u32 i;
  swap16(p + 0);
  for (i = 4; i < 0x28; i += 2) {
    swap16(p + i);
  }
  if (compType == 1) {
    u32 blocks = (length + 13) / 14;
    for (i = 0; i < blocks && 0x28 + (i + 1) * 6 <= room; ++i) {
      swap16(p + 0x28 + i * 6);
      swap16(p + 0x28 + i * 6 + 2);
    }
  }
}

void* musyxHostConvertSdir(void* data, u32 len, void* (*alloc)(u32 size)) {
  u8* old = (u8*)data;
  u32 n = 0;
  u32 i;
  u32 entriesSize;
  SDIR_DATA* out;
  u8* tail;

  while ((n + 1) * GC_SDIR_SIZE <= len && be16(old + n * GC_SDIR_SIZE) != 0xFFFF) {
    ++n;
  }
  entriesSize = (n + 1) * (u32)sizeof(SDIR_DATA);
  out = (SDIR_DATA*)alloc(entriesSize + len);
  if (out == NULL) {
    return NULL;
  }
  // The ADPCM parameter blocks stay where they were, after the new entries;
  // extraData is an offset from the start of the entry array, so it moves by
  // the growth of the array.
  tail = (u8*)out + entriesSize;
  memcpy(tail, old, len);
  sSeenNum = 0;
  for (i = 0; i <= n; ++i) {
    const u8* e = old + i * GC_SDIR_SIZE;
    SDIR_DATA* s = &out[i];
    memset(s, 0, sizeof(*s));
    s->id = be16(e + 0x00);
    if (i == n) {
      s->id = 0xFFFF;
      break;
    }
    s->ref_cnt = be16(e + 0x02);
    s->offset = be32(e + 0x04);
    s->addr = (void*)(uintptr_t)be32(e + 0x08);
    s->header.info = be32(e + 0x0C);
    s->header.length = be32(e + 0x10);
    s->header.loopOffset = be32(e + 0x14);
    s->header.loopLength = be32(e + 0x18);
    s->extraData = be32(e + 0x1C);
    if (s->extraData != 0) {
      u32 x = s->extraData;
      if (x + 0x28 <= len && seenOnce(x)) {
        swapAdpcmInfo(tail + x, len - x, (u8)(s->header.length >> 24), s->header.length & 0xFFFFFF);
      }
      s->extraData = entriesSize + x;
    }
  }
  fprintf(stderr, "openstrikers: musyx: sample directory converted (%u samples)\n", n);
  return out;
}
