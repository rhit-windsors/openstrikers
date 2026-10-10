// Host versions of the Dolphin SDK's THP decoders (movies: the intro, the NLG
// logo and the Strikers 101 lessons).
//
// The SDK's THPVideoDecode (extern/decomp/src/Dolphin/thp/THPDec.c) is mostly
// PowerPC paired-single assembly running out of the locked cache, so the video
// decoder here is written in C from the same format: each frame is a baseline
// JPEG, 4:2:0, written straight into three GX I8 textures (Y at full size, U
// and V at half) in the GX tile layout of 8x4 texels. THP scans differ from
// JPEG in two ways the SDK decoder relies on: there is no 0xFF00 byte
// stuffing, and a restart interval only realigns the bit reader to a byte
// (there are no RSTn markers in the data).
//
// THPAudioDecode follows THPAudio.c, reading the record header big-endian in
// place. One difference: the SDK writes right then left, the order of the
// GameCube's audio interface; the host mixer (musyx_host.c) and its output
// are left then right, so left comes first here.

#include <dolphin/types.h>

#include <math.h>
#include <string.h>

static u16 rd16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// ---------------------------------------------------------------------------
// Video

typedef struct HuffTable {
  u16 maxCode[18]; // largest code of each length, 0xFFFF if none
  s32 valPtr[17];  // index into vals of the first code of each length, minus that code
  u8 vals[256];
  u8 lookLen[512]; // 9-bit fast lookup: code length (0 = longer code)
  u8 lookVal[512];
  int valid;
} HuffTable;

typedef struct ThpDecoder {
  const u8* p;
  const u8* end;
  u32 bits; // left-aligned bit buffer
  int numBits;
  u16 width, height;
  u16 restartInterval;
  u8 quantSel[3];
  u8 dcSel[3], acSel[3];
  s32 predDC[3];
  u16 quant[4][64]; // natural order
  HuffTable huff[2][4]; // [class: 0 DC, 1 AC][id]
} ThpDecoder;

static const u8 kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static void buildHuff(HuffTable* h, const u8* counts, const u8* vals, int numVals) {
  int len, i, k = 0;
  u32 code = 0;

  memset(h, 0, sizeof(*h));
  memcpy(h->vals, vals, numVals);
  for (len = 1; len <= 16; ++len) {
    int n = counts[len - 1];
    if (n == 0) {
      h->maxCode[len] = 0xFFFF;
      h->valPtr[len] = 0;
    } else {
      h->valPtr[len] = k - (s32)code;
      for (i = 0; i < n; ++i, ++k, ++code) {
        if (len <= 9) {
          // Every 9-bit prefix that starts with this code decodes to it.
          int shift = 9 - len, fill;
          for (fill = 0; fill < (1 << shift); ++fill) {
            h->lookLen[(code << shift) | fill] = (u8)len;
            h->lookVal[(code << shift) | fill] = vals[k];
          }
        }
      }
      h->maxCode[len] = (u16)(code - 1);
    }
    code <<= 1;
  }
  h->maxCode[17] = 0xFFFF;
  h->valid = 1;
}

static void fillBits(ThpDecoder* d) {
  while (d->numBits <= 24) {
    u32 b = d->p < d->end ? *d->p++ : 0;
    d->bits |= b << (24 - d->numBits);
    d->numBits += 8;
  }
}

static u32 getBits(ThpDecoder* d, int n) {
  u32 v;
  if (n == 0) {
    return 0;
  }
  fillBits(d);
  v = d->bits >> (32 - n);
  d->bits <<= n;
  d->numBits -= n;
  return v;
}

static int decodeHuff(ThpDecoder* d, const HuffTable* h) {
  u32 look;
  int len;
  u32 code;

  fillBits(d);
  look = d->bits >> (32 - 9);
  if (h->lookLen[look] != 0) {
    len = h->lookLen[look];
    d->bits <<= len;
    d->numBits -= len;
    return h->lookVal[look];
  }
  code = d->bits >> (32 - 10);
  for (len = 10; len <= 16; ++len) {
    if (h->maxCode[len] != 0xFFFF && code <= h->maxCode[len]) {
      d->bits <<= len;
      d->numBits -= len;
      return h->vals[(h->valPtr[len] + (s32)code) & 0xFF];
    }
    code = d->bits >> (32 - (len + 1));
  }
  // Corrupt data: consume what we looked at and give zero.
  d->bits <<= 16;
  d->numBits -= 16;
  return 0;
}

