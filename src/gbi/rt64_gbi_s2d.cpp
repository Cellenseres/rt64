//
// RT64
//

#include "rt64_gbi_s2d.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#include "xxHash/xxh3.h"

#include "../include/rt64_extended_gbi.h"

#include "rt64_gbi_extended.h"
#include "rt64_gbi_f3d.h"
#include "rt64_gbi_f3dex.h"
#include "rt64_gbi_rdp.h"

namespace RT64 {
    namespace GBI_S2D {
#pragma pack(push, 1)
        struct uSprite {
            uint32_t sourceImage;
            uint32_t tlut;
            int16_t subImageWidth;
            int16_t stride;
            uint8_t sourceImageSiz;
            uint8_t sourceImageFmt;
            int16_t subImageHeight;
            int16_t sourceImageOffsetT;
            int16_t sourceImageOffsetS;
            uint8_t padding[4];
        };
#pragma pack(pop)

        constexpr uint8_t SharedSpriteFmt = G_IM_FMT_RGBA;
        constexpr uint8_t SharedSpriteSiz = G_IM_SIZ_32b;

        static int16_t clampS16(int32_t value) {
            constexpr int32_t MinS16 = std::numeric_limits<int16_t>::min();
            constexpr int32_t MaxS16 = std::numeric_limits<int16_t>::max();
            return int16_t(std::clamp(value, MinS16, MaxS16));
        }

        static uint8_t expand4To8(uint8_t value) {
            return uint8_t((value << 4) | value);
        }

        static uint8_t expand5To8(uint8_t value) {
            return uint8_t((value << 3) | (value >> 2));
        }

        static uint8_t readRDRAMByte(const State *state, uint32_t address) {
            return state->RDRAM[address ^ 3];
        }

        static uint16_t readRDRAMU16(const State *state, uint32_t address) {
            return uint16_t(uint16_t(readRDRAMByte(state, address)) << 8U) | readRDRAMByte(state, address + 1);
        }

        static uint32_t readRDRAMU32(const State *state, uint32_t address) {
            return (uint32_t(readRDRAMByte(state, address)) << 24U)
                | (uint32_t(readRDRAMByte(state, address + 1)) << 16U)
                | (uint32_t(readRDRAMByte(state, address + 2)) << 8U)
                | uint32_t(readRDRAMByte(state, address + 3));
        }

        static void writeGrayPixel(uint8_t *dst, uint8_t intensity, uint8_t alpha) {
            dst[0] = intensity;
            dst[1] = intensity;
            dst[2] = intensity;
            dst[3] = alpha;
        }

        static void writeRGBA16Pixel(uint8_t *dst, uint16_t rgba16) {
            dst[0] = expand5To8(uint8_t((rgba16 >> 11) & 0x1F));
            dst[1] = expand5To8(uint8_t((rgba16 >> 6) & 0x1F));
            dst[2] = expand5To8(uint8_t((rgba16 >> 1) & 0x1F));
            dst[3] = (rgba16 & 0x1) ? 0xFF : 0x00;
        }

        static void writeIA16Pixel(uint8_t *dst, uint16_t ia16) {
            writeGrayPixel(dst, uint8_t((ia16 >> 8) & 0xFF), uint8_t(ia16 & 0xFF));
        }

        static void writeRGBA32Pixel(uint8_t *dst, uint32_t rgba32) {
            dst[0] = uint8_t((rgba32 >> 24) & 0xFF);
            dst[1] = uint8_t((rgba32 >> 16) & 0xFF);
            dst[2] = uint8_t((rgba32 >> 8) & 0xFF);
            dst[3] = uint8_t(rgba32 & 0xFF);
        }

