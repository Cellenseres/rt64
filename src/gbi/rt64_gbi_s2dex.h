//
// RT64
//

#pragma once

#include "rt64_gbi.h"

#define S2DEX_G_BG_1CYC 0x01
#define S2DEX_G_BG_COPY 0x02
#define S2DEX_G_OBJ_RECTANGLE 0x03
#define S2DEX_G_OBJ_SPRITE 0x04
#define S2DEX_G_OBJ_MOVEMEM 0x05
#define S2DEX_G_RDPHALF_0 0xE4
#define S2DEX_G_SELECT_DL 0xB0
#define S2DEX_G_OBJ_LOADTXTR 0xC1
#define S2DEX_G_OBJ_LDTX_SPRITE 0xC2
#define S2DEX_G_OBJ_LDTX_RECT 0xC3
#define S2DEX_G_OBJ_LDTX_RECT_R 0xC4
#define S2DEX_G_OBJ_RENDERMODE 0xB1
#define S2DEX_G_OBJ_RECTANGLE_R 0xB2
#define S2DEX_MV_MATRIX 0x0000
#define S2DEX_MV_SUBMATRIX 0x0002
#define S2DEX_MV_VIEWPORT 0x0008
#define S2DEX_MV_MATRIX_SIZE 23
#define S2DEX_MV_SUBMATRIX_SIZE 7
#define S2DEX_MV_VIEWPORT_SIZE 15
#define S2DEX_TXTR_TYPE_LOADBLOCK 0x00001033
#define S2DEX_TXTR_TYPE_LOADTILE 0x00FC1034
#define S2DEX_TXTR_TYPE_LOADTLUT 0x00000030
#define S2DEX_STATUS_MAX_SID 12
#define S2DEX_STATUS_SID_ALIGNMENT_MASK 0x3
#define S2DEX_G_BGLT_LOADBLOCK 0x0033
#define S2DEX_G_BGLT_LOADTILE 0xFFF4
#define S2DEX_G_BG_FLAG_FLIPS 0x01
#define S2DEX_G_BG_FLAG_FLIPT 0x10
#define S2DEX_G_OBJRM_NOTXCLAMP 0x01
#define S2DEX_G_OBJRM_XLU 0x02
#define S2DEX_G_OBJRM_ANTIALIAS 0x04
#define S2DEX_G_OBJRM_BILERP 0x08
#define S2DEX_G_OBJRM_SHRINKSIZE_1 0x10
#define S2DEX_G_OBJRM_SHRINKSIZE_2 0x20
#define S2DEX_G_OBJRM_WIDEN 0x40

namespace RT64 {
    namespace GBI_S2DEX {
#pragma pack(push,1)
        struct uObjBg_t {
            uint16_t imageW;
            uint16_t imageX;
            uint16_t frameW;
            int16_t frameX;
            uint16_t imageH;
            uint16_t imageY;
            uint16_t frameH;
            int16_t frameY;
            uint32_t imageAddress;
            uint8_t imageSiz;
            uint8_t imageFmt;
            uint16_t imageLoad;
            uint16_t imageFlip;
            uint16_t imagePal;
            uint16_t tmemH;
            uint16_t tmemW;
            uint16_t tmemLoadTH;
            uint16_t tmemLoadSH;
            uint16_t tmemSize;
            uint16_t tmemSizeW;
        };

        struct uObjScaleBg_t {
            uint16_t imageW;
            uint16_t imageX;
            uint16_t frameW;
            int16_t frameX;
            uint16_t imageH;
            uint16_t imageY;
            uint16_t frameH;
            int16_t frameY;
            uint32_t imageAddress;
            uint8_t imageSiz;
            uint8_t imageFmt;
            uint16_t imageLoad;
            uint16_t imageFlip;
            uint16_t imagePal;
            uint16_t scaleH;
            uint16_t scaleW;
            int32_t imageYorig;
            uint8_t padding[4];
        };

        struct uObjBg {
            union {
                uObjBg_t bg;
                uObjScaleBg_t scaleBg;
            };
        };

