/*-------------------------------------------------------------------------
Loading regular
--------------------------------------------------------------------------*/

#define ARM9
#undef ARM7

#include <nds/system.h>
#include <nds/arm9/video.h>

#include "locations.h"
#include "common.h"
#include "loading.h"

bool arm9_macroMode = false;

void arm9_esrbScreen(void) {
	if (arm9_macroMode) {
		REG_POWERCNT &= ~BIT(15);
	}
	if (*(u32*)IMAGES_LOCATION == 0x494C4E4F) { // 'ONLI'
		dmaFill9(0, BG_GFX_SUB, 0xC000);
		dmaCopy((u16*)(IMAGES_LOCATION+0x18000), BG_PALETTE_SUB, 256*2);
		dmaCopy((u16*)(IMAGES_LOCATION+0x30200), BG_GFX_SUB+((256*78)/2), 0x2800);
		*(u32*)IMAGES_LOCATION = *(u32*)(IMAGES_LOCATION+4);
	}
	dmaCopy((u16*)IMAGES_LOCATION, VRAM_A, 0x18000);
}

void arm9_pleaseWaitText(void) {
	dmaCopy((u16*)(IMAGES_LOCATION+0x18000), BG_PALETTE_SUB, 256*2);
	dmaCopy((u16*)(IMAGES_LOCATION+0x18200), BG_GFX_SUB, 0xC000);
}

void arm9_errorText(void) {
	dmaCopy((u16*)(IMAGES_LOCATION+0x18000), BG_PALETTE_SUB, 256*2);
	dmaCopy((u16*)(IMAGES_LOCATION+0x24200), BG_GFX_SUB, 0xC000);
}