        static bool decodeSpriteSubImageRGBA32(State *state, const uSprite &sprite, int32_t width, int32_t height, int32_t stride, int32_t imageX, int32_t imageY, std::vector<uint8_t> &rgba32) {
            const uint8_t fmt = sprite.sourceImageFmt;
            const uint8_t siz = sprite.sourceImageSiz;
            const uint32_t imageAddress = state->rsp->fromSegmented(sprite.sourceImage);
            const bool usesTLUT = (sprite.tlut != 0) && (fmt == G_IM_FMT_CI);
            const uint32_t tlutAddress = usesTLUT ? state->rsp->fromSegmented(sprite.tlut) : 0;
            rgba32.resize(size_t(width) * size_t(height) * 4);

            for (int32_t y = 0; y < height; y++) {
                const int32_t srcY = imageY + y;
                for (int32_t x = 0; x < width; x++) {
                    const int32_t srcX = imageX + x;
                    uint8_t *dst = rgba32.data() + ((size_t(y) * size_t(width) + size_t(x)) * 4);

                    switch (fmt) {
                    case G_IM_FMT_RGBA:
                        if (siz == G_IM_SIZ_16b) {
                            const uint32_t address = imageAddress + uint32_t((srcY * stride + srcX) * 2);
                            writeRGBA16Pixel(dst, readRDRAMU16(state, address));
                        }
                        else if (siz == G_IM_SIZ_32b) {
                            const uint32_t address = imageAddress + uint32_t((srcY * stride + srcX) * 4);
                            writeRGBA32Pixel(dst, readRDRAMU32(state, address));
                        }
                        else {
                            return false;
                        }

                        break;
                    case G_IM_FMT_IA:
                        if (siz == G_IM_SIZ_4b) {
                            const uint32_t pixelIndex = uint32_t(srcY * stride + srcX);
                            const uint8_t byteValue = readRDRAMByte(state, imageAddress + (pixelIndex >> 1));
                            const uint8_t ia4 = ((pixelIndex & 1U) == 0) ? uint8_t(byteValue >> 4) : uint8_t(byteValue & 0xF);
                            uint8_t intensity = uint8_t(ia4 & 0xE);
                            intensity = uint8_t((intensity << 4) | (intensity << 1) | (intensity >> 2));
                            writeGrayPixel(dst, intensity, (ia4 & 0x1) ? 0xFF : 0x00);
                        }
                        else if (siz == G_IM_SIZ_8b) {
                            const uint32_t address = imageAddress + uint32_t(srcY * stride + srcX);
                            const uint8_t ia8 = readRDRAMByte(state, address);
                            writeGrayPixel(dst, expand4To8(uint8_t((ia8 >> 4) & 0xF)), expand4To8(uint8_t(ia8 & 0xF)));
                        }
                        else if (siz == G_IM_SIZ_16b) {
                            const uint32_t address = imageAddress + uint32_t((srcY * stride + srcX) * 2);
                            writeIA16Pixel(dst, readRDRAMU16(state, address));
                        }
                        else {
                            return false;
                        }

                        break;
                    case G_IM_FMT_I:
                        if (siz == G_IM_SIZ_4b) {
                            const uint32_t pixelIndex = uint32_t(srcY * stride + srcX);
                            const uint8_t byteValue = readRDRAMByte(state, imageAddress + (pixelIndex >> 1));
                            const uint8_t i4 = ((pixelIndex & 1U) == 0) ? uint8_t(byteValue >> 4) : uint8_t(byteValue & 0xF);
                            writeGrayPixel(dst, expand4To8(i4), expand4To8(i4));
                        }
                        else if (siz == G_IM_SIZ_8b) {
                            const uint32_t address = imageAddress + uint32_t(srcY * stride + srcX);
                            const uint8_t intensity = readRDRAMByte(state, address);
                            writeGrayPixel(dst, intensity, intensity);
                        }
                        else {
                            return false;
                        }

                        break;
                    case G_IM_FMT_CI:
                        if (!usesTLUT) {
                            return false;
                        }

                        if (siz == G_IM_SIZ_4b) {
                            const uint32_t pixelIndex = uint32_t(srcY * stride + srcX);
                            const uint8_t byteValue = readRDRAMByte(state, imageAddress + (pixelIndex >> 1));
                            const uint8_t colorIndex = ((pixelIndex & 1U) == 0) ? uint8_t(byteValue >> 4) : uint8_t(byteValue & 0xF);
                            writeRGBA16Pixel(dst, readRDRAMU16(state, tlutAddress + uint32_t(colorIndex * 2)));
                        }
                        else if (siz == G_IM_SIZ_8b) {
                            const uint32_t address = imageAddress + uint32_t(srcY * stride + srcX);
                            const uint8_t colorIndex = readRDRAMByte(state, address);
                            writeRGBA16Pixel(dst, readRDRAMU16(state, tlutAddress + uint32_t(colorIndex * 2)));
                        }
                        else {
                            return false;
                        }

                        break;
                    default:
                        return false;
                    }
                }
            }

            return true;
        }