        struct uObjTxtrBlock {
            uint32_t type;
            uint32_t image; // Segmented address
            uint16_t tsize;
            uint16_t tmem;
            uint16_t sid;
            uint16_t tline;
            uint32_t flag;
            uint32_t mask;
        };

        struct uObjTxtrTile {
            uint32_t type;
            uint32_t image; // Segmented address
            uint16_t twidth;
            uint16_t tmem;
            uint16_t sid;
            uint16_t theight;
            uint32_t flag;
            uint32_t mask;
        };

        struct uObjTxtrTLUT {
            uint32_t type;
            uint32_t image; // Segmented address
            uint16_t pnum;
            uint16_t phead;
            uint16_t sid;
            uint16_t zero;
            uint32_t flag;
            uint32_t mask;
        };

        union uObjTxtr {
            uObjTxtrBlock block;
            uObjTxtrTile tile;
            uObjTxtrTLUT tlut;
        };

        struct uObjTxSprite {
            uObjTxtr txtr;
        };

        struct uObjSprite {
            uint16_t scaleW;
            int16_t objX;
            uint16_t paddingX;
            uint16_t imageW;
            uint16_t scaleH;
            int16_t objY;
            uint16_t paddingY;
            uint16_t imageH;
            uint16_t imageAdrs;
            uint16_t imageStride;
            uint8_t imageFlags;
            uint8_t imagePal;
            uint8_t imageSiz;
            uint8_t imageFmt;
        };

        struct uObjTxSpriteFull {
            uObjTxtr txtr;
            uObjSprite sprite;
        };

        struct uObjMtx {
            int32_t A;
            int32_t B;
            int32_t C;
            int32_t D;
            int16_t Y;
            int16_t X;
            uint16_t baseScaleY;
            uint16_t baseScaleX;
        };

        struct uObjSubMtx {
            int16_t Y;
            int16_t X;
            uint16_t baseScaleY;
            uint16_t baseScaleX;
        };

#pragma pack(pop)

        inline bool isObjDma0Command(const DisplayList *dl) {
            return ((dl->w0 & 0x00FFFFFFU) == 0U);
        }

        inline bool isObjMoveMemCommand(const DisplayList *dl) {
            const uint8_t index = dl->p0(16, 8);
            const uint16_t size = uint16_t(dl->p0(0, 16));
            return (((index == S2DEX_MV_MATRIX) && (size == S2DEX_MV_MATRIX_SIZE))
                || ((index == S2DEX_MV_SUBMATRIX) && (size == S2DEX_MV_SUBMATRIX_SIZE))
                || ((index == S2DEX_MV_VIEWPORT) && (size == S2DEX_MV_VIEWPORT_SIZE)));
        }

        inline bool storeMoveWordStatus(State *state, const DisplayList *dl) {
            const uint8_t indexLo = dl->p0(0, 8);
            const uint8_t indexHi = dl->p0(16, 8);
            if ((indexLo != G_MW_GENSTAT) && (indexHi != G_MW_GENSTAT)) {
                return false;
            }

            const uint16_t sid = uint16_t(dl->p0(0, 16));
            if ((sid <= S2DEX_STATUS_MAX_SID) && ((sid & S2DEX_STATUS_SID_ALIGNMENT_MASK) == 0)) {
                state->rsp->S2D.statuses[sid >> 2] = dl->w1;
            }

            return true;
        }

        void objRenderMode(State *state, DisplayList **dl);
        void moveWord(State *state, DisplayList **dl);
        void objMoveMem(State *state, DisplayList **dl);
        void objRectangle(State *state, DisplayList **dl);
        void objRectangleR(State *state, DisplayList **dl);
        void objSprite(State *state, DisplayList **dl);
        void bg1Cyc(State *state, DisplayList **dl);
        void bgCopy(State *state, DisplayList **dl);
        void objLoadTxtr(State *state, DisplayList **dl);
        void objLoadTxSprite(State* state, DisplayList** dl);
        void objLoadTxRect(State* state, DisplayList** dl);
        void objLoadTxRectR(State* state, DisplayList** dl);
        void rdpHalf0(State *state, DisplayList **dl);

        void setup(GBI *gbi);
    };
};
