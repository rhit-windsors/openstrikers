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
// MusyX describes -- is done by the host mixer in salCtrlDsp (stage 2 of
// docs/PLAN-audio.md); until then each tick produces silence.

#include "musyx/musyx.h"
#include "musyx/hardware.h"
#include "musyx/sal.h"
#include "musyx/dspvoice.h"

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

bool salInitDsp(u32 flags) {
  (void)flags;
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
// Not yet: aux A/B (effects), surround, ITD, low-pass, depop.

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

static u8 sVoiceSeen[256];

static void mixVoice(DSPvoice* dv, s32* accL, s32* accR, const u8* aram, u32 aramSize) {
  _PB* pb = dv->pb;
  u16* fields = (u16*)pb;
  const u16* patch = (const u16*)dv->patchData;
  int sub, i;
  static int sTrace = -1;

  if (sTrace < 0) {
    const char* t = getenv("OPENSTRIKERS_AUDIO_TRACE");
    sTrace = t != NULL && t[0] >= '2';
  }
  if (sTrace && !sVoiceSeen[dv - dspVoice]) {
    sVoiceSeen[dv - dspVoice] = 1;
    fprintf(stderr,
            "openstrikers: voice %d start: smp %u comp %u fmt 0x%x addr 0x%x loop 0x%x end 0x%x loopType %u "
            "ratio 0x%x src %u ps 0x%x coef %d,%d vol %d/%d\n",
            (int)(dv - dspVoice), dv->smp_id, dv->smp_info.compType, pb->addr.format,
            pbAddr(pb->addr.currentAddressHi, pb->addr.currentAddressLo),
            pbAddr(pb->addr.loopAddressHi, pb->addr.loopAddressLo),
            pbAddr(pb->addr.endAddressHi, pb->addr.endAddressLo), pb->loopType,
            pbAddr(pb->src.ratioHi, pb->src.ratioLo), pb->srcSelect, pb->adpcm.pred_scale,
            (s16)pb->adpcm.a[0][0], (s16)pb->adpcm.a[0][1], (s16)pb->mix.vL, (s16)pb->mix.vR);
  }

  for (sub = 0; sub < SUBFRAMES; ++sub) {
    u32 ratio;
    for (i = 0; i < pb->update.updNum[sub]; ++i, patch += 2) {
      if (patch[0] < sizeof(_PB) / 2) {
        fields[patch[0]] = patch[1];
      }
    }
    if (pb->state != 1) {
      continue;
    }
    ratio = pb->srcSelect == 2 ? 0x10000 : pbAddr(pb->src.ratioHi, pb->src.ratioLo);
    for (i = 0; i < SUBFRAME_SAMPLES; ++i) {
      int n = sub * SUBFRAME_SAMPLES + i;
      s32 s0 = (s16)pb->src.last_samples[2];
      s32 s1 = (s16)pb->src.last_samples[3];
      u32 frac = pb->src.currentAddressFrac;
      s32 smp = pb->srcSelect == 2 ? s1 : s0 + (((s1 - s0) * (s32)frac) >> 16);
      s32 env = pb->ve.currentVolume;
      u32 steps;

      smp = (smp * env) >> 15;
      accL[n] += (smp * (s32)(s16)pb->mix.vL) >> 15;
      accR[n] += (smp * (s32)(s16)pb->mix.vR) >> 15;

      env += (s16)pb->ve.currentDelta;
      pb->ve.currentVolume = (u16)(env < 0 ? 0 : (env > 0x7FFF ? 0x7FFF : env));
      pb->mix.vL += pb->mix.vDeltaL;
      pb->mix.vR += pb->mix.vDeltaR;

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
}

static void hostMixFrame(void) {
  static s32 accL[FRAME_SAMPLES], accR[FRAME_SAMPLES];
  const u8* aram = (const u8*)ARGetStorageAddress();
  u32 aramSize = ARGetSize();
  int v, n;

  memset(accL, 0, sizeof(accL));
  memset(accR, 0, sizeof(accR));
  if (aram != NULL) {
    for (v = 0; v < salNumVoices; ++v) {
      DSPvoice* dv = &dspVoice[v];
      if (dv->state != 0 && dv->pb != NULL && dv->patchData != NULL) {
        mixVoice(dv, accL, accR, aram, aramSize);
      } else {
        sVoiceSeen[v] = 0;
      }
    }
  }
  for (n = 0; n < FRAME_SAMPLES; ++n) {
    sMixOut[n * 2] = clamp16(accL[n]);
    sMixOut[n * 2 + 1] = clamp16(accR[n]);
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
int musyxHostPump(int ticks, s16* out) {
  int n = 0;
  if (!salAiRunning || userCallback == NULL) {
    return 0;
  }
  for (; n < ticks; ++n) {
    salAIBufferIndex = (salAIBufferIndex + 1) % 4;
    userCallback();
    ++salTicks;
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