        static bool uploadSharedSpriteTexture(State *state, const uSprite &sprite, int32_t width, int32_t height, int32_t stride, int32_t imageX, int32_t imageY, uint64_t &hash) {
            std::vector<uint8_t> rgba32;
            if (!decodeSpriteSubImageRGBA32(state, sprite, width, height, stride, imageX, imageY, rgba32)) {
                return false;
            }

            XXH3_state_t xxh3;
            XXH3_64bits_reset(&xxh3);
            constexpr char UploadTag[] = "RT64_S2D_SHARED_RGBA32_V1";
            XXH3_64bits_update(&xxh3, UploadTag, sizeof(UploadTag));
            XXH3_64bits_update(&xxh3, &sprite.sourceImageFmt, sizeof(sprite.sourceImageFmt));
            XXH3_64bits_update(&xxh3, &sprite.sourceImageSiz, sizeof(sprite.sourceImageSiz));
            XXH3_64bits_update(&xxh3, &width, sizeof(width));
            XXH3_64bits_update(&xxh3, &height, sizeof(height));
            XXH3_64bits_update(&xxh3, rgba32.data(), rgba32.size());
            hash = XXH3_64bits_digest(&xxh3);

            const int workloadCursor = state->ext.workloadQueue->writeCursor;
            const uint64_t creationFrame = state->ext.workloadQueue->workloads[workloadCursor].submissionFrame;
            state->textureManager.uploadRGBA32(state, state->ext.textureCache, creationFrame, hash, rgba32.data(), int(rgba32.size()), uint16_t(width), uint16_t(height), uint32_t(width * 4));
            return true;
        }

        static uint32_t spriteLineWords(uint8_t siz, uint32_t width) {
            switch (siz) {
            case G_IM_SIZ_4b:
                return std::max<uint32_t>((width + 15) / 16, 1);
            case G_IM_SIZ_8b:
                return std::max<uint32_t>((width + 7) / 8, 1);
            case G_IM_SIZ_16b:
                return std::max<uint32_t>((width + 3) / 4, 1);
            case G_IM_SIZ_32b:
                return std::max<uint32_t>((width + 1) / 2, 1);
            default:
                return 1;
            }
        }

        static uint32_t spriteTextureBytes(uint8_t siz, uint32_t width, uint32_t height) {
            switch (siz) {
            case G_IM_SIZ_4b:
                return (width * height + 1) / 2;
            case G_IM_SIZ_8b:
                return width * height;
            case G_IM_SIZ_16b:
                return width * height * 2;
            case G_IM_SIZ_32b:
                return width * height * 4;
            default:
                return 0;
            }
        }

        static uint32_t spriteTMEMDataBytes(bool usesTLUT) {
            return usesTLUT ? (RDP_TMEM_BYTES / 2) : RDP_TMEM_BYTES;
        }

        static uint32_t spriteMaxRowsForWidth(uint8_t siz, uint32_t width, uint32_t tmemBytes) {
            const uint32_t bytesPerRow = spriteLineWords(siz, width) * 8;
            return std::max<uint32_t>(tmemBytes / std::max<uint32_t>(bytesPerRow, 1), 1);
        }

        static uint32_t spriteTileWidth(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_4b:
                return 128;
            case G_IM_SIZ_8b:
            case G_IM_SIZ_16b:
                return 64;
            case G_IM_SIZ_32b:
                return 32;
            default:
                return 64;
            }
        }

        static uint32_t spriteTileHeight(uint8_t siz) {
            switch (siz) {
            case G_IM_SIZ_4b:
            case G_IM_SIZ_8b:
                return 64;
            case G_IM_SIZ_16b:
                return 32;
            case G_IM_SIZ_32b:
                return 16;
            default:
                return 32;
            }
        }