static s32 extend(u32 v, int n) { return n == 0 ? 0 : (v < (1u << (n - 1)) ? (s32)v - (1 << n) + 1 : (s32)v); }

// 8x8 inverse DCT, separable, with a precomputed cosine table.
static float sIdctCos[8][8]; // [x][u] = C(u)/2 * cos((2x+1)u*pi/16)
static int sIdctReady;

static void initIdct(void) {
  const double pi = 3.14159265358979323846;
  int x, u;
  for (x = 0; x < 8; ++x) {
    for (u = 0; u < 8; ++u) {
      double c = u == 0 ? sqrt(0.5) : 1.0;
      sIdctCos[x][u] = (float)(c / 2.0 * cos((2 * x + 1) * u * pi / 16.0));
    }
  }
  sIdctReady = 1;
}

static int decodeBlock(ThpDecoder* d, int comp, u8 out[64]) {
  const HuffTable* dc = &d->huff[0][d->dcSel[comp]];
  const HuffTable* ac = &d->huff[1][d->acSel[comp]];
  const u16* q = d->quant[d->quantSel[comp]];
  float coef[64], tmp[64];
  int k, s, x, y, u, lastNonZero = 0;

  memset(coef, 0, sizeof(coef));
  s = decodeHuff(d, dc);
  d->predDC[comp] += extend(getBits(d, s & 15), s & 15);
  coef[0] = (float)(d->predDC[comp] * q[0]);

  for (k = 1; k < 64;) {
    int rs = decodeHuff(d, ac);
    int r = rs >> 4;
    s = rs & 15;
    if (s == 0) {
      if (r != 15) {
        break; // end of block
      }
      k += 16;
      continue;
    }
    k += r;
    if (k > 63) {
      break;
    }
    coef[kZigzag[k]] = (float)(extend(getBits(d, s), s) * q[kZigzag[k]]);
    lastNonZero = k;
    ++k;
  }

  if (lastNonZero == 0) {
    // DC only: a flat block.
    int v = (int)lrintf(coef[0] / 8.0f) + 128;
    memset(out, v < 0 ? 0 : (v > 255 ? 255 : v), 64);
    return 0;
  }

  // Columns, then rows.
  for (u = 0; u < 8; ++u) {
    for (y = 0; y < 8; ++y) {
      float sum = 0.0f;
      for (k = 0; k < 8; ++k) {
        sum += sIdctCos[y][k] * coef[k * 8 + u];
      }
      tmp[y * 8 + u] = sum;
    }
  }
  for (y = 0; y < 8; ++y) {
    for (x = 0; x < 8; ++x) {
      float sum = 0.0f;
      int v;
      for (k = 0; k < 8; ++k) {
        sum += sIdctCos[x][k] * tmp[y * 8 + k];
      }
      v = (int)lrintf(sum) + 128;
      out[y * 8 + x] = (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
  }
  return 0;
}

// Store an 8x8 block at (bx, by) of a GX I8 texture `width` texels wide:
// tiles of 8x4 texels, 32 bytes each, in rows of width/8 tiles.
static void storeBlockI8(u8* tex, u32 width, u32 bx, u32 by, const u8 blk[64]) {
  u32 tilesPerRow = width / 8;
  int y;
  for (y = 0; y < 8; ++y) {
    u32 py = by + y;
    u8* dst = tex + ((py / 4) * tilesPerRow + bx / 8) * 32 + (py % 4) * 8;
    memcpy(dst, blk + y * 8, 8);
  }
}

s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work) {
  static ThpDecoder dec; // the game decodes one movie at a time, on one thread
  ThpDecoder* d = &dec;
  const u8* p = (const u8*)file;
  u32 mcuX, mcuY, mcusPerRow, mcuRows, mcuCount = 0;
  int haveFrame = 0;
  (void)work;

  if (file == NULL) {
    return 25;
  }
  if (tileY == NULL || tileU == NULL || tileV == NULL) {
    return 27;
  }
  if (!sIdctReady) {
    initIdct();
  }

  memset(d->predDC, 0, sizeof(d->predDC));
  d->restartInterval = 0;

  // Markers up to the start of scan; same set the SDK accepts.
  for (;;) {
    u8 m;
    u16 len;
    if (*p++ != 0xFF) {
      return 3;
    }
    while (*p == 0xFF) {
      ++p;
    }
    m = *p++;
    if (m == 0xD8) {
      continue; // SOI, no length
    }
    len = rd16(p);
    switch (m) {
    case 0xC0: { // SOF0
      int i;
      if (p[2] != 8) {
        return 10;
      }
      d->height = rd16(p + 3);
      d->width = rd16(p + 5);
      if (p[7] != 3) {
        return 12;
      }
      for (i = 0; i < 3; ++i) {
        u8 sampling = p[8 + i * 3 + 1];
        if ((i == 0 && sampling != 0x22) || (i > 0 && sampling != 0x11)) {
          return 19;
        }
        d->quantSel[i] = p[8 + i * 3 + 2] & 3;
      }
      haveFrame = 1;
    } break;
    case 0xC4: { // DHT
      const u8* q = p + 2;
      const u8* qend = p + len;
      while (q < qend) {
        int cls = q[0] >> 4, id = q[0] & 3, n = 0, i;
        for (i = 0; i < 16; ++i) {
          n += q[1 + i];
        }
        if (n > 256) {
          return 3;
        }
        buildHuff(&d->huff[cls & 1][id], q + 1, q + 17, n);
        q += 17 + n;
      }
    } break;
    case 0xDB: { // DQT, 8-bit entries
      const u8* q = p + 2;
      const u8* qend = p + len;
      while (q < qend) {
        int id = q[0] & 3, i;
        for (i = 0; i < 64; ++i) {
          d->quant[id][kZigzag[i]] = q[1 + i];
        }
        q += 65;
      }
    } break;
    case 0xDD: // DRI
      d->restartInterval = rd16(p + 2);
      break;
    case 0xDA: { // SOS
      int i;
      if (p[2] != 3) {
        return 12;
      }
      for (i = 0; i < 3; ++i) {
        d->dcSel[i] = (p[3 + i * 2 + 1] >> 4) & 3;
        d->acSel[i] = p[3 + i * 2 + 1] & 3;
        if (!d->huff[0][d->dcSel[i]].valid || !d->huff[1][d->acSel[i]].valid) {
          return 15;
        }
      }
    } break;
    default:
      if ((m >= 0xE0 && m <= 0xEF) || m == 0xFE) {
        break; // APPn, COM
      }
      return 11;
    }
    p += len;
    if (m == 0xDA) {
      break;
    }
  }
  if (!haveFrame || (d->width & 15) != 0 || (d->height & 15) != 0) {
    return 3;
  }

  // The frame record does not give the scan's length; the bit reader pads
  // with zeros if a corrupt frame runs past it.
  d->p = p;
  d->end = p + (u32)d->width * d->height * 2;
  d->bits = 0;
  d->numBits = 0;

  mcusPerRow = d->width / 16;
  mcuRows = d->height / 16;
  for (mcuY = 0; mcuY < mcuRows; ++mcuY) {
    for (mcuX = 0; mcuX < mcusPerRow; ++mcuX) {
      u8 blk[64];
      u32 x = mcuX * 16, y = mcuY * 16;

      decodeBlock(d, 0, blk);
      storeBlockI8((u8*)tileY, d->width, x, y, blk);
      decodeBlock(d, 0, blk);
      storeBlockI8((u8*)tileY, d->width, x + 8, y, blk);
      decodeBlock(d, 0, blk);
      storeBlockI8((u8*)tileY, d->width, x, y + 8, blk);
      decodeBlock(d, 0, blk);
      storeBlockI8((u8*)tileY, d->width, x + 8, y + 8, blk);
      decodeBlock(d, 1, blk);
      storeBlockI8((u8*)tileU, d->width / 2, x / 2, y / 2, blk);
      decodeBlock(d, 2, blk);
      storeBlockI8((u8*)tileV, d->width / 2, x / 2, y / 2, blk);

      if (d->restartInterval != 0 && ++mcuCount == d->restartInterval) {
        // Restart: drop to the next byte boundary and reset the predictors.
        mcuCount = 0;
        d->bits <<= d->numBits & 7;
        d->numBits &= ~7;
        memset(d->predDC, 0, sizeof(d->predDC));
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Audio: DSP-ADPCM, one or two channels per video frame.
//
// Record header (big-endian): offsetNextChannel u32, sampleSize u32,
// lCoef[8][2] s16, rCoef[8][2] s16, lYn1, lYn2, rYn1, rYn2 s16. 0x50 bytes.

typedef struct ThpAdpcm {
  const u8* data;
  u32 nibble;
  u8 predictor;
  u8 scale;
} ThpAdpcm;

static void adpcmInit(ThpAdpcm* a, const u8* ptr) {
  a->data = ptr;
  a->nibble = 2;
  a->predictor = (u8)((*a->data & 0x70) >> 4);
  a->scale = (u8)(*a->data & 0xF);
  a->data++;
}

static s32 adpcmNext(ThpAdpcm* a) {
  s32 sample;
  if ((a->nibble & 0xF) == 0) {
    a->predictor = (u8)((*a->data & 0x70) >> 4);
    a->scale = (u8)(*a->data & 0xF);
    a->data++;
    a->nibble += 2;
  }
  if (a->nibble & 1) {
    sample = (s32)((u32)(*a->data & 0xF) << 28) >> 28;
    a->data++;
  } else {
    sample = (s32)((u32)(*a->data & 0xF0) << 24) >> 28;
  }
  a->nibble++;
  return sample;
}

// Decode one channel into every `step`-th s16 of out, with THPAudio.c's
// rounding (to nearest, ties to even, on a 16.16 value).
static void decodeChannel(const u8* coefs, s16 yn1, s16 yn2, const u8* data, u32 count, s16* out, int step) {
  ThpAdpcm a;
  u32 i;
  adpcmInit(&a, data);
  for (i = 0; i < count; ++i) {
    s32 sample = adpcmNext(&a);
    s64 yn = (s64)(s16)rd16(coefs + a.predictor * 4 + 2) * yn2;
    yn += (s64)(s16)rd16(coefs + a.predictor * 4) * yn1;
    yn += (s64)(sample << a.scale) << 11;
    yn <<= 5;
    if ((u16)(yn & 0xFFFF) > 0x8000 || ((u16)(yn & 0xFFFF) == 0x8000 && (yn & 0x10000))) {
      yn += 0x10000;
    }
    if (yn > 2147483647LL) {
      yn = 2147483647LL;
    }
    if (yn < -2147483648LL) {
      yn = -2147483648LL;
    }
    *out = (s16)(yn >> 16);
    out += step;
    yn2 = yn1;
    yn1 = (s16)(yn >> 16);
  }
}

u32 THPAudioDecode(s16* audioBuffer, u8* audioFrame, s32 flag) {
  u32 nextChannel, count;
  const u8* left;
  s16 *outL, *outR;
  int step;

  if (audioBuffer == NULL || audioFrame == NULL) {
    return 0;
  }
  nextChannel = rd32(audioFrame);
  count = rd32(audioFrame + 4);
  left = audioFrame + 0x50;

  if (flag == 1) { // planar: left block, then right
    outL = audioBuffer;
    outR = audioBuffer + count;
    step = 1;
  } else { // interleaved, left first (see the top of the file)
    outL = audioBuffer;
    outR = audioBuffer + 1;
    step = 2;
  }

  decodeChannel(audioFrame + 0x08, (s16)rd16(audioFrame + 0x48), (s16)rd16(audioFrame + 0x4A), left, count, outL,
                step);
  if (nextChannel == 0) {
    // Mono: the same samples on both sides.
    u32 i;
    for (i = 0; i < count; ++i) {
      outR[i * step] = outL[i * step];
    }
  } else {
    decodeChannel(audioFrame + 0x28, (s16)rd16(audioFrame + 0x4C), (s16)rd16(audioFrame + 0x4E),
                  left + nextChannel, count, outR, step);
  }
  return count;
}
