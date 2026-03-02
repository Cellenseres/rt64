//
// RT64
//

#include "rt64_gbi_s2dex.h"

#include <algorithm>
#include <array>
#include <limits>

#include "../include/rt64_extended_gbi.h"

#include "rt64_gbi_extended.h"
#include "rt64_gbi_f3d.h"
#include "rt64_gbi_f3dex.h"
#include "rt64_gbi_rdp.h"

//#define LOG_BGRECT_METHODS
//#define LOG_LOADTXTR_METHODS

namespace RT64 {
    namespace GBI_S2DEX {
        void objRenderMode(State *state, DisplayList **dl) {
            state->rsp->setObjRenderMode((*dl)->w1);
        }

        static uint8_t spriteLoadBlockSiz(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_4b:
            case G_IM_SIZ_8b:
            case G_IM_SIZ_16b:
                return G_IM_SIZ_16b;
            case G_IM_SIZ_32b:
                return G_IM_SIZ_32b;
            default:
                return siz;
            }
        }

        static uint32_t spriteSizBytes(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_8b:  return 1;
            case G_IM_SIZ_16b: return 2;
            case G_IM_SIZ_32b: return 4;
            default:           return 0;
            }
        }

        static bool spriteSupportsFullReplacementHash(uint8_t siz, int32_t width, int32_t height, int32_t stride, int32_t offsetS, int32_t offsetT) {
            if ((siz == G_IM_SIZ_4b) || (width <= 0) || (height <= 0)) {
                return false;
            }

            // Restrict this to large contiguous sprites so replacement hashes stay useful for
            // fullscreen/background art instead of every small UI element.
            return (stride == width) && (offsetS == 0) && (offsetT == 0) && ((width * height) >= 16384);
        }

        static bool beginFullReplacementSprite(State *state, RDP *rdp, uint8_t fmt, uint8_t siz, uint32_t imageAddress, uint16_t width,
            uint16_t renderLine, uint16_t renderLrs, uint16_t renderLrt, uint64_t &replacementHash)
        {
            rdp->setTextureImage(fmt, siz, width, imageAddress);
            rdp->setTile(G_TX_LOADTILE, fmt, siz, renderLine, 0, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            rdp->loadTileReplacementCheck(G_TX_LOADTILE, 0, 0, renderLrs, renderLrt, siz, fmt, 0, 0, replacementHash);
            state->startSpriteCommand(replacementHash);
            return state->ext.textureCache->hasReplacement(replacementHash);
        }

        static uint32_t spriteSizLineBytes(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_8b:  return 1;
            case G_IM_SIZ_16b: return 2;
            case G_IM_SIZ_32b: return 2;
            default:           return 0;
            }
        }

        static uint32_t spriteSizShift(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_4b:  return 2;
            case G_IM_SIZ_8b:  return 1;
            case G_IM_SIZ_16b:
            case G_IM_SIZ_32b:
            default:
                return 0;
            }
        }

        static uint32_t spriteSizIncr(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_4b:  return 3;
            case G_IM_SIZ_8b:  return 1;
            case G_IM_SIZ_16b:
            case G_IM_SIZ_32b:
            default:
                return 0;
            }
        }

        static uint32_t spriteMaxTexelsPerLoad(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_4b:  return 8192;
            case G_IM_SIZ_8b:  return 4096;
            case G_IM_SIZ_16b: return 2048;
            case G_IM_SIZ_32b: return 1024;
            default:           return 2048;
            }
        }

        static uint16_t spriteCalcDxt(uint32_t width, uint8_t siz) {
            if (siz == G_IM_SIZ_4b) {
                const uint32_t txl2Words = std::max<uint32_t>(1, width / 16);
                return uint16_t(((1U << 11) + txl2Words - 1) / txl2Words);
            }

            const uint32_t bytes = spriteSizBytes(siz);
            const uint32_t txl2Words = std::max<uint32_t>(1, (width * bytes) / 8);
            return uint16_t(((1U << 11) + txl2Words - 1) / txl2Words);
        }

        static int16_t clampS16(int32_t v) {
            constexpr int32_t MinS16 = std::numeric_limits<int16_t>::min();
            constexpr int32_t MaxS16 = std::numeric_limits<int16_t>::max();
            return int16_t(std::clamp(v, MinS16, MaxS16));
        }

        static bool spriteCanUseLoadBlock16(int32_t width) {
            static constexpr std::array<int32_t, 31> LoadBlock16Widths = {
                4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 112, 128, 168,
                192, 224, 256, 336, 384, 448, 512, 672, 768, 896, 1024, 1344, 1536,
                1792, 2048
            };

            return std::binary_search(LoadBlock16Widths.begin(), LoadBlock16Widths.end(), width);
        }

        static bool spriteNeedsYGuardRow(int32_t sy, int32_t ulyBase) {
            return (sy != 1024) || ((ulyBase & 0x3) != 0);
        }

        enum class SpriteLoadMethod {
            Block,
            Tile
        };

        static SpriteLoadMethod spriteLoadMethod(uint8_t fmt, uint8_t siz, int32_t width) {
            if ((fmt == G_IM_FMT_RGBA) && (siz == G_IM_SIZ_16b) && spriteCanUseLoadBlock16(width)) {
                return SpriteLoadMethod::Block;
            }

            return SpriteLoadMethod::Tile;
        }

        void sprite2DBase(State *state, DisplayList **dl) {
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
            const uint8_t *spriteBytes = state->fromRDRAM(rdramAddress);
            memcpy(state->rsp->S2D.sprite2D_buffer.data(), spriteBytes, sizeof(uSprite));
            state->rsp->S2D.sprite2D_valid = true;
        }

        void sprite2DScaleFlip(State *state, DisplayList **dl) {
            state->rsp->S2D.sprite2D_scale_x = std::max<uint16_t>((*dl)->p1(16, 16), 1);
            state->rsp->S2D.sprite2D_scale_y = std::max<uint16_t>((*dl)->p1(0, 16), 1);
            state->rsp->S2D.sprite2D_flip_x = ((*dl)->p0(8, 8) != 0) ? 1 : 0;
            state->rsp->S2D.sprite2D_flip_y = ((*dl)->p0(0, 8) != 0) ? 1 : 0;
        }

        void sprite2DDraw(State *state, DisplayList **dl) {
            if (!state->rsp->S2D.sprite2D_valid) {
                return;
            }

            RDP *rdp = state->rdp.get();
            const uSprite *sprite = reinterpret_cast<const uSprite *>(state->rsp->S2D.sprite2D_buffer.data());
            const uint8_t fmt = sprite->sourceImageFmt;
            const uint8_t siz = sprite->sourceImageSiz;
            if (siz > G_IM_SIZ_32b) {
                return;
            }

            const int32_t width = std::max<int32_t>(sprite->subImageWidth, 1);
            const int32_t height = std::max<int32_t>(sprite->subImageHeight, 1);
            const int32_t stride = std::max<int32_t>(sprite->stride, width);
            const int32_t sx = std::max<int32_t>(state->rsp->S2D.sprite2D_scale_x, 1);
            const int32_t sy = std::max<int32_t>(state->rsp->S2D.sprite2D_scale_y, 1);
            const int32_t ulx = int16_t((*dl)->p1(16, 16));
            const int32_t ulyBase = int16_t((*dl)->p1(0, 16));
            const int32_t lrx = ulx + int32_t((uint64_t(width) * 4096ULL + uint32_t(sx) - 1) / uint32_t(sx));
            const int32_t totalLry = ulyBase + int32_t((uint64_t(height) * 4096ULL + uint32_t(sy) - 1) / uint32_t(sy));
            if (lrx <= ulx) {
                return;
            }

            int16_t baseUls = 0;
            int16_t baseUlt = 0;
            uint32_t imageAddress = state->rsp->fromSegmented(sprite->sourceImage);
            const int32_t offsetS = std::max<int32_t>(sprite->sourceImageOffsetS, 0);
            const int32_t offsetT = std::max<int32_t>(sprite->sourceImageOffsetT, 0);
            if (siz == G_IM_SIZ_4b) {
                const int32_t texelOffset = offsetT * stride + offsetS;
                imageAddress += uint32_t(texelOffset >> 1);
                baseUls = int16_t((texelOffset & 1) ? 16 : 0);
            }
            else {
                const uint32_t bytesPerTexel = spriteSizBytes(siz);
                imageAddress += uint32_t((offsetT * stride + offsetS) * int32_t(bytesPerTexel));
            }

            const bool flipX = (state->rsp->S2D.sprite2D_flip_x != 0);
            const bool flipY = (state->rsp->S2D.sprite2D_flip_y != 0);
            const int16_t dsdx = clampS16(flipX ? -sx : sx);
            const int16_t dtdy = clampS16(flipY ? -sy : sy);

            int32_t effective_lrx = lrx;
            int16_t effective_dsdx = dsdx;
            if (!flipX) {
                const FixedRect &scissorRect = rdp->scissorRectStack[rdp->scissorStackSize - 1];
                const int32_t native_width = rdp->colorImage.width * 4;
                const int32_t native_height = 240 * 4; // N64 native height in 4x fixed-point
                const bool depthSourcePrim = (rdp->otherMode.zSource() == G_ZS_PRIM);
                if (depthSourcePrim
                    && (ulx <= 0) && (lrx >= native_width) && (scissorRect.lrx > native_width)
                    && (ulyBase <= 0) && (totalLry >= native_height)) {
                    effective_lrx = scissorRect.lrx;
                    effective_dsdx = clampS16(int32_t((int64_t(dsdx) * lrx) / scissorRect.lrx));
                }
            }

            const bool tightStride = (stride == width);
            const SpriteLoadMethod loadMethod = tightStride ? spriteLoadMethod(fmt, siz, width) : SpriteLoadMethod::Tile;
            const bool useLoadBlock = (loadMethod == SpriteLoadMethod::Block);
            const bool useYGuardRow = spriteNeedsYGuardRow(sy, ulyBase);
            const uint8_t loadSiz = spriteLoadBlockSiz(siz);
            const uint16_t dxt = spriteCalcDxt(uint32_t(width), siz);
            const uint32_t renderLineBytes = spriteSizLineBytes(siz);
            const uint16_t renderLine = uint16_t(((uint32_t(width) * renderLineBytes) + 7) >> 3);
            const uint16_t renderLrs = uint16_t((width - 1) << 2);
            const uint16_t renderLrt = uint16_t((height - 1) << 2);
            const int32_t maxLoadedRows = std::max<int32_t>(1, int32_t(spriteMaxTexelsPerLoad(siz) / uint32_t(width)));
            const int32_t guardBudget = useYGuardRow ? 2 : 0;
            const int32_t maxCoreRows = tightStride ? std::max<int32_t>(1, maxLoadedRows - guardBudget) : 1;
            const bool useFullReplacementHash = spriteSupportsFullReplacementHash(siz, width, height, stride, offsetS, offsetT);
            bool fullReplacementCommandActive = false;

            if (useFullReplacementHash) {
                uint64_t replacementHash = 0;
                const bool hasReplacement = beginFullReplacementSprite(state, rdp, fmt, siz, imageAddress, uint16_t(width), renderLine, renderLrs, renderLrt, replacementHash);
                fullReplacementCommandActive = (replacementHash != 0);

                if (hasReplacement) {
                    const int16_t uls = flipX ? clampS16(width << 5) : 0;
                    const int16_t ult = flipY ? clampS16(height << 5) : 0;

                    rdp->setTile(G_TX_RENDERTILE, fmt, siz, renderLine, 0, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    rdp->setTileReplacementHash(G_TX_RENDERTILE, replacementHash);
                    rdp->setTileSize(G_TX_RENDERTILE, 0, 0, renderLrs, renderLrt);
                    rdp->drawTexRect(ulx, ulyBase, effective_lrx, totalLry, G_TX_RENDERTILE, uls, ult, effective_dsdx, dtdy, false);
                    rdp->clearTileReplacementHash(G_TX_RENDERTILE);
                    state->endSpriteCommand();
                    return;
                }
            }

            for (int32_t coreRowStart = 0; coreRowStart < height; coreRowStart += maxCoreRows) {
                const int32_t coreRows = std::min<int32_t>(maxCoreRows, height - coreRowStart);
                int32_t loadStart = coreRowStart;
                int32_t loadEnd = coreRowStart + coreRows;
                if (useYGuardRow) {
                    int32_t extraGuardBudget = std::max<int32_t>(0, maxLoadedRows - coreRows);
                    if ((extraGuardBudget > 0) && (loadStart > 0)) {
                        loadStart--;
                        extraGuardBudget--;
                    }

                    if ((extraGuardBudget > 0) && (loadEnd < height)) {
                        loadEnd++;
                    }
                }

                const int32_t loadedRows = std::max<int32_t>(1, loadEnd - loadStart);
                const int32_t guardTop = coreRowStart - loadStart;

                uint32_t chunkAddress = imageAddress;
                if (siz == G_IM_SIZ_4b) {
                    chunkAddress += uint32_t((loadStart * stride) >> 1);
                }
                else {
                    chunkAddress += uint32_t(loadStart * stride * int32_t(spriteSizBytes(siz)));
                }

                if (useLoadBlock) {
                    const int32_t texelCount = width * loadedRows;
                    const uint16_t loadLrs = uint16_t((((uint32_t(texelCount) + spriteSizIncr(siz)) >> spriteSizShift(siz)) - 1) & 0xFFF);
                    rdp->setTextureImage(fmt, loadSiz, 1, chunkAddress);
                    rdp->setTile(G_TX_LOADTILE, fmt, loadSiz, 0, 0, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    rdp->loadBlock(G_TX_LOADTILE, 0, 0, loadLrs, dxt);
                }
                else {
                    rdp->setTextureImage(fmt, siz, uint16_t(stride), chunkAddress);
                    rdp->setTile(G_TX_LOADTILE, fmt, siz, renderLine, 0, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    rdp->loadTile(G_TX_LOADTILE, 0, 0, renderLrs, uint16_t((loadedRows - 1) << 2));
                }

                rdp->setTile(G_TX_RENDERTILE, fmt, siz, renderLine, 0, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                rdp->setTileSize(G_TX_RENDERTILE, 0, 0, renderLrs, uint16_t((loadedRows - 1) << 2));

                const int32_t chunkUly = ulyBase + int32_t((uint64_t(coreRowStart) * 4096ULL + uint32_t(sy) - 1) / uint32_t(sy));
                const int32_t chunkLry = ulyBase + int32_t((uint64_t(coreRowStart + coreRows) * 4096ULL + uint32_t(sy) - 1) / uint32_t(sy));
                const int16_t uls = flipX ? clampS16((width << 5) + baseUls) : baseUls;
                const int32_t ultBase = flipY ? (int32_t(baseUlt) + ((guardTop + coreRows) << 5)) : (int32_t(baseUlt) + (guardTop << 5));
                const int16_t ult = clampS16(ultBase);
                rdp->drawTexRect(ulx, chunkUly, effective_lrx, chunkLry, G_TX_RENDERTILE, uls, ult, effective_dsdx, dtdy, false);
            }

            if (fullReplacementCommandActive) {
                state->endSpriteCommand();
            }
        }

        static void setObjSpriteTile(State *state, const uObjSprite &sprite) {
            RDP *rdp = state->rdp.get();
            const uint32_t w = std::max<uint32_t>(sprite.imageW >> 5, 1);
            const uint32_t h = std::max<uint32_t>(sprite.imageH >> 5, 1);
            rdp->setTile(
                G_TX_RENDERTILE,
                sprite.imageFmt,
                sprite.imageSiz,
                sprite.imageStride,
                sprite.imageAdrs,
                sprite.imagePal,
                G_TX_CLAMP | G_TX_NOMIRROR,
                G_TX_CLAMP | G_TX_NOMIRROR,
                0,
                0,
                0,
                0
            );
            rdp->setTileSize(G_TX_RENDERTILE, 0, 0, uint16_t((w - 1) << 2), uint16_t((h - 1) << 2));
        }

        static void drawObjRectLike(State *state, const uObjSprite &sprite, bool useMatrix) {
            RDP *rdp = state->rdp.get();

            const uint32_t rawScaleW = std::max<uint32_t>(sprite.scaleW, 1);
            const uint32_t rawScaleH = std::max<uint32_t>(sprite.scaleH, 1);
            const uint32_t scaleW = useMatrix ? std::max<uint32_t>((uint64_t(state->rsp->S2D.objBaseScaleX) * rawScaleW) >> 10, 1) : rawScaleW;
            const uint32_t scaleH = useMatrix ? std::max<uint32_t>((uint64_t(state->rsp->S2D.objBaseScaleY) * rawScaleH) >> 10, 1) : rawScaleH;
            const uint32_t imageW = std::max<uint32_t>(sprite.imageW, 1);
            const uint32_t imageH = std::max<uint32_t>(sprite.imageH, 1);

            int32_t ulx = int16_t(sprite.objX);
            int32_t uly = int16_t(sprite.objY);
            int32_t lrx = ulx + int32_t((uint64_t(imageW) * (0x80007FFFULL / scaleW)) >> 24);
            int32_t lry = uly + int32_t((uint64_t(imageH) * (0x80007FFFULL / scaleH)) >> 24);

            if (useMatrix) {
                auto tx = [&](int32_t x, int32_t y) -> int32_t {
                    return int32_t(((int64_t(x) * state->rsp->S2D.objMtxA) + (int64_t(y) * state->rsp->S2D.objMtxB)) >> 16) + state->rsp->S2D.objMtxX;
                };
                auto ty = [&](int32_t x, int32_t y) -> int32_t {
                    return int32_t(((int64_t(x) * state->rsp->S2D.objMtxC) + (int64_t(y) * state->rsp->S2D.objMtxD)) >> 16) + state->rsp->S2D.objMtxY;
                };

                const int32_t x0 = tx(ulx, uly);
                const int32_t y0 = ty(ulx, uly);
                const int32_t x1 = tx(lrx, uly);
                const int32_t y1 = ty(lrx, uly);
                const int32_t x2 = tx(ulx, lry);
                const int32_t y2 = ty(ulx, lry);
                const int32_t x3 = tx(lrx, lry);
                const int32_t y3 = ty(lrx, lry);

                ulx = std::min(std::min(x0, x1), std::min(x2, x3));
                uly = std::min(std::min(y0, y1), std::min(y2, y3));
                lrx = std::max(std::max(x0, x1), std::max(x2, x3));
                lry = std::max(std::max(y0, y1), std::max(y2, y3));
            }

            if ((lrx <= ulx) || (lry <= uly)) {
                return;
            }

            setObjSpriteTile(state, sprite);

            int16_t uls = 0;
            int16_t ult = 0;
            int16_t dsdx = clampS16(int32_t(rawScaleW));
            int16_t dtdy = clampS16(int32_t(rawScaleH));
            if ((sprite.imageFlags & S2DEX_G_BG_FLAG_FLIPS) != 0) {
                uls = clampS16(int32_t(sprite.imageW));
                dsdx = clampS16(-int32_t(rawScaleW));
            }

            if ((sprite.imageFlags & S2DEX_G_BG_FLAG_FLIPT) != 0) {
                ult = clampS16(int32_t(sprite.imageH));
                dtdy = clampS16(-int32_t(rawScaleH));
            }

            if ((sprite.imageFlags & S2DEX_G_BG_FLAG_FLIPS) == 0) {
                const FixedRect &scissorRect = rdp->scissorRectStack[rdp->scissorStackSize - 1];
                const int32_t native_width = rdp->colorImage.width * 4;
                const int32_t native_height = 240 * 4; // N64 native height in 4x fixed-point
                const bool depthSourcePrim = (rdp->otherMode.zSource() == G_ZS_PRIM);
                if (depthSourcePrim
                    && (ulx <= 0) && (lrx >= native_width) && (scissorRect.lrx > native_width)
                    && (uly <= 0) && (lry >= native_height)) {
                    const int32_t old_lrx = lrx;
                    lrx = scissorRect.lrx;
                    dsdx = clampS16(int32_t((int64_t(dsdx) * old_lrx) / scissorRect.lrx));
                }
            }

            rdp->drawTexRect(ulx, uly, lrx, lry, G_TX_RENDERTILE, uls, ult, dsdx, dtdy, false);
        }

        void objRectangleOrMoveMem(State *state, DisplayList **dl) {
            if (isObjDma0Command(*dl)) {
                objRectangle(state, dl);
            }
            else {
                GBI_F3D::moveMem(state, dl);
            }
        }

        void objSpriteOrVertex(State *state, DisplayList **dl) {
            if (isObjDma0Command(*dl)) {
                objSprite(state, dl);
            }
            else {
                GBI_F3D::vertex(state, dl);
            }
        }

        void objMoveMem(State *state, DisplayList **dl) {
            switch ((*dl)->p0(16, 8)) {
            case S2DEX_MV_MATRIX: {
                const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
                const uObjMtx *objMtx = reinterpret_cast<const uObjMtx *>(state->fromRDRAM(rdramAddress));
                state->rsp->S2D.objMtxA = objMtx->A;
                state->rsp->S2D.objMtxB = objMtx->B;
                state->rsp->S2D.objMtxC = objMtx->C;
                state->rsp->S2D.objMtxD = objMtx->D;
                state->rsp->S2D.objMtxX = objMtx->X;
                state->rsp->S2D.objMtxY = objMtx->Y;
                state->rsp->S2D.objBaseScaleX = std::max<uint16_t>(objMtx->baseScaleX, 1);
                state->rsp->S2D.objBaseScaleY = std::max<uint16_t>(objMtx->baseScaleY, 1);
                break;
            }
            case S2DEX_MV_SUBMATRIX: {
                const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
                const uObjSubMtx *objMtx = reinterpret_cast<const uObjSubMtx *>(state->fromRDRAM(rdramAddress));
                state->rsp->S2D.objMtxX = objMtx->X;
                state->rsp->S2D.objMtxY = objMtx->Y;
                state->rsp->S2D.objBaseScaleX = std::max<uint16_t>(objMtx->baseScaleX, 1);
                state->rsp->S2D.objBaseScaleY = std::max<uint16_t>(objMtx->baseScaleY, 1);
                break;
            }
            case S2DEX_MV_VIEWPORT:
                state->rsp->setViewport((*dl)->w1);
                break;
            default:
                break;
            }
        }

        void objRectangle(State *state, DisplayList **dl) {
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
            const uint8_t *spriteBytes = state->fromRDRAM(rdramAddress);
            const uObjSprite *sprite = reinterpret_cast<const uObjSprite *>(spriteBytes);
            drawObjRectLike(state, *sprite, false);
        }

        void objRectangleR(State *state, DisplayList **dl) {
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
            const uObjSprite *sprite = reinterpret_cast<const uObjSprite *>(state->fromRDRAM(rdramAddress));
            drawObjRectLike(state, *sprite, true);
        }

        void objSprite(State *state, DisplayList **dl) {
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
            const uObjSprite *sprite = reinterpret_cast<const uObjSprite *>(state->fromRDRAM(rdramAddress));
            drawObjRectLike(state, *sprite, true);
        }

        void moveWord(State *state, DisplayList **dl) {
            if (storeMoveWordStatus(state, *dl)) {
                return;
            }

            GBI_F3D::moveWord(state, dl);
        }

        void rdpHalf0(State *state, DisplayList **dl) {
            uint8_t nextCode = (*dl + 1)->w0 >> 24;
            if (nextCode == S2DEX_G_SELECT_DL) {
                assert(false);
            }
            else if (nextCode == F3D_G_RDPHALF_1) {
                GBI_RDP::texrect(state, dl);
            }
        }

        void bg1CycTMEMLoadTile(RDP &rdp, const uObjScaleBg_t &scaleBg, uint32_t imagePtr, uint16_t imageSrcWsize, int16_t loadLines, int16_t tmemSH) {
            // TODO: Does it make sense that when using 32-bit images, the lrt is 0?
            const bool is32Bits = (scaleBg.imageSiz == G_IM_SIZ_32b);
            uint8_t textureImageSiz = is32Bits ? G_IM_SIZ_32b : G_IM_SIZ_16b;
            uint16_t tileLrt = is32Bits ? 0 : ((loadLines << 2) - 1);
            rdp.setTextureImage(G_IM_FMT_RGBA, textureImageSiz, imageSrcWsize >> 1, imagePtr);
            rdp.loadTile(G_TX_LOADTILE, 0, 0, (tmemSH - 1) << 4, tileLrt);
        }
        
        void bg1CycTMEMSetAndLoadTile(RDP &rdp, const uObjScaleBg_t &scaleBg, uint32_t imagePtr, uint16_t imageSrcWsize, int16_t loadLines, uint16_t tmemSliceWmax, int16_t tmemAdrs, int16_t tmemSH) {
            const bool is32Bits = (scaleBg.imageSiz == G_IM_SIZ_32b);
            uint8_t setSiz = is32Bits ? G_IM_SIZ_32b : G_IM_SIZ_16b;
            rdp.setTile(G_TX_LOADTILE, G_IM_FMT_RGBA, setSiz, tmemSliceWmax, tmemAdrs, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            bg1CycTMEMLoadTile(rdp, scaleBg, imagePtr, imageSrcWsize, loadLines, tmemSH);
        }

        void bg1CycTMEMLoad(RDP &rdp, const uObjScaleBg_t &scaleBg, uint32_t imageTop, uint32_t &imagePtr, int16_t &imageRemain, uint16_t imageSrcWsize, uint16_t imagePtrX0, 
            int16_t tmemSrcLines, uint16_t tmemSliceWmax, int16_t drawLines, int16_t usesBilerp, int16_t flagSplit)
        {
            int16_t loadLines = drawLines + usesBilerp;
            int16_t iLoadable = imageRemain - flagSplit;

            // Load everything at once.
            if (iLoadable >= loadLines) {
                bg1CycTMEMLoadTile(rdp, scaleBg, imagePtr, imageSrcWsize, loadLines, tmemSliceWmax);
                imagePtr += imageSrcWsize * drawLines;
                imageRemain -= drawLines;
            }
            // Partitioned load.
            else {
                uint32_t imageTopSeg = imageTop & 0xFF000000;
                int16_t subSliceY2 = imageRemain;
                int16_t subSliceL2 = loadLines - subSliceY2;
                int16_t subSliceD2 = drawLines - subSliceY2;
                if (subSliceL2 > 0) {
                    uint32_t imagePtr2 = imageTop + imagePtrX0;
                    if (subSliceY2 & 0x1) {
                        imagePtr2 -= imageSrcWsize;
                        imagePtr2 = imageTopSeg | (imagePtr2 & 0x00FFFFFF);
                        subSliceY2--;
                        subSliceL2++;
                    }

                    bg1CycTMEMSetAndLoadTile(rdp, scaleBg, imagePtr2, imageSrcWsize, subSliceL2, tmemSliceWmax, subSliceY2 * tmemSliceWmax, tmemSliceWmax);
                }

                if (flagSplit) {
                    uint32_t imagePtr1A = imagePtr + iLoadable * imageSrcWsize;
                    uint32_t imagePtr1B = imageTop;
                    int16_t subSliceY1 = iLoadable;
                    int16_t subSliceL1 = iLoadable & 1;
                    if (subSliceL1) {
                        imagePtr1A -= imageSrcWsize;
                        imagePtr1B -= imageSrcWsize;
                        imagePtr1B = imageTopSeg | (imagePtr1B & 0x00FFFFFF);
                        subSliceY1--;
                    }

                    subSliceL1++;

                    int16_t tmemSH_A = (imageSrcWsize - imagePtrX0) >> 3;
                    int16_t tmemSH_B = tmemSliceWmax - tmemSH_A;
                    bg1CycTMEMSetAndLoadTile(rdp, scaleBg, imagePtr1B, imageSrcWsize, subSliceL1, tmemSliceWmax, subSliceY1 * tmemSliceWmax + tmemSH_A, tmemSH_B);
                    bg1CycTMEMSetAndLoadTile(rdp, scaleBg, imagePtr1A, imageSrcWsize, subSliceL1, tmemSliceWmax, subSliceY1 * tmemSliceWmax, tmemSH_A);
                }

                if (iLoadable > 0) {
                    bg1CycTMEMSetAndLoadTile(rdp, scaleBg, imagePtr, imageSrcWsize, iLoadable, tmemSliceWmax, 0, tmemSliceWmax);
                }
                else {
                    uint8_t loadSiz = (scaleBg.imageSiz == G_IM_SIZ_32b) ? G_IM_SIZ_32b : G_IM_SIZ_16b;
                    rdp.setTile(G_TX_LOADTILE, G_IM_FMT_RGBA, loadSiz, tmemSliceWmax, 0, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                }

                imageRemain -= drawLines;
                if (imageRemain > 0) {
                    imagePtr += imageSrcWsize * drawLines;
                }
                else {
                    imageRemain = tmemSrcLines - subSliceD2;
                    imagePtr = imageTop + subSliceD2 * imageSrcWsize + imagePtrX0;
                }
            }
        }

        void bg1Cyc(State *state, DisplayList **dl) {
#       ifdef LOG_BGRECT_METHODS
            RT64_LOG_PRINTF("bg1Cyc::start(0x%08X)", (*dl)->w1);
#       endif

            RDP *rdp = state->rdp.get();
            RSP *rsp = state->rsp.get();
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
            // TODO load this into the S2D struct buffer for a more accurate implementation in case there's ever command state bleed.
            const uObjBg *bgObject = reinterpret_cast<const uObjBg *>(state->fromRDRAM(rdramAddress));
            const uObjBg_t &bg = bgObject->bg;
            const uObjScaleBg_t &scaleBg = bgObject->scaleBg;

            // Validate the image loading method is one of the two supported methods. It's worth noting 
            // that despite the load block method being documented, it doesn't actually work in the retail
            // microcode and it should therefore be treated as load tile as well.
            assert((bg.imageLoad == S2DEX_G_BGLT_LOADBLOCK) || (bg.imageLoad == S2DEX_G_BGLT_LOADTILE));

            // The scale should at least be 1.
            uint16_t scaleW = std::max(scaleBg.scaleW, uint16_t(1));
            uint16_t scaleH = std::max(scaleBg.scaleH, uint16_t(1));

            // Max frame area determined from the image size and the scale.
            int32_t frameWmax = (int32_t(scaleBg.imageW) << 10) / scaleW;
            int32_t frameHmax = (int32_t(scaleBg.imageH) << 10) / scaleH;
            frameWmax = (frameWmax - 1) & ~0x3;
            frameHmax = (frameHmax - 1) & ~0x3;

            // Clamp the frame's width to the max frame area.
            int16_t frameW = scaleBg.frameW;
            int16_t frameH = scaleBg.frameH;
            frameWmax = std::max(scaleBg.frameW - frameWmax, 0);
            frameHmax = std::max(scaleBg.frameH - frameHmax, 0);
            frameW -= frameWmax;
            frameH -= frameHmax;
            
            int16_t frameX0 = scaleBg.frameX;
            int16_t frameY0 = scaleBg.frameY;
            const bool imageFlipS = scaleBg.imageFlip & S2DEX_G_BG_FLAG_FLIPS;
            if (imageFlipS) {
                frameX0 += frameWmax;
            }

            // TODO: This scissor state probably needs to be pulled from the one tracked
            // by the RSP instead of the RDP, which is not currently implemented yet.
            const FixedRect &scissorRect = rdp->scissorRectStack[rdp->scissorStackSize - 1];
            int32_t scissorX0 = scissorRect.ulx;
            int32_t scissorY0 = scissorRect.uly;
            int32_t scissorX1 = scissorRect.lrx;
            int32_t scissorY1 = scissorRect.lry;
            int16_t pixX0 = int16_t(std::max(scissorX0 - frameX0, 0));
            int16_t pixY0 = int16_t(std::max(scissorY0 - frameY0, 0));
            int16_t pixX1 = int16_t(std::max(frameW - scissorX1 + frameX0, 0));
            int16_t pixY1 = int16_t(std::max(frameH - scissorY1 + frameY0, 0));

            // Cut out the part outside of the current scissor.
            frameW = frameW - (pixX0 + pixX1);
            frameH = frameH - (pixY0 + pixY1);
            frameX0 = frameX0 + pixX0;
            frameY0 = frameY0 + pixY0;

            // The frame is no longer valid, don't draw anything.
            if ((frameW <= 0) || (frameH <= 0)) {
                return;
            }

            // Compute the frame size after being clamped by the scissor.
            int16_t frameX1 = frameX0 + frameW;
            int16_t framePtrY0 = frameY0 >> 2;
            int16_t frameRemain = frameH >> 2;
            int16_t imageSrcW = scaleBg.imageW << 3;
            int16_t imageSrcH = scaleBg.imageH << 3;
            
            // Find the corresponding range in the image for the frame.
            // The image size needs to be extended when using bilerp.
            int16_t usesBilerp = (rsp->objRenderMode & S2DEX_G_OBJRM_BILERP) ? 1 : 0;

            // Some games will incorrectly set this flag despite not actually using bilerp during drawing. The side effect is that it'll end
            // up using a method of loading textures that makes it almost impossible for the emulation to detect tiles that can be upscaled due
            // to the various shenanigans the microcode has to do to load an image that is bigger than what it really is.
            // 
            // This enhancement falls under the category of a developer intended fix because there's a clear mismatch between the end result
            // and the loading method used to achieve it, as evidenced by the current state of the RDP. The end result is it makes the loading
            // logic much simpler and allows the emulation to upscale the tiles properly.
            //
            const bool rdpUsesBilerp = (rdp->otherMode.textFilt() == G_TF_BILERP);
            const bool bilerpFixEnabled = state->ext.enhancementConfig->s2dex.fixBilerpMismatch;
            if (usesBilerp && !rdpUsesBilerp && bilerpFixEnabled) {
                usesBilerp = 0;
            }

            int16_t imageW = (frameW * scaleW) >> 7;
            int16_t imageSliceW = imageW + (usesBilerp * 32);
            int16_t imageX0;
            if (imageFlipS) {
                imageX0 = scaleBg.imageX + ((pixX1 * scaleW) >> 7);
            }
            else {
                imageX0 = scaleBg.imageX + ((pixX0 * scaleW) >> 7);
            }

            int16_t imageY0 = scaleBg.imageY + (pixY0 * scaleH >> 7);
            int32_t imageYorig = scaleBg.imageYorig;

            // Keep scrolling down the image one row at a time if the 
            // left of the image is greater than the source's width.
            while (imageX0 >= imageSrcW) {
                imageX0 -= imageSrcW;
                imageY0 += 32;
                imageYorig += 32;
            }

            // Loop around when the top of the image is greater than
            // the source's height.
            while (imageY0 >= imageSrcH) {
                imageY0 -= imageSrcH;
                imageYorig -= imageSrcH;
            }

            // The TMEM loads will need to be split if the image range covers the entire image's width.
            int16_t flagSplit = (imageX0 + imageSliceW >= imageSrcW);

            // How many lines can be loaded into TMEM per draw.
            int16_t tmemSrcLines = imageSrcH >> 5;

            // Determine the amount of TMEM that can be used based on the format and size of the image.
            // We limit the amount of TMEM that can be used to half for CI images since the upper region
            // needs to be used for the palette.
            int16_t tmemSize = (scaleBg.imageFmt == G_IM_FMT_CI) ? 256 : 512;
            int16_t tmemShift = (0x200 >> scaleBg.imageSiz);
            int16_t tmemMask = (tmemShift - 1);
            int32_t imageSliceWmax;
            if (scaleBg.imageSiz == G_IM_SIZ_32b) {
                tmemSize = 480;
                imageSliceWmax = 0x2800;
            }
            else {
                imageSliceWmax = (scaleBg.frameW * scaleW) >> 7;
                imageSliceWmax = std::min(imageSliceWmax + (usesBilerp << 5), int32_t(imageSrcW));
            }

            // Get the amount of lines that can be loaded into TMEM at once.
            uint16_t tmemSliceWmax = (imageSliceWmax + tmemMask) / tmemShift + 1;
            int16_t tmemSliceLines = tmemSize / tmemSliceWmax;
            int16_t imageSliceLines = tmemSliceLines - usesBilerp;
            int32_t frameSliceLines = (imageSliceLines << 20) / scaleH;

            // Figure out which line to start with from the image coordinates.
            int32_t imageLYoffset = (int32_t(imageY0) - imageYorig) << 5;
            if (imageLYoffset < 0) {
                imageLYoffset -= (scaleH - 1);
            }

            int32_t frameLYoffset = (imageLYoffset / scaleH) << 10;

            // Determine which slice number corresponds to this offset.
            int32_t imageNumSlice;
            if (frameLYoffset >= 0) {
                imageNumSlice = frameLYoffset / frameSliceLines;
            }
            else {
                imageNumSlice = (frameLYoffset - frameSliceLines + 1) / frameSliceLines;
            }

            // How much of the first drawn rectangle will be hidden at the top of the frame.
            int32_t frameLSliceL0 = frameSliceLines * imageNumSlice;
            int32_t frameLYslice = frameLSliceL0 & ~0x3FF;
            int32_t frameLHidden = frameLYoffset - frameLYslice;
            int32_t imageLHidden = (frameLHidden >> 10) * scaleH;
            frameLSliceL0 = (frameLSliceL0 & 0x3FF) + frameSliceLines - frameLHidden;

            // Image parameters for the slice.
            uint16_t imageT = (imageLHidden >> 5) & 0x1F;
            imageLHidden >>= 10;
            int16_t imageISliceL0 = imageSliceLines - imageLHidden;
            int16_t imageIY0 = imageSliceLines * imageNumSlice + (imageYorig & ~0x1F) / 32 + imageLHidden;
            uint16_t imageHLowered = (scaleBg.imageH >> 2);
            if (imageIY0 < 0) {
                imageIY0 += imageHLowered;
            }

            if (imageIY0 >= imageHLowered) {
                imageIY0 -= imageHLowered;
            }

            const uint32_t imageAddress = state->rsp->fromSegmented(scaleBg.imageAddress);
            uint16_t imageSrcWsize = (imageSrcW / tmemShift) << 3;
            uint16_t imagePtrX0 = (imageX0 / tmemShift) << 3;
            uint32_t imagePtr = imageAddress + imageSrcWsize * imageIY0 + imagePtrX0;
            uint16_t imageS = imageX0 & tmemMask;
            if (imageFlipS) {
                imageS = -(imageS + imageW);
            }

            // RDP Commands constant throughout the image.
            uint8_t loadSiz = (scaleBg.imageSiz == G_IM_SIZ_32b) ? G_IM_SIZ_32b : G_IM_SIZ_16b;
            rdp->setTile(G_TX_LOADTILE, G_IM_FMT_RGBA, loadSiz, tmemSliceWmax, 0, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            rdp->setTile(G_TX_RENDERTILE, scaleBg.imageFmt, scaleBg.imageSiz, tmemSliceWmax, 0, uint8_t(scaleBg.imagePal), G_TX_WRAP | G_TX_MIRROR, G_TX_WRAP | G_TX_MIRROR, 0xF, 0xF, 0, 0);

            // Attempt to use a single tile instead to draw the entire rect at once if there's a framebuffer copy available.
            // If it fails to find a possible tile copy, fall back to the regular approach.
            // FIXME: The rest of the code should still run to account for state bleeding without drawing any rectangles.
            if (state->ext.enhancementConfig->s2dex.framebufferFastPath) {
                int16_t uls = imageS;
                int16_t ult = imageT;
                int16_t lrs = uls + scaleBg.imageW;
                int16_t lrt = ult + scaleBg.imageH;
                rdp->setTextureImage(G_IM_FMT_RGBA, loadSiz, imageSrcWsize >> 1, imagePtr);
                rdp->setTileSize(G_TX_RENDERTILE, uls, ult, lrs, lrt);

                uint64_t replacementHash = 0;
                bool replacementCheck = rdp->loadTileReplacementCheck(G_TX_LOADTILE, uls, ult, lrs, lrt, scaleBg.imageSiz, scaleBg.imageFmt, scaleBg.imageLoad, scaleBg.imagePal, replacementHash);
                bool singleTileMode = replacementCheck || rdp->loadTileCopyCheck(G_TX_LOADTILE, uls, ult, lrs, lrt);
                state->startSpriteCommand(replacementHash);
                if (singleTileMode) {
                    int32_t uly = frameY0 & ~0x3;
                    rdp->setTileReplacementHash(G_TX_RENDERTILE, replacementHash);
                    rdp->loadTile(G_TX_LOADTILE, uls, ult, lrs, lrt);
                    rdp->drawTexRect(frameX0, uly, frameX1, uly + frameH, G_TX_RENDERTILE, uls, ult, scaleW, scaleH, false);
                    rdp->clearTileReplacementHash(G_TX_RENDERTILE);
                    state->endSpriteCommand();
                    return;
                }
            }

            rdp->setTileSize(G_TX_RENDERTILE, 0, 0, 0, 0);
            
            // Draw the image.
            int16_t imageRemain = tmemSrcLines - imageIY0;
            int16_t imageSliceH = imageISliceL0;
            int32_t frameSliceCount = frameLSliceL0;
            while (frameRemain > 0) {
                int16_t frameSliceH = frameSliceCount >> 10;
                if (frameSliceH <= 0) {
                    imageRemain -= imageSliceH;
                    if (imageRemain > 0) {
                        imagePtr += imageSrcWsize * imageSliceH;
                    }
                    else {
                        imagePtr = imageAddress - (imageRemain * imageSrcWsize) + imagePtrX0;
                        imageRemain += tmemSrcLines;
                    }
                }
                else {
                    frameSliceCount &= 0x3FF;
                    frameRemain -= frameSliceH;

                    // Final slice.
                    if (frameRemain < 0) {
                        frameSliceH += frameRemain;
                        imageSliceH += ((frameRemain * scaleH) >> 10) + 1;
                        imageSliceH = std::min(imageSliceH, imageSliceLines);
                    }

                    bg1CycTMEMLoad(*rdp, scaleBg, imageAddress, imagePtr, imageRemain, imageSrcWsize, imagePtrX0, tmemSrcLines, tmemSliceWmax, imageSliceH, usesBilerp, flagSplit);

                    int16_t framePtrY1 = framePtrY0 + frameSliceH;
                    rdp->drawTexRect(frameX0, framePtrY0 << 2, frameX1, framePtrY1 << 2, G_TX_RENDERTILE, imageS, imageT, scaleW, scaleH, false);
                    framePtrY0 = framePtrY1;
                }

                frameSliceCount += frameSliceLines;
                imageSliceH = imageSliceLines;
                imageT = 0;
            }

            state->endSpriteCommand();

#       ifdef LOG_BGRECT_METHODS
            RT64_LOG_PRINTF("bg1Cyc::end(0x%08X)", (*dl)->w1);
#       endif
        }
        
        void bgCopy(State *state, DisplayList **dl) {
            // TODO: Reimplement more accurately to match the microcode.

#       ifdef LOG_BGRECT_METHODS
            RT64_LOG_PRINTF("bgCopy::start(0x%08X)", (*dl)->w1);
#       endif
            
            RDP *rdp = state->rdp.get();
            RSP* rsp = state->rsp.get();
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
            // TODO load this into the S2D struct buffer for a more accurate implementation in case there's ever command state bleed.
            const uObjBg *bgObject = reinterpret_cast<const uObjBg *>(state->fromRDRAM(rdramAddress));
            const uObjBg_t &bg = bgObject->bg;
            assert(bg.imageLoad == S2DEX_G_BGLT_LOADTILE); // TODO: Only the load tile version is implemented.
            
            const uint16_t TMEMAddress = 0;
            const uint8_t lineShiftOffset = (bg.imageSiz == G_IM_SIZ_32b) ? 1 : 0;
            const uint16_t bgLine = bg.imageW >> (2 + bg.imageSiz - lineShiftOffset);
            const uint16_t dsdx = 4 << 10;
            const uint16_t dsdy = 4 << 8;
            const uint16_t lrSubstract = 4;
            const uint16_t bgRectLrs = bg.imageW - lrSubstract;
            const uint32_t savedOtherModeH = rdp->otherMode.H;
            rsp->setTextureImage(bg.imageFmt, bg.imageSiz, bg.imageW >> 2, state->rsp->fromSegmented(bg.imageAddress));
            rdp->setTile(G_TX_LOADTILE, bg.imageFmt, bg.imageSiz, bgLine, TMEMAddress, static_cast<uint8_t>(bg.imagePal), 0, 0, 0, 0, 0, 0);
            rdp->setTile(G_TX_RENDERTILE, bg.imageFmt, bg.imageSiz, bgLine, TMEMAddress, static_cast<uint8_t>(bg.imagePal), 0, 0, 0, 0, 0, 0);

            // Attempt to use a single tile instead to draw the entire rect at once if there's a framebuffer copy available.
            // If it fails to find a possible tile copy, fall back to the regular approach.
            // FIXME: The rest of the code should still run to account for state bleeding without drawing any rectangles.
            uint16_t bgRectLrt = bg.imageH - lrSubstract;
            uint64_t replacementHash = 0;
            if (state->ext.enhancementConfig->s2dex.framebufferFastPath) {
                bool replacementCheck = rdp->loadTileReplacementCheck(G_TX_LOADTILE, 0, 0, bgRectLrs, bgRectLrt, bg.imageSiz, bg.imageFmt, bg.imageLoad, bg.imagePal, replacementHash);
                bool singleTileMode = replacementCheck || rdp->loadTileCopyCheck(G_TX_LOADTILE, 0, 0, bgRectLrs, bgRectLrt);
                state->startSpriteCommand(replacementHash);
                if (singleTileMode) {
                    rdp->setTileReplacementHash(G_TX_RENDERTILE, replacementHash);
                    rdp->setTileSize(G_TX_RENDERTILE, 0, 0, bgRectLrs, bgRectLrt);
                    rdp->loadTile(G_TX_LOADTILE, 0, 0, bgRectLrs, bgRectLrt);
                    rdp->drawTexRect(bg.frameX, bg.frameY, bg.frameX + bg.frameW - lrSubstract, bg.frameY + bg.frameH - lrSubstract, G_TX_RENDERTILE, 0, 0, dsdx, dsdy, false);
                    rdp->clearTileReplacementHash(G_TX_RENDERTILE);
                    state->endSpriteCommand();
                    return;
                }
            }

            const uint32_t Division = (bg.imageFmt == G_IM_FMT_CI) ? 2 : 1;
            const uint16_t bgRectCount = bg.imageH / (bg.tmemH / Division); // TODO: Check for remaining pixels for the last Texrect.
            bgRectLrt = (bg.tmemH / Division) - lrSubstract;
            rdp->setTileSize(G_TX_RENDERTILE, 0, 0, bgRectLrs, bgRectLrt);

            for (uint16_t r = 0; r < bgRectCount; r++) {
                const uint16_t bgRectUlt = r * (bg.tmemH / Division);
                rdp->loadTile(G_TX_LOADTILE, 0, bgRectUlt, bgRectLrs, bgRectUlt + bgRectLrt);
                rdp->drawTexRect(bg.frameX, bg.frameY + bgRectUlt, bg.frameX + bg.frameW - lrSubstract, bg.frameY + bgRectUlt + bgRectLrt, G_TX_RENDERTILE, 0, 0, dsdx, dsdy, false);
            }

            state->endSpriteCommand();

#       ifdef LOG_BGRECT_METHODS
            RT64_LOG_PRINTF("bgCopy::end(0x%08X)", (*dl)->w1);
#       endif
        }

        void readS2DStruct(State *state, uint32_t ptr, uint32_t loadSize) {
            // Convert the segmented obj pointer
            uint32_t rdramAddress = state->rsp->fromSegmentedMasked(ptr);
            // Mask the address as the RSP DMA hardware would
            rdramAddress &= RSP_DMA_MASK;
            // Truncate the load size as the ucode does
            loadSize &= 0xFF;
            // Load the struct from RDRAM into the S2D struct buffer
            memcpy(state->rsp->S2D.struct_buffer.data(), state->fromRDRAM(rdramAddress), loadSize);
        }

        void doLoadTxtr(State *state, const uObjTxtr *obj) {
            // Not sure what this does yet so it's just left as the original register name.
            // Maybe used for tracking what the most recent command was so it can insert syncs as needed?
            int32_t r1 = state->rsp->S2D.data_02AE;

            const uint16_t sid = uint16_t(uint8_t(obj->block.sid));
            // Must be divisible by 4 and no more than 12 to work correctly.
            // Technically the RSP does support unaligned reads/writes, so perfect simulation would require ditching the divisibility
            // requirement and handling those cases correctly. Handling values above 12 is basically impossible because it would
            // depend on dmem overrun.
            const bool validSid = ((sid <= S2DEX_STATUS_MAX_SID) && ((sid & S2DEX_STATUS_SID_ALIGNMENT_MASK) == 0));
            assert(validSid);
            if (!validSid) {
                return;
            }

            const uint32_t mask = obj->block.mask;
            const uint32_t flag = obj->block.flag;
            // Get the status for the given id.
            uint32_t status = state->rsp->S2D.statuses[sid / 4];

            // Skip the load if the masked status is equal to the provided flag.
            if ((status & mask) != flag) {
                // Update the status for the given id.
                state->rsp->S2D.statuses[sid / 4] = (status & ~mask) | (flag & mask);
                state->rsp->S2D.data_02AE = -127;

                if (r1 == -127) {
                    // RDP Tile Sync, not needed
                }

                RDP *rdp = state->rdp.get();
                const uint32_t imageAddress = state->rsp->fromSegmented(obj->block.image);
                switch (obj->block.type) {
                case S2DEX_TXTR_TYPE_LOADBLOCK: {
                    const uint16_t tsize = obj->block.tsize;
                    rdp->setTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, tsize + 1, imageAddress);
                    rdp->setTile(G_TX_LOADTILE, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, obj->block.tmem, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    if (r1 >= 0) {
                        // RDP Load Sync, not needed
                    }

                    rdp->loadBlock(G_TX_LOADTILE, 0, 0, uint16_t(obj->block.tsize << 2), obj->block.tline);
                    break;
                }
                case S2DEX_TXTR_TYPE_LOADTILE: {
                    const uint16_t twidth = obj->tile.twidth;
                    rdp->setTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, twidth + 1, imageAddress);
                    rdp->setTile(G_TX_LOADTILE, G_IM_FMT_RGBA, G_IM_SIZ_16b, uint16_t((twidth + 1) >> 2), obj->tile.tmem, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    if (r1 >= 0) {
                        // RDP Load Sync, not needed
                    }

                    rdp->loadTile(G_TX_LOADTILE, 0, 0, uint16_t(obj->tile.twidth << 2), obj->tile.theight);
                    break;
                }
                case S2DEX_TXTR_TYPE_LOADTLUT: {
                    rdp->setTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, imageAddress);
                    rdp->setTile(G_TX_LOADTILE, G_IM_FMT_RGBA, G_IM_SIZ_4b, 0, obj->tlut.phead, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    if (r1 >= 0) {
                        // RDP Load Sync, not needed
                    }

                    rdp->loadTLUT(G_TX_LOADTILE, 0, 0, uint16_t(obj->tlut.pnum << 2), 0);
                    break;
                }
                default:
                    assert(false && "Invalid sprite load command");
                    break;
                }
            }
        }

        void objLoadTxtr(State *state, DisplayList **dl) {
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxtr::start(0x%08X)", (*dl)->w1);
        #endif
            // Load the struct from rdram
            readS2DStruct(state, (*dl)->w1, ((*dl)->w0 & 0xFFFFFF) + 1);
            
            // Execute the texture load
            const uObjTxSprite *obj = reinterpret_cast<const uObjTxSprite *>(state->rsp->S2D.struct_buffer.data());
            doLoadTxtr(state, &obj->txtr);
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxtr::end(0x%08X)", (*dl)->w1);
        #endif
        }

        void objLoadTxSprite(State *state, DisplayList **dl) {
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxSprite::start(0x%08X)", (*dl)->w1);
        #endif
            // Load the struct from rdram
            readS2DStruct(state, (*dl)->w1, ((*dl)->w0 & 0xFFFFFF) + 1);
            
            // Execute the texture load
            const uObjTxSpriteFull* obj = reinterpret_cast<const uObjTxSpriteFull *>(state->rsp->S2D.struct_buffer.data());
            doLoadTxtr(state, &obj->txtr);
            drawObjRectLike(state, obj->sprite, true);
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxSprite::end(0x%08X)", (*dl)->w1);
        #endif
        }

        void objLoadTxRect(State *state, DisplayList **dl) {
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxRect::start(0x%08X)", (*dl)->w1);
        #endif
            // Load the struct from rdram
            readS2DStruct(state, (*dl)->w1, ((*dl)->w0 & 0xFFFFFF) + 1);
            
            // Execute the texture load
            const uObjTxSpriteFull* obj = reinterpret_cast<const uObjTxSpriteFull *>(state->rsp->S2D.struct_buffer.data());
            doLoadTxtr(state, &obj->txtr);
            drawObjRectLike(state, obj->sprite, false);
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxRect::end(0x%08X)", (*dl)->w1);
        #endif
        }

        void objLoadTxRectR(State *state, DisplayList **dl) {
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxRectR::start(0x%08X)", (*dl)->w1);
        #endif
            // Load the struct from rdram
            readS2DStruct(state, (*dl)->w1, ((*dl)->w0 & 0xFFFFFF) + 1);

            // Execute the texture load
            const uObjTxSpriteFull* obj = reinterpret_cast<const uObjTxSpriteFull *>(state->rsp->S2D.struct_buffer.data());
            doLoadTxtr(state, &obj->txtr);
            drawObjRectLike(state, obj->sprite, true);
        #ifdef LOG_LOADTXTR_METHODS
            RT64_LOG_PRINTF("objLoadTxRectR::end(0x%08X)", (*dl)->w1);
        #endif
        }

        void reset(State *state) {
            state->rsp->resetS2DState();
        }

        void setup(GBI *gbi) {
            gbi->constants = {
                { F3DENUM::G_MTX_MODELVIEW, 0x00 },
                { F3DENUM::G_MTX_PROJECTION, 0x01 },
                { F3DENUM::G_MTX_MUL, 0x00 },
                { F3DENUM::G_MTX_LOAD, 0x02 },
                { F3DENUM::G_MTX_NOPUSH, 0x00 },
                { F3DENUM::G_MTX_PUSH, 0x04 },
                { F3DENUM::G_TEXTURE_ENABLE, 0x00000002 },
                { F3DENUM::G_SHADING_SMOOTH, 0x00000200 },
                { F3DENUM::G_CULL_FRONT, 0x00001000 },
                { F3DENUM::G_CULL_BACK, 0x00002000 },
                { F3DENUM::G_CULL_BOTH, 0x00003000 }
            };

            gbi->map[F3D_G_SPNOOP] = &GBI_EXTENDED::noOpHook;
            gbi->map[S2DEX_G_OBJ_RECTANGLE] = &objRectangleOrMoveMem;
            gbi->map[S2DEX_G_OBJ_SPRITE] = &objSpriteOrVertex;
            gbi->map[S2DEX_G_OBJ_MOVEMEM] = &objMoveMem;
            gbi->map[F3D_G_SPRITE2D_BASE] = &sprite2DBase;
            gbi->map[F3D_G_CULLDL] = &sprite2DScaleFlip;
            gbi->map[F3D_G_POPMTX] = &sprite2DDraw;
            gbi->map[F3D_G_TEXTURE] = &GBI_F3D::texture;
            gbi->map[F3D_G_SETGEOMETRYMODE] = &GBI_F3D::setGeometryMode;
            gbi->map[F3D_G_CLEARGEOMETRYMODE] = &GBI_F3D::clearGeometryMode;
            gbi->map[S2DEX_G_OBJ_RENDERMODE] = &objRenderMode;
            gbi->map[S2DEX_G_OBJ_RECTANGLE_R] = &objRectangleR;
            gbi->map[S2DEX_G_BG_1CYC] = &bg1Cyc;
            gbi->map[S2DEX_G_BG_COPY] = &bgCopy;
            gbi->map[S2DEX_G_OBJ_LOADTXTR] = &objLoadTxtr;
            gbi->map[S2DEX_G_OBJ_LDTX_SPRITE] = &objLoadTxSprite;
            gbi->map[S2DEX_G_OBJ_LDTX_RECT] = &objLoadTxRect;
            gbi->map[S2DEX_G_OBJ_LDTX_RECT_R] = &objLoadTxRectR;
            gbi->map[F3D_G_DL] = &GBI_F3D::runDl;
            gbi->map[F3D_G_ENDDL] = &GBI_F3D::endDl;
            gbi->map[F3D_G_MOVEWORD] = &moveWord;
            gbi->map[F3D_G_SETOTHERMODE_H] = &GBI_F3D::setOtherModeH;
            gbi->map[F3D_G_SETOTHERMODE_L] = &GBI_F3D::setOtherModeL;
            gbi->map[S2DEX_G_RDPHALF_0] = &rdpHalf0;
            gbi->map[F3D_G_RDPHALF_1] = &GBI_F3D::rdpHalf1;
            gbi->map[F3D_G_RDPHALF_2] = &GBI_F3D::rdpHalf2;
            gbi->map[F3DEX_G_LOAD_UCODE] = &GBI_F3DEX::loadUCode;
            gbi->map[G_SETCIMG] = &GBI_F3D::setColorImage;
            gbi->map[G_SETZIMG] = &GBI_F3D::setDepthImage;
            gbi->map[G_SETTIMG] = &GBI_F3D::setTextureImage;

            gbi->resetFromTask = &reset;
        }
    }
};