        static bool spriteNeedsYGuardRow(int32_t sy, int32_t ulyBase) {
            return (sy != 1024) || ((ulyBase & 0x3) != 0);
        }

        static bool spriteSupportsFullReplacementHash(uint8_t siz, int32_t width, int32_t height, int32_t stride, int32_t offsetS, int32_t offsetT) {
            if ((siz == G_IM_SIZ_4b) || (width <= 0) || (height <= 0)) {
                return false;
            }

            return (stride == width) && (offsetS == 0) && (offsetT == 0) && ((width * height) >= 16384);
        }

        static bool beginFullReplacementSprite(State *state, RDP *rdp, uint8_t fmt, uint8_t siz, uint32_t imageAddress,
            uint16_t width, uint16_t renderLine, uint16_t renderLrs, uint16_t renderLrt, uint64_t &replacementHash)
        {
            rdp->setTextureImage(fmt, siz, width, imageAddress);
            rdp->setTile(G_TX_LOADTILE, fmt, siz, renderLine, 0, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            rdp->loadTileReplacementCheck(G_TX_LOADTILE, 0, 0, renderLrs, renderLrt, siz, fmt, 0, 0, replacementHash);
            state->startSpriteCommand(replacementHash);
            return state->ext.textureCache->hasReplacement(replacementHash);
        }

        static int32_t spriteScreenSpan(int32_t texels, int32_t scale) {
            return int32_t((uint64_t(texels) * 4096ULL + uint32_t(scale) - 1) / uint32_t(scale));
        }

        static void loadSpriteTLUT(State *state, const uSprite &sprite) {
            RDP *rdp = state->rdp.get();
            const bool usesTLUT = (sprite.tlut != 0) && (sprite.sourceImageFmt != G_IM_FMT_RGBA);
            state->rsp->setOtherModeH(2, G_MDSFT_TEXTLUT, usesTLUT ? G_TT_RGBA16 : G_TT_NONE);
            if (!usesTLUT) {
                return;
            }

            const uint32_t tlutAddress = state->rsp->fromSegmented(sprite.tlut);
            const uint16_t paletteEntries = (sprite.sourceImageSiz == G_IM_SIZ_4b) ? 16 : 256;
            rdp->setTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, tlutAddress);
            rdp->setTile(G_TX_LOADTILE, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 256, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            rdp->loadTLUT(G_TX_LOADTILE, 0, 0, uint16_t((paletteEntries - 1) << 2), 0);
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
            const int32_t imageX = std::max<int32_t>(sprite->sourceImageOffsetS, 0);
            const int32_t imageY = std::max<int32_t>(sprite->sourceImageOffsetT, 0);
            const int32_t sx = std::max<int32_t>(state->rsp->S2D.sprite2D_scale_x, 1);
            const int32_t sy = std::max<int32_t>(state->rsp->S2D.sprite2D_scale_y, 1);
            const int32_t ulx = int16_t((*dl)->p1(16, 16));
            const int32_t uly = int16_t((*dl)->p1(0, 16));
            const bool flipX = (state->rsp->S2D.sprite2D_flip_x != 0);
            const bool flipY = (state->rsp->S2D.sprite2D_flip_y != 0);
            const bool needsYFilter = spriteNeedsYGuardRow(sy, uly);
            const bool usesTLUT = (sprite->tlut != 0) && (fmt != G_IM_FMT_RGBA);
            const uint32_t imageAddress = state->rsp->fromSegmented(sprite->sourceImage);
            const uint32_t textureBytes = spriteTextureBytes(siz, uint32_t(width), uint32_t(height));
            const uint32_t availableTMEM = spriteTMEMDataBytes(usesTLUT);
            const int32_t loadImageW = stride;
            const int32_t loadImageH = std::max<int32_t>(imageY + height, 1);

            const int32_t defaultLrx = ulx + spriteScreenSpan(width, sx);
            const int32_t totalLry = uly + spriteScreenSpan(height, sy);
            if ((defaultLrx <= ulx) || (totalLry <= uly)) {
                return;
            }

            int32_t effectiveLrx = defaultLrx;
            int16_t effectiveDsdx = clampS16(flipX ? -sx : sx);
            const int16_t dtdy = clampS16(flipY ? -sy : sy);
            if (!flipX) {
                const FixedRect &scissorRect = rdp->scissorRectStack[rdp->scissorStackSize - 1];
                const int32_t nativeWidth = rdp->colorImage.width * 4;
                const int32_t nativeHeight = 240 * 4;
                const bool depthSourcePrim = (rdp->otherMode.zSource() == G_ZS_PRIM);
                if (depthSourcePrim
                    && (ulx <= 0) && (defaultLrx >= nativeWidth) && (scissorRect.lrx > nativeWidth)
                    && (uly <= 0) && (totalLry >= nativeHeight))
                {
                    effectiveLrx = scissorRect.lrx;
                    const int32_t adjustedDsdx = std::max<int32_t>(1, int32_t((int64_t(sx) * defaultLrx) / scissorRect.lrx));
                    effectiveDsdx = clampS16(adjustedDsdx);
                }
            }

            const int32_t screenScaleX = std::max<int32_t>(std::abs(int32_t(effectiveDsdx)), 1);
            const bool spriteUpscalingEnabled = (state->ext.userConfig->spriteUpscaling == UserConfiguration::SpriteUpscaling::Upscaled);
            const bool previousDisableUpscale2D = rdp->extended.drawExtendedFlags.disableUpscale2D;
            bool currentDisableUpscale2D = previousDisableUpscale2D;
            auto applyDisableUpscale2D = [&](bool disable) {
                if (disable != currentDisableUpscale2D) {
                    rdp->disableUpscale2D(disable);
                    currentDisableUpscale2D = disable;
                }
            };

            applyDisableUpscale2D(!spriteUpscalingEnabled);

            const bool useFullReplacementHash = !usesTLUT && spriteSupportsFullReplacementHash(siz, width, height, stride, imageX, imageY);
            bool fullReplacementCommandActive = false;
            if (useFullReplacementHash) {
                const uint16_t renderLine = uint16_t(spriteLineWords(siz, uint32_t(width)));
                const uint16_t renderLrs = uint16_t((width - 1) << 2);
                const uint16_t renderLrt = uint16_t((height - 1) << 2);
                uint64_t replacementHash = 0;
                const bool hasReplacement = beginFullReplacementSprite(state, rdp, fmt, siz, imageAddress, uint16_t(width), renderLine, renderLrs, renderLrt, replacementHash);
                fullReplacementCommandActive = (replacementHash != 0);

                if (hasReplacement) {
                    const int16_t uls = flipX ? clampS16(width << 5) : 0;
                    const int16_t ult = flipY ? clampS16(height << 5) : 0;
                    rdp->setTile(G_TX_RENDERTILE, fmt, siz, renderLine, 0, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    rdp->setTileReplacementHash(G_TX_RENDERTILE, replacementHash);
                    rdp->setTileSize(G_TX_RENDERTILE, 0, 0, renderLrs, renderLrt);
                    rdp->drawTexRect(ulx, uly, effectiveLrx, totalLry, G_TX_RENDERTILE, uls, ult, effectiveDsdx, dtdy, false);
                    rdp->clearTileReplacementHash(G_TX_RENDERTILE);
                    applyDisableUpscale2D(previousDisableUpscale2D);
                    state->endSpriteCommand();
                    return;
                }
            }

            loadSpriteTLUT(state, *sprite);

            // Prefer row subdivision and only split in X when a single source row exceeds the TMEM budget.
            int32_t tileWidth = width;
            if ((spriteLineWords(siz, uint32_t(tileWidth)) * 8) > availableTMEM) {
                tileWidth = std::min<int32_t>(tileWidth, int32_t(spriteTileWidth(siz)));
                while ((tileWidth > 1) && ((spriteLineWords(siz, uint32_t(tileWidth)) * 8) > availableTMEM)) {
                    tileWidth = std::max(tileWidth / 2, 1);
                }
            }

            const int32_t maxLoadRows = std::max<int32_t>(int32_t(spriteMaxRowsForWidth(siz, uint32_t(tileWidth), availableTMEM)), 1);
            const int32_t guardBudget = needsYFilter ? 2 : 0;
            int32_t tileHeight = std::max<int32_t>(maxLoadRows - guardBudget, 1);
            if ((textureBytes > availableTMEM) && (tileWidth < width)) {
                tileHeight = std::min<int32_t>(tileHeight, std::max<int32_t>(int32_t(spriteTileHeight(siz)) - guardBudget, 1));
            }

            const bool subdividedSprite = (tileWidth < width) || (tileHeight < height);
            uint64_t sharedSpriteHash = 0;
            const bool sharedSpriteTexture = spriteUpscalingEnabled && subdividedSprite && uploadSharedSpriteTexture(state, *sprite, width, height, stride, imageX, imageY, sharedSpriteHash);
            const uint16_t sharedRenderLine = uint16_t(spriteLineWords(SharedSpriteSiz, uint32_t(width)));
            const uint16_t sharedRenderLrs = uint16_t((width - 1) << 2);
            const uint16_t sharedRenderLrt = uint16_t((height - 1) << 2);
            bool sharedSpriteCommandActive = false;
            if (sharedSpriteTexture) {
                state->startSpriteCommand(sharedSpriteHash);
                sharedSpriteCommandActive = true;
            }

            // Shared full-sprite uploads avoid per-rectangle upscale seams on subdivided sprites.
            const bool disableUpscale2D = !spriteUpscalingEnabled || (subdividedSprite && !sharedSpriteTexture);
            applyDisableUpscale2D(disableUpscale2D);

            for (int32_t screenY0 = 0; screenY0 < height; screenY0 += tileHeight) {
                const int32_t pieceH = std::min<int32_t>(tileHeight, height - screenY0);
                const int32_t screenY1 = screenY0 + pieceH;
                const int32_t sourceY0 = flipY ? (imageY + height - screenY1) : (imageY + screenY0);
                int32_t loadY0 = sourceY0;
                int32_t loadH = pieceH;
                int32_t visibleOffsetY = 0;
                if (needsYFilter) {
                    if ((loadY0 > 0) && (loadH < maxLoadRows)) {
                        loadY0--;
                        loadH++;
                        visibleOffsetY++;
                    }

                    if (((loadY0 + loadH) < loadImageH) && (loadH < maxLoadRows)) {
                        loadH++;
                    }
                }

                const int32_t pieceUly = uly + spriteScreenSpan(screenY0, sy);
                const int32_t pieceLry = (screenY1 >= height) ? totalLry : (uly + spriteScreenSpan(screenY1, sy));
                if (pieceLry <= pieceUly) {
                    continue;
                }

                for (int32_t screenX0 = 0; screenX0 < width; screenX0 += tileWidth) {
                    const int32_t pieceW = std::min<int32_t>(tileWidth, width - screenX0);
                    const int32_t screenX1 = screenX0 + pieceW;
                    const int32_t sourceX0 = flipX ? (imageX + width - screenX1) : (imageX + screenX0);
                    const uint16_t loadLine = uint16_t(spriteLineWords(siz, uint32_t(pieceW)));
                    const uint16_t loadLrs = uint16_t((pieceW - 1) << 2);
                    const uint16_t loadLrt = uint16_t((loadH - 1) << 2);

                    const int32_t pieceUlx = ulx + spriteScreenSpan(screenX0, screenScaleX);
                    const int32_t pieceLrx = (screenX1 >= width) ? effectiveLrx : (ulx + spriteScreenSpan(screenX1, screenScaleX));
                    if (pieceLrx <= pieceUlx) {
                        continue;
                    }

                    rdp->setTextureImage(fmt, siz, uint16_t(loadImageW), imageAddress);
                    rdp->setTile(G_TX_LOADTILE, fmt, siz, loadLine, 0, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                    rdp->loadTile(G_TX_LOADTILE, uint16_t(sourceX0 << 2), uint16_t(loadY0 << 2), uint16_t(((sourceX0 + pieceW) - 1) << 2), uint16_t(((loadY0 + loadH) - 1) << 2));

                    if (sharedSpriteTexture) {
                        rdp->setTile(G_TX_RENDERTILE, SharedSpriteFmt, SharedSpriteSiz, sharedRenderLine, 0, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                        rdp->setTileReplacementHash(G_TX_RENDERTILE, sharedSpriteHash);
                        rdp->setTileSize(G_TX_RENDERTILE, 0, 0, sharedRenderLrs, sharedRenderLrt);

                        const int16_t uls = flipX ? clampS16(screenX1 << 5) : clampS16(screenX0 << 5);
                        const int16_t ult = flipY ? clampS16(screenY1 << 5) : clampS16(screenY0 << 5);
                        rdp->drawTexRect(pieceUlx, pieceUly, pieceLrx, pieceLry, G_TX_RENDERTILE, uls, ult, effectiveDsdx, dtdy, false);
                        rdp->clearTileReplacementHash(G_TX_RENDERTILE);
                    }
                    else {
                        rdp->setTile(G_TX_RENDERTILE, fmt, siz, loadLine, 0, 0, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_CLAMP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
                        rdp->setTileSize(G_TX_RENDERTILE, 0, 0, loadLrs, loadLrt);

                        const int16_t uls = flipX ? clampS16(pieceW << 5) : 0;
                        const int16_t ult = flipY ? clampS16((visibleOffsetY + pieceH) << 5) : clampS16(visibleOffsetY << 5);
                        rdp->drawTexRect(pieceUlx, pieceUly, pieceLrx, pieceLry, G_TX_RENDERTILE, uls, ult, effectiveDsdx, dtdy, false);
                    }
                }
            }

            applyDisableUpscale2D(previousDisableUpscale2D);

            if (fullReplacementCommandActive || sharedSpriteCommandActive) {
                state->endSpriteCommand();
            }
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
            gbi->map[F3D_G_MTX] = &GBI_F3D::matrix;
            gbi->map[F3D_G_MOVEMEM] = &GBI_F3D::moveMem;
            gbi->map[F3D_G_VTX] = &GBI_F3D::vertex;
            gbi->map[F3D_G_DL] = &GBI_F3D::runDl;
            gbi->map[F3D_G_ENDDL] = &GBI_F3D::endDl;
            gbi->map[F3D_G_SPRITE2D_BASE] = &sprite2DBase;
            gbi->map[F3D_G_TRI1] = &GBI_F3D::tri1;
            gbi->map[F3D_G_QUAD] = &GBI_F3D::quad;
            gbi->map[F3D_G_CULLDL] = &sprite2DScaleFlip;
            gbi->map[F3D_G_POPMTX] = &sprite2DDraw;
            gbi->map[F3D_G_MOVEWORD] = &GBI_F3D::moveWord;
            gbi->map[F3D_G_TEXTURE] = &GBI_F3D::texture;
            gbi->map[F3D_G_SETOTHERMODE_H] = &GBI_F3D::setOtherModeH;
            gbi->map[F3D_G_SETOTHERMODE_L] = &GBI_F3D::setOtherModeL;
            gbi->map[F3D_G_SETGEOMETRYMODE] = &GBI_F3D::setGeometryMode;
            gbi->map[F3D_G_CLEARGEOMETRYMODE] = &GBI_F3D::clearGeometryMode;
            gbi->map[F3D_G_RDPHALF_1] = &GBI_F3D::rdpHalf1;
            gbi->map[F3D_G_RDPHALF_2] = &GBI_F3D::rdpHalf2;
            gbi->map[F3DEX_G_LOAD_UCODE] = &GBI_F3DEX::loadUCode;
            gbi->map[G_SETCIMG] = &GBI_F3D::setColorImage;
            gbi->map[G_SETZIMG] = &GBI_F3D::setDepthImage;
            gbi->map[G_SETTIMG] = &GBI_F3D::setTextureImage;
            gbi->map[G_RDPNOOP] = &GBI_RDP::noOp;

            gbi->resetFromTask = &reset;
        }
    };
};
