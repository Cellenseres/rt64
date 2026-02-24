//
// RT64
//

#pragma once

#include "rt64_gbi.h"

#define S2DEX2_G_OBJ_RENDERMODE 0x0B
#define S2DEX2_G_BG_1CYC 0x09
#define S2DEX2_G_BG_COPY 0x0A
#define S2DEX2_G_OBJ_RECTANGLE 0x01
#define S2DEX2_G_OBJ_SPRITE 0x02
#define S2DEX2_G_RDPHALF_0 0xE4
#define S2DEX2_G_SELECT_DL 0x04
#define S2DEX2_G_OBJ_LOADTXTR 0x05
#define S2DEX2_G_OBJ_LDTX_SPRITE 0x06
#define S2DEX2_G_OBJ_LDTX_RECT 0x07
#define S2DEX2_G_OBJ_LDTX_RECT_R 0x08
#define S2DEX2_G_OBJ_RECTANGLE_R 0xDA
#define S2DEX2_G_OBJ_MOVEMEM 0xDC


namespace RT64 {
    namespace GBI_S2DEX2 {
        void setup(GBI *gbi);
    };
};
