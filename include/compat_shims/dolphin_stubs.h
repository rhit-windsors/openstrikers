#pragma once
// aurora fixes
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/mtx.h>
#include <dolphin/mtx/mtx44ext.h>
typedef GXTexObj _GXTexObj;
typedef GXTlutObj _GXTlutObj;
typedef GXRenderModeObj _GXRenderModeObj;
typedef GXTexGenType _GXTexGenType;
typedef GXTexGenSrc _GXTexGenSrc;
typedef GXTevOp _GXTevOp;
typedef GXTevBias _GXTevBias;
typedef GXTevScale _GXTevScale;
typedef GXTevAlphaArg _GXTevAlphaArg;
typedef GXTevRegID _GXTevRegID;
typedef GXCullMode _GXCullMode;
typedef GXBlendFactor _GXBlendFactor;
typedef GXCompare _GXCompare;
typedef GXLightID _GXLightID;
typedef GXTexMtxType _GXTexMtxType;
typedef GXVtxFmt _GXVtxFmt;
typedef GXProjectionType _GXProjectionType;
typedef GXTexFmt _GXTexFmt;
typedef GXPrimitive _GXPrimitive;
typedef GXTlut _GXTlut;
inline void GXClearGPMetric() {}
inline void GXSetGPMetric(int, int) {}
inline void GXReadGPMetric(u32* val0, u32* val1) { *val0 = 0; *val1 = 0; }
inline GXRenderModeObj GXEurgb60Hz480IntDf = {0}; // stub
typedef const float (*CMtxP)[4];
inline void SISetSamplingRate(int) {}
// On hardware VI's retrace kicked off an SI transfer and this callback ran off
// that interrupt, once per field. Nothing here generates it, so compat_shims.cpp
// stores the callback and calls it once per host frame instead. It used to be a
// no-op, which meant VBlankPadUpdate() never ran and the pad state the game reads
// stayed zeroed forever.
typedef void (*PADSamplingCallback)(void);
void PADSetSamplingCallback(PADSamplingCallback callback);

#ifdef __cplusplus
extern "C" {
#endif
extern GXRenderModeObj GXNtsc480Prog; // declaration only, defined in compat_shims.cpp

typedef void (*VMLogStatsCallback)(u32 faultAddr, u32 mainAddr, u32 pageIndex, u32 elapsed, u32 wroteBack);
inline void GXWaitDrawDone() {}
#ifdef __cplusplus
}
#endif


