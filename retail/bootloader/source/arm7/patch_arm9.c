#include <string.h>
#include "nds_header.h"
#include "module_params.h"
#include "patch.h"
#include "find.h"
#include "virtual_slot2.h"
#include "load_crt0.h"
#include "common.h"
#include "cardengine_header_arm9.h"
#include "unpatched_funcs.h"
#include "debug_file.h"
#include "my_fat.h"
#include "tonccpy.h"
#include "value_bits.h"

#define FEATURE_SLOT_GBA			0x00000010
#define FEATURE_SLOT_NDS			0x00000020

//bool cardReadFound = false; // patch_common.c

bool isPawsAndClaws(const tNDSHeader* ndsHeader) {
	const char* romTid = getRomTid(ndsHeader);

	if (strncmp(romTid, "AQ2", 3) == 0 // Paws & Claws: Pet Resort
	 || strncmp(romTid, "YMU", 3) == 0 // Paws & Claws: Pet Vet 2
	) {
		return true;
	}

	return false;
}

u32 postCardReadCodeOffset = 0;

static bool patchCardRead(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams, bool* usesThumbPtr, int* readTypePtr, int* sdk5ReadTypePtr, u32** cardReadEndOffsetPtr, u32 startOffset) {
	bool usesThumb = patchOffsetCache.a9IsThumb;
	int readType = 0;
	int sdk5ReadType = 0; // SDK 5

	// Card read
	// SDK 5
	//dbg_printf("Trying SDK 5 thumb...\n");
    u32* cardReadEndOffset = patchOffsetCache.cardReadEndOffset;
	if (!patchOffsetCache.cardReadEndOffset) {
		cardReadEndOffset = (u32*)findCardReadEndOffsetThumb5Type1(ndsHeader, moduleParams, startOffset);
		if (cardReadEndOffset) {
			sdk5ReadType = 1;
			usesThumb = true;
			patchOffsetCache.a9IsThumb = usesThumb;
		}
		if (!cardReadEndOffset) {
			// SDK 5
			cardReadEndOffset = (u32*)findCardReadEndOffsetThumb5Type0(ndsHeader, moduleParams, startOffset);
			if (cardReadEndOffset) {
				sdk5ReadType = 0;
				usesThumb = true;
				patchOffsetCache.a9IsThumb = usesThumb;
			}
		}
		if (!cardReadEndOffset) {
			//dbg_printf("Trying thumb...\n");
			cardReadEndOffset = (u32*)findCardReadEndOffsetThumb(ndsHeader, startOffset);
			if (cardReadEndOffset) {
				usesThumb = true;
				patchOffsetCache.a9IsThumb = usesThumb;
			}
		}
		if (!cardReadEndOffset) {
			cardReadEndOffset = findCardReadEndOffsetType0(ndsHeader, moduleParams, startOffset);
		}
		if (!cardReadEndOffset) {
			//dbg_printf("Trying alt...\n");
			cardReadEndOffset = findCardReadEndOffsetType1(ndsHeader, moduleParams, startOffset);
			if (cardReadEndOffset) {
				readType = 1;
				if (*(cardReadEndOffset - 1) == 0xFFFFFE00) {
					dbg_printf("Found thumb\n\n");
					--cardReadEndOffset;
					usesThumb = true;
					patchOffsetCache.a9IsThumb = usesThumb;
				}
			}
		}
		if (cardReadEndOffset) {
			patchOffsetCache.cardReadEndOffset = cardReadEndOffset;
		}
	}
	*usesThumbPtr = usesThumb;
	*readTypePtr = readType;
	*sdk5ReadTypePtr = sdk5ReadType; // SDK 5
	*cardReadEndOffsetPtr = cardReadEndOffset;
	if (!cardReadEndOffset) { // Not necessarily needed
		return false;
	}
	u32* cardReadStartOffset = patchOffsetCache.cardReadStartOffset;
	if (!patchOffsetCache.cardReadStartOffset) {
		// SDK 5
		//dbg_printf("Trying SDK 5 thumb...\n");
		if (sdk5ReadType == 0) {
			cardReadStartOffset = (u32*)findCardReadStartOffsetThumb5Type0(moduleParams, (u16*)cardReadEndOffset);
		} else {
			cardReadStartOffset = (u32*)findCardReadStartOffsetThumb5Type1(moduleParams, (u16*)cardReadEndOffset);
		}
		if (!cardReadStartOffset) {
			//dbg_printf("Trying thumb...\n");
			cardReadStartOffset = (u32*)findCardReadStartOffsetThumb((u16*)cardReadEndOffset);
		}
		if (!cardReadStartOffset) {
			//dbg_printf("Trying SDK 5...\n");
			cardReadStartOffset = (u32*)findCardReadStartOffset5(moduleParams, cardReadEndOffset);
		}
		if (!cardReadStartOffset) {
			if (readType == 0) {
				cardReadStartOffset = findCardReadStartOffsetType0(moduleParams, cardReadEndOffset);
			} else {
				cardReadStartOffset = findCardReadStartOffsetType1(cardReadEndOffset);
			}
		}
		if (cardReadStartOffset) {
			patchOffsetCache.cardReadStartOffset = cardReadStartOffset;
		}
	}
	if (!cardReadStartOffset) {
		return false;
	}
	//cardReadFound = true;

	// Card struct
	u32** cardStruct = (u32**)(cardReadEndOffset - 1);
	u32* cardStructPatch = (usesThumb ? ce9->thumbPatches->cardStructArm9 : ce9->patches->cardStructArm9);
	if (moduleParams->sdk_version > 0x3000000) {
		// Save card struct
		ce9->cardStruct0 = (u32)(*cardStruct + 7);
		*cardStructPatch = (u32)(*cardStruct + 7); // Cache management alternative
	} else {
		// Save card struct
		ce9->cardStruct0 = (u32)(*cardStruct + 6);
		*cardStructPatch = (u32)(*cardStruct + 6); // Cache management alternative
	}

	// Patch
	const u32 newCardRead = (u32)ce9->patches->card_read_arm9;
	// tonccpy(cardReadStartOffset, cardReadPatch, usesThumb ? (isSdk5(moduleParams) ? 0xB0 : 0xA0) : 0xE0); // 0xE0 = 0xF0 - 0x08
	if (usesThumb) {
		u16* offsetThumb = (u16*)cardReadStartOffset;
		offsetThumb[0] = 0xB540; // push {r6, lr}
		offsetThumb[1] = 0x4E01; // ldr r6, =newCardRead
		offsetThumb[2] = 0x47B0; // blx r6
		offsetThumb[3] = 0xBD40; // pop {r6, pc}
		cardReadStartOffset[2] = newCardRead;
	} else {
		cardReadStartOffset[0] = 0xE51FF004; // ldr pc, =newCardRead
		cardReadStartOffset[1] = newCardRead;
	}
    dbg_printf("cardRead location : ");
    dbg_hexa((u32)cardReadStartOffset);
    dbg_printf("\n");
    dbg_hexa((u32)ce9);
    dbg_printf("\n");

	/* extern u32 apPatchPostCardReadFileCluster;
	extern u32 apPatchPostCardReadOffset;
	extern u32 apPatchPostCardReadSize;
	if (apPatchPostCardReadFileCluster == CLUSTER_FREE || apPatchPostCardReadOffset == 0 || apPatchPostCardReadSize == 0) {
		dbg_printf("\n");
		return true;
	}

	postCardReadCodeOffset = (u32)cardReadStartOffset;
	postCardReadCodeOffset += usesThumb ? 0xC : 8;

	aFile file;
	getFileFromCluster(&file, apPatchPostCardReadFileCluster);
	fileRead((char*)postCardReadCodeOffset, &file, apPatchPostCardReadOffset, apPatchPostCardReadSize);

	dbg_printf("Loaded post card read AP-fix\n\n"); */
	return true;
}

static bool patchCardReadMvDK4(u32 startOffset) {
	u32* offset = findCardReadCheckOffsetMvDK4(startOffset);
	if (!offset) {
		return false;
	}

	//offset[2] = 0xE3A00001; // mov r0, #1
	offset[3] = 0xE1A00000; // nop
	offset[4] += 0xD0000000; // bne to b

	dbg_printf("cardReadMvDK4 location : ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");
	return true;
}

static void patchCardSaveCmd(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	const char* romTid = getRomTid(ndsHeader);
	if (strncmp(romTid, "TAM", 3) != 0 // The Amazing Spider-Man
	 && strncmp(romTid, "AWD", 3) != 0 // Diddy Kong Racing
	 && strncmp(romTid, "BO5", 3) != 0 // Golden Sun: Dark Dawn
	 && strncmp(romTid, "AMM", 3) != 0 // Minna no Mahjong DS
	 && strncmp(romTid, "B8I", 3) != 0 // Spider-Man: Edge of Time
	// && strncmp(romTid, "CP3", 3) != 0 // Viva Pinata: Pocket Paradise
	) return;

	// Card save command
	u32* offset = patchOffsetCache.cardSaveCmdOffset;
	if (!patchOffsetCache.cardSaveCmdOffset) {
		if (isSdk5(moduleParams)) {
			offset = findCardSaveCmdOffset5(ndsHeader);
		} else if (moduleParams->sdk_version > 0x2020000) {
			offset = findCardSaveCmdOffset3(ndsHeader);
		} else {
			offset = findCardSaveCmdOffset2(ndsHeader);
		}
		if (offset) {
			patchOffsetCache.cardSaveCmdOffset = offset;
		}
	}

	if (!offset) {
		return;
	}
	// Patch
	if (isSdk5(moduleParams)) {
		// SDK 5
		if (*offset == 0xE92D4070) {
			if (strncmp(romTid, "BO5", 3) != 0) {
				tonccpy(offset, ce9->patches->card_save_arm9, 8);
			} else if (offset[1] == 0xE1A05001) {
				// offset[6] = 0xE1A00000; // nop
				// setBL((u32)offset+19*4, (u32)patchOffsetCache.cardReadStartOffset+8);
				ce9->cardStruct1 = offset[21]+0x4FC;
				offset[22] = (u32)ce9->patches->card_save_arm9;
			} else {
				// offset[10] = 0xE1A00000; // nop
				// setBL((u32)offset+23*4, (u32)patchOffsetCache.cardReadStartOffset+8);
				ce9->cardStruct1 = offset[25]+0x4FC;
				offset[26] = (u32)ce9->patches->card_save_arm9;
			}
		} else {
			tonccpy(offset, ce9->thumbPatches->card_save_arm9, 0xC);
			/* // u16* offsetThumb = (u16*)offset;
			// offsetThumb[12] = 0x46C0; // nop
			// offsetThumb[13] = 0x46C0; // nop
			// setBLThumb((u32)offset+27*2, (u32)patchOffsetCache.cardReadStartOffset+0xC);
			ce9->cardStruct1 = offset[15]+0x7C;
			offset[17] = (u32)ce9->patches->card_save_arm9; */
		}
		// ce9->cardSaveCmdPos = 7;
	} else if (moduleParams->sdk_version > 0x2020000) {
		// SDK 2.2 - 4
		// const u8 add = (moduleParams->sdk_version > 0x3000000) ? 0x1C : 0x18;
		// u16* offsetThumb = (u16*)offset;
		if (*offset == 0xE92D47F0 || *offset == 0xE92D43F8) {
			tonccpy(offset, ce9->patches->card_save_arm9, 8);
			/* setB((u32)offset+8*4, (u32)offset+27*4);
			offset[37] = 0xE1A00000; // nop
			offset[38] = 0xE1A00000; // nop
			setBL((u32)offset+40*4, (u32)patchOffsetCache.cardReadStartOffset+8); */
			// offset[41] = 0xE1A00000; // nop
			// ce9->cardStruct1 = offset[57]+add;
			// offset[59] = (u32)ce9->patches->card_save_arm9;
		} /* else if (*offset == 0xE92D43F8) {
			setB((u32)offset+8*4, (u32)offset+26*4);
			offset[36] = 0xE1A00000; // nop
			offset[37] = 0xE1A00000; // nop
			setBL((u32)offset+39*4, (u32)patchOffsetCache.cardReadStartOffset+8);
			// offset[40] = 0xE1A00000; // nop
			// ce9->cardStruct1 = offset[54]+add;
			// offset[56] = (u32)ce9->patches->card_save_arm9;
		} else if (offsetThumb[2] == 0x1C06) {
			// for (int i = 10; i <= 43; i++) {
			//	offsetThumb[i] = 0x46C0; // nop
			// }
			offsetThumb[55] = 0x46C0; // nop
			offsetThumb[56] = 0x46C0; // nop
			setBLThumb((u32)offset+58*2, (u32)patchOffsetCache.cardReadStartOffset+0xC);
			// offsetThumb[60] = 0x46C0; // nop
			ce9->cardStruct1 = offset[44]+add;
			// offset[46] = (u32)ce9->patches->card_save_arm9;
		} */ else {
			tonccpy(offset, ce9->thumbPatches->card_save_arm9, 0xC);
			// for (int i = 10; i <= 39; i++) {
			//	offsetThumb[i] = 0x46C0; // nop
			// }
			/* offsetThumb[53] = 0x46C0; // nop
			offsetThumb[54] = 0x46C0; // nop
			setBLThumb((u32)offset+56*2, (u32)patchOffsetCache.cardReadStartOffset+0xC);
			if (offsetThumb[58] == 0x2001) {
				// offsetThumb[58] = 0x46C0; // nop
				ce9->cardStruct1 = offset[42]+add;
				// offset[43] = (u32)ce9->patches->card_save_arm9;
			} else {
				// offsetThumb[59] = 0x46C0; // nop
				ce9->cardStruct1 = offset[41]+add;
				// offset[42] = (u32)ce9->patches->card_save_arm9;
			} */
		}
		// ce9->cardSaveCmdPos = (moduleParams->sdk_version > 0x3000000) ? 6 : 5;
	} else {
		// SDK 2
		if (*offset == 0xE92D47F0) {
			tonccpy(offset, ce9->patches->card_save_arm9, 8); // Write
			if (offset[1] == 0xE59F60BC) {
				/* setB((u32)offset+6*4, (u32)offset+23*4);
				offset[30] = 0xE1A00000; // nop
				offset[31] = 0xE1A00000; // nop
				setBL((u32)offset+33*4, (u32)patchOffsetCache.cardReadStartOffset+8);
				offset[34] = 0xE1A00000; // nop */

				u32* offset2 = (offset + 0xD4/4);
				if (*offset2 == 0xE92D47F0) {
					tonccpy(offset2, ce9->patches->card_save_arm9, 8); // Read
					/* setB((u32)offset2+6*4, (u32)offset2+23*4);
					offset2[30] = 0xE1A00000; // nop
					offset2[31] = 0xE1A00000; // nop
					setBL((u32)offset2+33*4, (u32)patchOffsetCache.cardReadStartOffset+8);
					offset2[34] = 0xE1A00000; // nop */
				}
			} else {
				/* setB((u32)offset+6*4, (u32)offset+22*4);
				offset[29] = 0xE1A00000; // nop
				offset[30] = 0xE1A00000; // nop
				setBL((u32)offset+32*4, (u32)patchOffsetCache.cardReadStartOffset+8);
				offset[33] = 0xE1A00000; // nop */

				u32* offset2 = (offset + 0xD0/4);
				if (*offset2 == 0xE92D47F0) {
					tonccpy(offset2, ce9->patches->card_save_arm9, 8); // Read
					/* setB((u32)offset2+6*4, (u32)offset2+22*4);
					offset2[29] = 0xE1A00000; // nop
					offset2[30] = 0xE1A00000; // nop
					setBL((u32)offset2+32*4, (u32)patchOffsetCache.cardReadStartOffset+8);
					offset2[33] = 0xE1A00000; // nop */
				}
			}
		} else if (*offset == 0xE92D000F) {
			setBL((u32)offset+49*4, (u32)ce9->patches->card_save_arm9); // Write
			u32* offset2 = (offset + 0xF4/4);
			if (*offset2 == 0xE92D000F) {
				setBL((u32)offset2+49*4, (u32)ce9->patches->card_save_arm9); // Read
			}
		} else {
			tonccpy(offset, ce9->thumbPatches->card_save_arm9, 0xC); // Write
			/* u16* offsetThumb = (u16*)offset;
			for (int i = 7; i <= 29; i++) {
				offsetThumb[i] = 0x46C0; // nop
			}
			offsetThumb[39] = 0x46C0; // nop
			offsetThumb[40] = 0x46C0; // nop
			setBLThumb((u32)offset+42*2, (u32)patchOffsetCache.cardReadStartOffset+0xC);
			offsetThumb[44] = 0x46C0; // nop */

			u32* offset2 = (offset + 0x9C/4);
			u16* offsetThumb2 = (u16*)offset2;
			if (*offsetThumb2 == 0xB5F0) {
				tonccpy(offset, ce9->thumbPatches->card_save_arm9, 0xC); // Read
				/* for (int i = 7; i <= 29; i++) {
					offsetThumb2[i] = 0x46C0; // nop
				}
				offsetThumb2[39] = 0x46C0; // nop
				offsetThumb2[40] = 0x46C0; // nop
				setBLThumb((u32)offset2+42*2, (u32)patchOffsetCache.cardReadStartOffset+0xC);
				offsetThumb2[44] = 0x46C0; // nop */
			}
		}
	}

	dbg_printf("cardSaveCmd location : ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");
}

static void patchCardPullOut(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams, bool usesThumb, int sdk5ReadType, u32** cardPullOutOffsetPtr) {
	// Card pull out
	u32* cardPullOutOffset = patchOffsetCache.cardPullOutOffset;
	if (!patchOffsetCache.cardPullOutOffset) {
		cardPullOutOffset = NULL;
		if (usesThumb) {
			//dbg_printf("Trying SDK 5 thumb...\n");
			if (sdk5ReadType == 0) {
				cardPullOutOffset = (u32*)findCardPullOutOffsetThumb5Type0(ndsHeader, moduleParams);
			} else {
				cardPullOutOffset = (u32*)findCardPullOutOffsetThumb5Type1(ndsHeader, moduleParams);
			}
			if (!cardPullOutOffset) {
				//dbg_printf("Trying thumb...\n");
				cardPullOutOffset = (u32*)findCardPullOutOffsetThumb(ndsHeader);
			}
		} else {
			cardPullOutOffset = findCardPullOutOffset(ndsHeader, moduleParams);
		}
		if (cardPullOutOffset) {
			patchOffsetCache.cardPullOutOffset = cardPullOutOffset;
		}
	}
	*cardPullOutOffsetPtr = cardPullOutOffset;
	if (!cardPullOutOffset) {
		return;
	}

	// Patch
	*cardPullOutOffset = usesThumb ? 0x47704770 : 0xE12FFF1E; // bx lr
    dbg_printf("cardPullOut location : ");
    dbg_hexa((u32)cardPullOutOffset);
    dbg_printf("\n\n");
}

static void patchCacheFlush(cardengineArm9* ce9, bool usesThumb, u32* cardPullOutOffset) {
	if (!cardPullOutOffset) {
		return;
	}

	// Patch
	u32* cacheFlushPatch = (usesThumb ? ce9->thumbPatches->cacheFlushRef : ce9->patches->cacheFlushRef);
	*cacheFlushPatch = (u32)cardPullOutOffset + 13;
}

/*static void patchForceToPowerOff(cardengineArm9* ce9, const tNDSHeader* ndsHeader, bool usesThumb) {
	// Force to power off
	u32* forceToPowerOffOffset = findForceToPowerOffOffset(ndsHeader);
	if (!forceToPowerOffOffset) {
		return;
	}
	// Patch
	u32* cardPullOutPatch = (usesThumb ? ce9->thumbPatches->card_pull : ce9->patches->card_pull);
	tonccpy(forceToPowerOffOffset, cardPullOutPatch, 0x4);
}*/

static bool patchCardId(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams, bool usesThumb, u32* cardReadEndOffset) {
	if (!isPawsAndClaws(ndsHeader) && !cardReadEndOffset) {
		return true;
	}

	// Card ID
	u32* cardIdStartOffset = patchOffsetCache.cardIdOffset;
	if (!patchOffsetCache.cardIdChecked) {
		cardIdStartOffset = NULL;
		u32* cardIdEndOffset = NULL;
		if (usesThumb) {
			cardIdEndOffset = (u32*)findCardIdEndOffsetThumb(ndsHeader, moduleParams, (u16*)cardReadEndOffset);
			cardIdStartOffset = (u32*)findCardIdStartOffsetThumb(moduleParams, (u16*)cardIdEndOffset);
		} else {
			cardIdEndOffset = findCardIdEndOffset(ndsHeader, moduleParams, cardReadEndOffset);
			cardIdStartOffset = findCardIdStartOffset(moduleParams, cardIdEndOffset);
		}
		if (cardIdStartOffset) {
			patchOffsetCache.cardIdOffset = cardIdStartOffset;
		}
		patchOffsetCache.cardIdChecked = true;
	}

	if (cardIdStartOffset) {
        // Patch
		extern u32 baseChipID;
		if (usesThumb) {
			cardIdStartOffset[0] = 0x47704800; // ldr r0, baseChipID + bx lr
			cardIdStartOffset[1] = baseChipID;
		} else {
			cardIdStartOffset[0] = 0xE59F0000; // ldr r0, baseChipID
			cardIdStartOffset[1] = 0xE12FFF1E; // bx lr
			cardIdStartOffset[2] = baseChipID;
		}
		dbg_printf("cardId location : ");
		dbg_hexa((u32)cardIdStartOffset);
		dbg_printf("\n\n");
	} else if (isSdk5(moduleParams)) {
		return false;
	}

	return true;
}

static void patchCardReadDma(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams, bool usesThumb) {
	// Card read dma
	u32* cardReadDmaStartOffset = patchOffsetCache.cardReadDmaOffset;
	if (!patchOffsetCache.cardReadDmaChecked) {
		cardReadDmaStartOffset = NULL;
		u32* cardReadDmaEndOffset = findCardReadDmaEndOffset(ndsHeader, moduleParams);
		if (!cardReadDmaEndOffset && usesThumb) {
			cardReadDmaEndOffset = (u32*)findCardReadDmaEndOffsetThumb(ndsHeader);
		}
		if (usesThumb) {
			cardReadDmaStartOffset = (u32*)findCardReadDmaStartOffsetThumb((u16*)cardReadDmaEndOffset);
		} else {
			cardReadDmaStartOffset = findCardReadDmaStartOffset(moduleParams, cardReadDmaEndOffset);
		}
		if (cardReadDmaStartOffset) {
			patchOffsetCache.cardReadDmaOffset = cardReadDmaStartOffset;
		}
		if (cardReadDmaEndOffset) {
			patchOffsetCache.cardReadDmaEndOffset = cardReadDmaEndOffset;
		}
		patchOffsetCache.cardReadDmaChecked = true;
	}
	if (!cardReadDmaStartOffset) {
		return;
	}
	// Patch
	const u32 newCardReadDma = (u32)ce9->patches->card_dma_arm9;
	if (strncmp(getRomTid(ndsHeader), "BO5", 3) == 0) {
		cardReadDmaStartOffset[0] = 0xE3A00000; // mov r0, #0
		cardReadDmaStartOffset[1] = 0xE12FFF1E; // bx lr
	} else if (usesThumb) {
		u16* offsetThumb = (u16*)cardReadDmaStartOffset;
		offsetThumb[0] = 0xB540; // push {r6, lr}
		offsetThumb[1] = 0x4E01; // ldr r6, =newCardReadDma
		offsetThumb[2] = 0x47B0; // blx r6
		offsetThumb[3] = 0xBD40; // pop {r6, pc}
		cardReadDmaStartOffset[2] = newCardReadDma;
	} else {
		cardReadDmaStartOffset[0] = 0xE51FF004; // ldr pc, =newCardReadDma
		cardReadDmaStartOffset[1] = newCardReadDma;
	}
    dbg_printf("cardReadDma location : ");
    dbg_hexa((u32)cardReadDmaStartOffset);
    dbg_printf("\n\n");
}

static bool dmaRequired(const tNDSHeader* ndsHeader) {
	const char* romTid = getRomTid(ndsHeader);

	return
	   (strncmp(romTid, "CAY", 3) == 0 // Army Men: Soldiers of Misfortune
	 || strncmp(romTid, "B5B", 3) == 0 // Call of Duty: Modern Warfare 3: Defiance
	 || strncmp(romTid, "B7F", 3) == 0 // The Magic School Bus: Oceans
	 || strncmp(romTid, "AYT", 3) == 0 // Tales of Innocence
	 || strncmp(romTid, "AH9", 3) == 0 // Tony Hawk's American Sk8land
	 || strncmp(romTid, "YUT", 3) == 0 // Ultimate Mortal Kombat
	);
}

static bool patchCardEndReadDma(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams, bool usesThumb) {
	/* const char* romTid = getRomTid(ndsHeader);

	if (strncmp(romTid, "AWD", 3) == 0 // Diddy Kong Racing
	 || strncmp(romTid, "CP3", 3) == 0 // Viva Pinata
	 || strncmp(romTid, "BO5", 3) == 0 // Golden Sun: Dark Dawn
	 || strncmp(romTid, "Y8L", 3) == 0 // Golden Sun: Dark Dawn (Demo Version)
	 || strncmp(romTid, "B8I", 3) == 0 // Spider-Man: Edge of Time
	 || strncmp(romTid, "TAM", 3) == 0 // The Amazing Spider-Man
	 || strncmp(romTid, "V2G", 3) == 0 // Mario vs. Donkey Kong: Mini-Land Mayhem
	 || !cardReadDMA) return false; */

	if (!dmaRequired(ndsHeader)) {
		return false;
	}

	u32* offset = patchOffsetCache.cardEndReadDmaOffset;
	u32 offsetDmaHandler = patchOffsetCache.dmaHandlerOffset;
	  if (!patchOffsetCache.cardEndReadDmaChecked) {
		if (!patchOffsetCache.cardReadDmaEndOffset) {
			u32* cardReadDmaEndOffset = findCardReadDmaEndOffset(ndsHeader, moduleParams);
			if (!cardReadDmaEndOffset && usesThumb) {
				cardReadDmaEndOffset = (u32*)findCardReadDmaEndOffsetThumb(ndsHeader);
			}
			if (cardReadDmaEndOffset) {
				patchOffsetCache.cardReadDmaEndOffset = cardReadDmaEndOffset;
			}
		}
		offset = findCardEndReadDma(ndsHeader,moduleParams,usesThumb,patchOffsetCache.cardReadDmaEndOffset,&offsetDmaHandler);
		if (offset) {
			patchOffsetCache.cardEndReadDmaOffset = offset;
			patchOffsetCache.dmaHandlerOffset = offsetDmaHandler;
		}
		patchOffsetCache.cardEndReadDmaChecked = true;
	  }
	if(offset) {
	  dbg_printf("\nDMA CARD READ METHOD ACTIVE\n");
	dbg_printf("cardEndReadDma location : ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");
	  if(!isSdk5(moduleParams)) {
		// SDK1-4
		if(usesThumb) {
			u16* thumbOffset = (u16*)offset;
			u16* thumbOffsetStartFunc = (u16*)offset;
			bool lrOnly = false;
			for (int i = 0; i <= 18; i++) {
				thumbOffsetStartFunc--;
				if (*thumbOffsetStartFunc==0xB5F0) {
					//dbg_printf("\nthumbOffsetStartFunc : ");
					//dbg_hexa(*thumbOffsetStartFunc);
					lrOnly = true;
					break;
				}
			}
			thumbOffset--;
			if (lrOnly) {
				thumbOffset--;
				thumbOffset[0] = *thumbOffsetStartFunc; // push	{r4-r7, lr}
				thumbOffset[1] = 0xB081; // SUB SP, SP, #4
			} else {
				*thumbOffset = 0xB5F8; // push	{r3-r7, lr}
			}
			ce9->thumbPatches->cardEndReadDmaRef = (u32*)thumbOffset;
		} else {
			u32* armOffset = (u32*)offset;
			u32* armOffsetStartFunc = (u32*)offset;
			bool lrOnly = false;
			for (int i = 0; i <= 16; i++) {
				armOffsetStartFunc--;
				if (*armOffsetStartFunc==0xE92D4000 || *armOffsetStartFunc==0xE92D4030 || *armOffsetStartFunc==0xE92D40F0) {
					//dbg_printf("\narmOffsetStartFunc : ");
					//dbg_hexa(*armOffsetStartFunc);
					lrOnly = true;
					break;
				}
			}
			armOffset--;
			if (lrOnly) {
				armOffset--;
				armOffset[0] = *armOffsetStartFunc; // "STMFD SP!, {LR}", "STMFD SP!, {R4,R5,LR}", or "STMFD SP!, {R4-R7,LR}"
				armOffset[1] = 0xE24DD004; // SUB SP, SP, #4
				if (offsetDmaHandler && *armOffsetStartFunc==0xE92D40F0) {
					offsetDmaHandler += 14*sizeof(u32); // Relocation fix
				}
			} else {
				*armOffset = 0xE92D40F8; // STMFD SP!, {R3-R7,LR}
			}
			ce9->patches->cardEndReadDmaRef = offsetDmaHandler==0 ? armOffset : (u32*)offsetDmaHandler;
		}
	  } else {
		// SDK5
		if(usesThumb) {
			u16* thumbOffset = (u16*)offset;
			while(*thumbOffset!=0xB508) { // push	{r3, lr}
				thumbOffset--;
			}
			ce9->thumbPatches->cardEndReadDmaRef = (u32*)thumbOffset;
			thumbOffset[1] = 0x46C0; // NOP
			thumbOffset[2] = 0x46C0; // NOP
			thumbOffset[3] = 0x46C0; // NOP
			thumbOffset[4] = 0x46C0; // NOP
		} else  {
			u32* armOffset = (u32*)offset;
			armOffset--;
			*armOffset = 0xE92D4008; // STMFD SP!, {R3,LR}
			ce9->patches->cardEndReadDmaRef = armOffset;
		}
	  }
	  return true;
	}
	return false;
}

static bool patchCardSetDma(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams, bool usesThumb) {
	/* const char* romTid = getRomTid(ndsHeader);

	if (strncmp(romTid, "AWD", 3) == 0 // Diddy Kong Racing
	 || strncmp(romTid, "CP3", 3) == 0 // Viva Pinata
	 || strncmp(romTid, "BO5", 3) == 0 // Golden Sun: Dark Dawn
	 || strncmp(romTid, "Y8L", 3) == 0 // Golden Sun: Dark Dawn (Demo Version)
	 || strncmp(romTid, "B8I", 3) == 0 // Spider-Man: Edge of Time
	 || strncmp(romTid, "TAM", 3) == 0 // The Amazing Spider-Man
	 || strncmp(romTid, "V2G", 3) == 0 // Mario vs. Donkey Kong: Mini-Land Mayhem
	 || !cardReadDMA) return false; */

	if (!dmaRequired(ndsHeader)) {
		return false;
	}

	dbg_printf("\npatchCardSetDma\n");

	u32* setDmaoffset = patchOffsetCache.cardSetDmaOffset;
	if (!patchOffsetCache.cardSetDmaChecked) {
		setDmaoffset = findCardSetDma(ndsHeader,moduleParams,usesThumb);
		if (setDmaoffset) {
			patchOffsetCache.cardSetDmaOffset = setDmaoffset;
		}
		patchOffsetCache.cardSetDmaChecked = true;
	}
	if(setDmaoffset) {
	  dbg_printf("\nDMA CARD SET METHOD ACTIVE\n");
	dbg_printf("cardSetDma location : ");
	dbg_hexa((u32)setDmaoffset);
	dbg_printf("\n\n");
	  const u32 newCardSetDma = (u32)ce9->patches->card_set_dma_arm9;
		if (usesThumb) {
			u16* offsetThumb = (u16*)setDmaoffset;
			offsetThumb[0] = 0xB540; // push {r6, lr}
			offsetThumb[1] = 0x4E01; // ldr r6, =newCardSetDma
			offsetThumb[2] = 0x47B0; // blx r6
			offsetThumb[3] = 0xBD40; // pop {r6, pc}
			setDmaoffset[2] = newCardSetDma;
		} else {
			setDmaoffset[0] = 0xE51FF004; // ldr pc, =newCardSetDma
			setDmaoffset[1] = newCardSetDma;
		}

	  return true;
	}

    return false;
}

static void patchReset(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	const u32 newReset = (u32)ce9->patches->reset_arm9;
	const char* romTid = getRomTid(ndsHeader);
	if (ndsHeader->unitCode == 0 && (strcmp(romTid, "NTRJ") == 0 || strncmp(romTid, "HND", 3) == 0 || strncmp(romTid, "HNE", 3) == 0)) {
		u32* offset = patchOffsetCache.srlStartOffset9;

		if (!patchOffsetCache.srlStartOffsetChecked) {
			offset = findSrlStartOffset9(ndsHeader);
			if (offset) patchOffsetCache.srlStartOffset9 = offset;
			patchOffsetCache.srlStartOffsetChecked = true;
		}

		if (offset) {
			// Patch
			offset[0] = 0xE59F1000; // ldr r1, =newReset
			offset[1] = 0xE59FF000; // ldr pc, =reset_arm9
			offset[2] = 0x4C525344; // 'DSRL'
			offset[3] = newReset;
			dbg_printf("srlStart location : ");
			dbg_hexa((u32)offset);
			dbg_printf("\n\n");
		}
	}

	u32* offset = patchOffsetCache.resetOffset;

    if (!patchOffsetCache.resetChecked) {
		offset = findResetOffset(ndsHeader, moduleParams, (bool)patchOffsetCache.srlStartOffset9);
		if (offset) patchOffsetCache.resetOffset = offset;
		patchOffsetCache.resetChecked = true;
	}

	if (!offset) {
		return;
	}

	// Patch
	offset[0] = 0xE51FF004; // ldr pc, =newReset
	offset[1] = newReset;
	dbg_printf("reset location : ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");
}

static void patchResetTwl(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	extern u32 accessControl;
	if (!(accessControl & BIT(4)) && strncmp(ndsHeader->gameCode, "DMF", 3) != 0) {
		return;
	}

	u32* nandTmpJumpFuncOffset = patchOffsetCache.nandTmpJumpFuncOffset;

    if (!patchOffsetCache.nandTmpJumpFuncChecked) {
		nandTmpJumpFuncOffset = findNandTmpJumpFuncOffset(ndsHeader, moduleParams);
		if (nandTmpJumpFuncOffset) patchOffsetCache.nandTmpJumpFuncOffset = nandTmpJumpFuncOffset;
		patchOffsetCache.nandTmpJumpFuncChecked = true;
	}

	if (!nandTmpJumpFuncOffset) {
		return;
	}

	// Patch
	const u32 newReset = (u32)ce9->patches->reset_arm9;
	if (moduleParams->sdk_version < 0x5008000) {
		nandTmpJumpFuncOffset[0] = 0xE59F0000; // ldr r0, =0xFFFFFFFF
		nandTmpJumpFuncOffset[1] = 0xE59FF000; // ldr pc, =newReset
		nandTmpJumpFuncOffset[2] = 0xFFFFFFFF;
		nandTmpJumpFuncOffset[3] = newReset;
	} else if (nandTmpJumpFuncOffset[-3] == 0xE8BD8008 && nandTmpJumpFuncOffset[-1] == 0x02FFE230) { // DEBUG
		nandTmpJumpFuncOffset[-15] = 0xE3A00000; // mov r0, #0
		nandTmpJumpFuncOffset[-14] = 0xE51FF004; // ldr pc, =newReset
		nandTmpJumpFuncOffset[-13] = newReset;
		dbg_printf("Reset (TWL) patched!\n");
	} else if (nandTmpJumpFuncOffset[-2] == 0xE8BD8008 && nandTmpJumpFuncOffset[-1] == 0x02FFE230) {
		nandTmpJumpFuncOffset[-11] = 0xE3A00000; // mov r0, #0
		nandTmpJumpFuncOffset[-10] = 0xE51FF004; // ldr pc, =newReset
		nandTmpJumpFuncOffset[-9] = newReset;
		dbg_printf("Reset (TWL) patched!\n");
		if (nandTmpJumpFuncOffset[-13] == 0xE12FFF1C) {
			nandTmpJumpFuncOffset[-17] = 0xE59F0000; // ldr r0, =0xFFFFFFFF
			nandTmpJumpFuncOffset[-16] = 0xE59FF000; // ldr pc, =newReset
			nandTmpJumpFuncOffset[-15] = 0xFFFFFFFF;
			nandTmpJumpFuncOffset[-14] = newReset;
			dbg_printf("Exit-to-menu patched!\n");
		}
	} else if (nandTmpJumpFuncOffset[-2] == 0xE12FFF1C) {
		nandTmpJumpFuncOffset[-6] = 0xE59F0000; // ldr r0, =0xFFFFFFFF
		nandTmpJumpFuncOffset[-5] = 0xE59FF000; // ldr pc, =newReset
		nandTmpJumpFuncOffset[-4] = 0xFFFFFFFF;
		nandTmpJumpFuncOffset[-3] = newReset;
		dbg_printf("Exit-to-menu patched!\n");
	} else if (nandTmpJumpFuncOffset[-15] == 0xE8BD8008 && nandTmpJumpFuncOffset[-14] == 0x02FFE230) {
		nandTmpJumpFuncOffset[-24] = 0xE3A00000; // mov r0, #0
		nandTmpJumpFuncOffset[-23] = 0xE51FF004; // ldr pc, =newReset
		nandTmpJumpFuncOffset[-22] = newReset;
		dbg_printf("Reset (TWL) patched!\n");
		if (nandTmpJumpFuncOffset[-26] == 0xE12FFF1C) {
			nandTmpJumpFuncOffset[-20] = 0xE59F0000; // ldr r0, =0xFFFFFFFF
			nandTmpJumpFuncOffset[-19] = 0xE59FF000; // ldr pc, =newReset
			nandTmpJumpFuncOffset[-18] = 0xFFFFFFFF;
			nandTmpJumpFuncOffset[-17] = newReset;
			dbg_printf("Exit-to-menu patched!\n");
		}
	} else if (nandTmpJumpFuncOffset[-15] == 0xE12FFF1C) {
		nandTmpJumpFuncOffset[-19] = 0xE59F0000; // ldr r0, =0xFFFFFFFF
		nandTmpJumpFuncOffset[-18] = 0xE59FF000; // ldr pc, =newReset
		nandTmpJumpFuncOffset[-17] = 0xFFFFFFFF;
		nandTmpJumpFuncOffset[-16] = newReset;
		dbg_printf("Exit-to-menu patched!\n");
	}
	dbg_printf("nandTmpJumpFunc location : ");
	dbg_hexa((u32)nandTmpJumpFuncOffset);
	dbg_printf("\n\n");
}

bool patchedCardIrqEnable = false;

static bool patchCardIrqEnable(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	const char* romTid = getRomTid(ndsHeader);

	if (strncmp(romTid, "AWD", 3) == 0 // Diddy Kong Racing - Fix corrupted 3D model bug
	 || strncmp(romTid, "CP3", 3) == 0 // Viva Pinata - Fix touch and model rendering bug
	 || strncmp(romTid, "BO5", 3) == 0 // Golden Sun: Dark Dawn - Fix black screen on boot
	 || strncmp(romTid, "Y8L", 3) == 0 // Golden Sun: Dark Dawn (Demo Version) - Fix black screen on boot
	 || strncmp(romTid, "B8I", 3) == 0 // Spider-Man: Edge of Time - Fix white screen on boot
	 || strncmp(romTid, "TAM", 3) == 0 // The Amazing Spider-Man - Fix white screen on boot
	) {
		return true;
	}

	bool usesThumb = patchOffsetCache.a9CardIrqIsThumb;

	// Card irq enable
	u32* cardIrqEnableOffset = patchOffsetCache.a9CardIrqEnableOffset;
	if (!patchOffsetCache.a9CardIrqEnableOffset) {
		cardIrqEnableOffset = a9FindCardIrqEnableOffset(ndsHeader, moduleParams, &usesThumb);
		if (cardIrqEnableOffset) {
			patchOffsetCache.a9CardIrqEnableOffset = cardIrqEnableOffset;
			patchOffsetCache.a9CardIrqIsThumb = usesThumb;
		}
	}
	if (!cardIrqEnableOffset) {
		return false;
	}
	const u32 newCardIrqEnable = (u32)ce9->patches->card_irq_enable;
	if (usesThumb) {
		u16* offsetThumb = (u16*)cardIrqEnableOffset;
		offsetThumb[0] = 0xB540; // push {r6, lr}
		offsetThumb[1] = 0x4E01; // ldr r6, =newCardIrqEnable
		offsetThumb[2] = 0x47B0; // blx r6
		offsetThumb[3] = 0xBD40; // pop {r6, pc}
		cardIrqEnableOffset[2] = newCardIrqEnable;
	} else {
		cardIrqEnableOffset[0] = 0xE51FF004; // ldr pc, =newCardIrqEnable
		cardIrqEnableOffset[1] = newCardIrqEnable;
	}
    dbg_printf("cardIrqEnable location : ");
    dbg_hexa((u32)cardIrqEnableOffset);
    dbg_printf("\n\n");
	patchedCardIrqEnable = true;
	return true;
}

static void patchMpu(const tNDSHeader* ndsHeader, const module_params_t* moduleParams, u32 patchMpuRegion) {
	if (!extendedMemory || patchMpuRegion == 2 || ndsHeader->unitCode > 0) {
		return;
	}

	if (patchOffsetCache.patchMpuRegion != patchMpuRegion) {
		patchOffsetCache.patchMpuRegion = 0;
		patchOffsetCache.mpuStartOffset = 0;
		patchOffsetCache.mpuDataOffset = 0;
		patchOffsetCache.mpuInitOffset = 0;
	}

	unpatchedFunctions* unpatchedFuncs = (unpatchedFunctions*)UNPATCHED_FUNCTION_LOCATION;

	// Find the mpu init
	u32* mpuStartOffset = patchOffsetCache.mpuStartOffset;
	u32* mpuDataOffset = patchOffsetCache.mpuDataOffset;
	if (!patchOffsetCache.mpuStartOffset) {
		mpuStartOffset = findMpuStartOffset(ndsHeader, patchMpuRegion);
	}
	if (mpuStartOffset) {
		dbg_printf("Mpu start: ");
		dbg_hexa((u32)mpuStartOffset);
		dbg_printf("\n\n");
	}
	if (!patchOffsetCache.mpuDataOffset) {
		mpuDataOffset = findMpuDataOffset(moduleParams, patchMpuRegion, mpuStartOffset);
	}
	if (mpuDataOffset) {
		if (!isSdk5(moduleParams)) {
			// Change the region 1 configuration

			u32 mpuInitRegionNewData = PAGE_32M | 0x02000000 | 1;
			u32 mpuNewDataAccess     = 0;
			u32 mpuNewInstrAccess    = 0;
			int mpuAccessOffset      = 0;
			switch (patchMpuRegion) {
				case 0:
					mpuInitRegionNewData = PAGE_128M | 0x00000000 | 1;
					break;
				case 3:
					mpuInitRegionNewData = PAGE_32M | 0x08000000 | 1;
					mpuNewInstrAccess    = 0x5111111;
					mpuAccessOffset      = 5;
					break;
			}

			unpatchedFuncs->mpuDataOffset = mpuDataOffset;
			unpatchedFuncs->mpuInitRegionOldData = *mpuDataOffset;
			*mpuDataOffset = mpuInitRegionNewData;

			if (mpuAccessOffset) {
				unpatchedFuncs->mpuAccessOffset = mpuAccessOffset;
				if (mpuNewInstrAccess) {
					unpatchedFuncs->mpuOldInstrAccess = mpuDataOffset[mpuAccessOffset];
					mpuDataOffset[mpuAccessOffset] = mpuNewInstrAccess;
				}
				if (mpuNewDataAccess) {
					unpatchedFuncs->mpuOldDataAccess = mpuDataOffset[mpuAccessOffset + 1];
					mpuDataOffset[mpuAccessOffset + 1] = mpuNewDataAccess;
				}
			}
		}

		dbg_printf("Mpu data: ");
		dbg_hexa((u32)mpuDataOffset);
		dbg_printf("\n\n");
	}

	// Patch out all further mpu reconfiguration
	const u32* mpuInitRegionSignature = getMpuInitRegionSignature(patchMpuRegion);
	u32* mpuInitOffset = patchOffsetCache.mpuInitOffset;
	if (!mpuInitOffset) {
		mpuInitOffset = (u32*)mpuStartOffset;
	}
	extern u32 iUncompressedSize;
	u32 patchSize = iUncompressedSize;
	if (mpuInitOffset == mpuStartOffset) {
		mpuInitOffset = findOffset(
			//(u32*)((u32)mpuStartOffset + 4), patchSize,
			mpuInitOffset + 1, patchSize,
			mpuInitRegionSignature, 1
		);
	}
	if (mpuInitOffset) {
		dbg_printf("Mpu init: ");
		dbg_hexa((u32)mpuInitOffset);
		dbg_printf("\n\n");

		*mpuInitOffset = 0xE1A00000; // nop

		// Try to find it
		/*for (int i = 0; i < 0x100; i++) {
			mpuDataOffset += i;
			if ((*mpuDataOffset & 0xFFFFFF00) == 0x02000000) {
				*mpuDataOffset = PAGE_32M | 0x02000000 | 1;
				break;
			}
			if (i == 100) {
				*mpuStartOffset = 0xE1A00000;
			}
		}*/
	}

	if (!isSdk5(moduleParams) && unpatchedFuncs->mpuInitRegionOldData != 0x200002B) {
		u32* mpuDataOffsetAlt = patchOffsetCache.mpuDataOffsetAlt;
		if (!patchOffsetCache.mpuDataOffsetAlt) {
			mpuDataOffsetAlt = findMpuDataOffsetAlt(ndsHeader);
			if (mpuDataOffsetAlt) {
				patchOffsetCache.mpuDataOffsetAlt = mpuDataOffsetAlt;
			}
		}
		if (mpuDataOffsetAlt) {
			unpatchedFuncs->mpuDataOffsetAlt = mpuDataOffsetAlt;
			unpatchedFuncs->mpuInitRegionOldDataAlt = *mpuDataOffsetAlt;

			*mpuDataOffsetAlt = PAGE_32M | 0x02000000 | 1;

			dbg_printf("Mpu data alt: ");
			dbg_hexa((u32)mpuDataOffsetAlt);
			dbg_printf("\n\n");
		}
	}

	patchOffsetCache.patchMpuRegion = patchMpuRegion;
	patchOffsetCache.mpuStartOffset = mpuStartOffset;
	patchOffsetCache.mpuDataOffset = mpuDataOffset;
	patchOffsetCache.mpuInitOffset = mpuInitOffset;
}

static void patchMpu2(const tNDSHeader* ndsHeader, const module_params_t* moduleParams, const bool usesCloneboot) {
	if (((moduleParams->sdk_version < 0x2008000) && !extendedMemory) || ndsHeader->unitCode > 0) {
		return;
	}

	unpatchedFunctions* unpatchedFuncs = (unpatchedFunctions*)UNPATCHED_FUNCTION_LOCATION;

	// Find the mpu init
	u32* mpuStartOffset = patchOffsetCache.mpuStartOffset2;
	if (mpuStartOffset) {
		dbg_printf("Mpu start 2: ");
		dbg_hexa((u32)mpuStartOffset);
		dbg_printf("\n\n");
	}
	u32* mpuDataOffset = patchOffsetCache.mpuDataOffset2;
	if (!patchOffsetCache.mpuStartOffset2) {
		mpuStartOffset = findMpuStartOffset(ndsHeader, 2);
	}
	if (!patchOffsetCache.mpuDataOffset2) {
		mpuDataOffset = findMpuDataOffset(moduleParams, 2, mpuStartOffset);
	}
	if (mpuDataOffset) {
		if (moduleParams->sdk_version < 0x5000000) {
			// Change the region 2 configuration

			/*u32 mpuInitRegionNewData = PAGE_32M | 0x02000000 | 1;
			u32 mpuNewDataAccess = 0x15111111;
			u32 mpuNewInstrAccess = 0x5111111;
			int mpuAccessOffset = 6;

			*mpuDataOffset = mpuInitRegionNewData;

			if (mpuNewInstrAccess) {
				mpuDataOffset[mpuAccessOffset] = mpuNewInstrAccess;
			}
			if (mpuNewDataAccess) {
				mpuDataOffset[mpuAccessOffset + 1] = mpuNewDataAccess;
			}*/
			unpatchedFuncs->mpuDataOffset2 = mpuDataOffset;
			unpatchedFuncs->mpuInitRegionOldData2 = *mpuDataOffset;
			*mpuDataOffset = 0;
		}

		dbg_printf("Mpu data 2: ");
		dbg_hexa((u32)mpuDataOffset);
		dbg_printf("\n\n");
	}

	// Patch out all further mpu reconfiguration
	const u32* mpuInitRegionSignature = getMpuInitRegionSignature(2);
	u32* mpuInitOffset = patchOffsetCache.mpuInitOffset2;
	if (!mpuInitOffset) {
		mpuInitOffset = (u32*)mpuStartOffset;
	}
	extern u32 iUncompressedSize;
	u32 patchSize = iUncompressedSize;
	if (mpuInitOffset == mpuStartOffset) {
		mpuInitOffset = findOffset(
			//(u32*)((u32)mpuStartOffset + 4), patchSize,
			mpuInitOffset + 1, patchSize,
			mpuInitRegionSignature, 1
		);
	}
	if (mpuInitOffset) {
		dbg_printf("Mpu init 2: ");
		dbg_hexa((u32)mpuInitOffset);
		dbg_printf("\n\n");

		u32 mpuInitOffsetInSrl = (u32)mpuInitOffset;
		mpuInitOffsetInSrl -= (u32)ndsHeader->arm9destination;

		if (mpuInitOffsetInSrl >= 0 && mpuInitOffsetInSrl < 0x4000 && usesCloneboot) {
			unpatchedFuncs->mpuInitOffset2 = mpuInitOffset;
		}
		*mpuInitOffset = 0xE1A00000; // nop

		// Try to find it
		/*for (int i = 0; i < 0x100; i++) {
			mpuDataOffset += i;
			if ((*mpuDataOffset & 0xFFFFFF00) == 0x02000000) {
				*mpuDataOffset = PAGE_32M | 0x02000000 | 1;
				break;
			}
			if (i == 100) {
				*mpuStartOffset = 0xE1A00000;
			}
		}*/
	}

	patchOffsetCache.mpuStartOffset2 = mpuStartOffset;
	patchOffsetCache.mpuDataOffset2 = mpuDataOffset;
	patchOffsetCache.mpuInitOffset2 = mpuInitOffset;
}

void patchSlot2IoBlock(const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	extern u32 _io_dldi_features;
	extern u32 arm7mbk;
	if ((_io_dldi_features & FEATURE_SLOT_NDS) || moduleParams->sdk_version < 0x5000000 || arm7mbk == 0x080037C0) {
		return;
	}

	u32* offset = patchOffsetCache.mpuDataOffset2;
	if (!patchOffsetCache.mpuDataOffset2) {
		offset = findMpuFlagsSetOffset(ndsHeader);
		if (offset) {
			patchOffsetCache.mpuDataOffset2 = offset;
		}
	}

	if (!offset) {
		return;
	}

	// Disable MPU cache and write buffer for the Slot-2 region
	offset[0] = 0xE3A00042; // mov r0, #0x42
	offset[2] = 0xE3A00042; // mov r0, #0x42
	offset[4] = 0xE3A00002; // mov r0, #0x02

	dbg_printf("Mpu flags set: ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");

	offset = patchOffsetCache.mpuCodeCacheChangeOffset;
	if (!patchOffsetCache.mpuCodeCacheChangeOffset) {
		offset = findMpuCodeCacheChangeOffset(ndsHeader);
		if (offset) {
			patchOffsetCache.mpuCodeCacheChangeOffset = offset;
		}
	}

	if (!offset) {
		return;
	}

	// Disable MPU cache and write buffer changes for the Slot-2 region
	offset[0] = 0xE12FFF1E; // bx lr
	offset[4] = 0xE12FFF1E; // bx lr
	offset[8] = 0xE12FFF1E; // bx lr
	offset[12] = 0xE12FFF1E; // bx lr
	offset[16] = 0xE12FFF1E; // bx lr
	offset[21] = 0xE12FFF1E; // bx lr
	offset[25] = 0xE12FFF1E; // bx lr

	dbg_printf("Mpu cache change: ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");
}

void patchMpuChange(const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	extern u32 arm7mbk;
	if (!extendedMemory || moduleParams->sdk_version < 0x5000000 || arm7mbk == 0x080037C0) {
		return;
	}

	u32* offset = patchOffsetCache.mpuChangeOffset;
	if (!patchOffsetCache.mpuChangeOffset) {
		offset = findMpuChange(ndsHeader);
		if (offset) {
			patchOffsetCache.mpuChangeOffset = offset;
		}
	}

	if (!offset) {
		return;
	}

	if (offset[0] == 0xE3A00001 || offset[0] == 0x03A0202C) {
		offset[3] = 0xE1A00000; // nop
	} else {
		u16* thumbOffset = (u16*)offset;
		thumbOffset[3] = 0x46C0; // nop
		thumbOffset[4] = 0x46C0; // nop
	}

	dbg_printf("Mpu change: ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");
}

/*u32* patchLoHeapPointer(const module_params_t* moduleParams, const tNDSHeader* ndsHeader, u32 saveSize) {
	if (moduleParams->sdk_version < 0x2008000) {
		return 0;
	}

	u32* heapPointer = NULL;
	if (patchOffsetCache.ver != patchOffsetCacheFileVersion
	 || patchOffsetCache.type != 1) {
		patchOffsetCache.heapPointerOffset = 0;
	} else {
		heapPointer = patchOffsetCache.heapPointerOffset;
	}
	if (!patchOffsetCache.heapPointerOffset) {
		heapPointer = findHeapPointerOffset(moduleParams, ndsHeader);
	}
    if(!heapPointer || *heapPointer<0x02000000 || *heapPointer>0x03000000) {
        dbg_printf("ERROR: Wrong lo heap pointer\n");
        dbg_printf("heap pointer value: ");
	    dbg_hexa(*heapPointer);
		dbg_printf("\n\n");
        return 0;
    } else if (!patchOffsetCache.heapPointerOffset) {
		patchOffsetCache.heapPointerOffset = heapPointer;
	}

    u32* oldheapPointer = (u32*)*heapPointer;

    dbg_printf("old lo heap pointer: ");
	dbg_hexa((u32)oldheapPointer);
    dbg_printf("\n\n");

	u32 shrinkSize = 0;
	switch (ndsHeader->deviceSize) {
		case 0x09:
		default:
			shrinkSize = 0x2000;	// 0x4000000
			break;
		case 0x0A:
			shrinkSize = 0x4000;	// 0x8000000
			break;
	}

	*heapPointer += shrinkSize; // shrink heap by FAT table cache size

    dbg_printf("new lo heap pointer: ");
	dbg_hexa((u32)*heapPointer);
    dbg_printf("\n\n");
    dbg_printf("Lo Heap Shrink Sucessfull\n\n");

    return oldheapPointer;
}*/

void patchHiHeapPointer(cardengineArm9* ce9, const module_params_t* moduleParams, const tNDSHeader* ndsHeader) {
	extern u8 _io_dldi_size;
	extern bool ce9Alt;
	extern bool ce9AltLargeTable;
	extern u32 arm7mbk;
	extern u32 accessControl;
	extern u32 fatTableAddr;
	const char* romTid = getRomTid(ndsHeader);

	extern u32 cheatSize;
	extern u32 apPatchSize;
	const u32 cheatSizeTotal = cheatSize+(apPatchIsCheat ? apPatchSize : 0);

	const bool nandAccess = (accessControl & BIT(4)); // isDSiWare
	const bool laterSdk = ((moduleParams->sdk_version >= 0x2008000 && moduleParams->sdk_version != 0x2012774) || moduleParams->sdk_version == 0x20029A8);
	const bool ce9NotInHeap = (ce9Alt || (u32)ce9 == CARDENGINE_ARM9_LOCATION_DLDI_START);

	if ((!nandAccess && extendedMemory)
	|| !laterSdk
	|| (ce9NotInHeap && !ce9AltLargeTable && cheatSizeTotal <= 4 && _io_dldi_size < 0x0E)
	|| (((strncmp(romTid, "YEE", 3) == 0 && romTid[3] != 'J') || strncmp(romTid, "BEB", 3) == 0 || strncmp(romTid, "BEE", 3) == 0) && !ce9AltLargeTable && _io_dldi_size < 0x0E) // Inazuma Eleven 1 & 2
	|| strncmp(romTid, "CLJ", 3) == 0 // Mario & Luigi: Bowser's Inside Story
	|| strncmp(romTid, "VSO", 3) == 0 // Sonic Classic Collection
	|| arm7mbk == 0x080037C0) {
		return;
	}

	u32* heapPointer = patchOffsetCache.heapPointer2Offset;
	if (!patchOffsetCache.heapPointer2Offset) {
		heapPointer = findHeapPointer2Offset(moduleParams, ndsHeader);
	}
	if(!heapPointer || *heapPointer<0x02000000 || *heapPointer>0x03000000) {
        dbg_printf("ERROR: Wrong heap pointer\n");
        dbg_printf("heap pointer value: ");
	    dbg_hexa(*heapPointer);
		dbg_printf("\n\n");
        return;
    } else if (!patchOffsetCache.heapPointer2Offset) {
		patchOffsetCache.heapPointer2Offset = heapPointer;
	}

    dbg_printf("hi heap end: ");
	dbg_hexa((u32)heapPointer);
    dbg_printf("\n\n");

    u32* oldheapPointer = (u32*)*heapPointer;

    dbg_printf("old hi heap value: ");
	dbg_hexa((u32)oldheapPointer);
    dbg_printf("\n\n");

	if (nandAccess && extendedMemory) {
		*heapPointer = 0x027D8000;
	} else if (ce9NotInHeap && !ce9AltLargeTable) {
		*heapPointer = CHEAT_ENGINE_LOCATION_B4DS-0x400000;
		if ((u32)ce9 == CARDENGINE_ARM9_LOCATION_DLDI_32) {
			*heapPointer = (u32)ce9;
		} else if (_io_dldi_size == 0x0E) {
			*heapPointer = 0x023DC000;
		} else if (_io_dldi_size == 0x0F) {
			*heapPointer = 0x023D8000;
		}
	} else {
		*heapPointer = (fatTableAddr < 0x023C0000 || fatTableAddr >= (u32)ce9) ? (u32)ce9 : fatTableAddr; // shrink heap by FAT table size + ce9 binary size
	}

    dbg_printf("new hi heap value: ");
	dbg_hexa((u32)*heapPointer);
    dbg_printf("\n\n");
    dbg_printf(!(nandAccess && extendedMemory) ? "Hi Heap Shrink Successful\n\n" : "Hi Heap Grow Successful\n\n");
}

void patchKeyInputs(const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	const u32 signature = 0x04000130;
	const u32 signatureHeader = isSdk5(moduleParams) ? 0x02FFFFA8 : 0x027FFFA8;
	const u32 newOffset = isSdk5(moduleParams) ? 0x02FFFF78 : 0x027FFF78;
	extern u32 iUncompressedSize;

	u32* offset = ndsHeader->arm9destination;
	for (int i = 0; i < iUncompressedSize/4; i++) {
		if (*offset == signature) {
			if (offset[-2] == signatureHeader || offset[-1] == signatureHeader || offset[1] == signatureHeader || offset[2] == signatureHeader) {
				*offset = newOffset;

				dbg_printf("KEYINPUT found: ");
				dbg_hexa((u32)offset);
				dbg_printf("\n");
			}
		}
		offset++;
	}
	dbg_printf("\n");

	*(u16*)newOffset = 0x3FF;
}

u32 relocateBssPart(const tNDSHeader* ndsHeader, u32 bssEnd, u32 bssPartStart, u32 bssPartEnd, u32 newPartStart) {
	extern u32 iUncompressedSize;

	u32 subtract = bssPartEnd-bssPartStart;
	if (newPartStart < bssPartStart) {
		subtract = bssPartStart-newPartStart;
	}

	u32* addr = (u32*)ndsHeader->arm9destination;
	for (u32 i = 0; i < iUncompressedSize/4; i++) {
		if (addr[i] >= bssPartStart && addr[i] < bssPartEnd) {
			addr[i] -= bssPartStart;
			addr[i] += newPartStart;
		} else if (bssPartEnd != bssEnd && addr[i] >= bssPartEnd && addr[i] < bssEnd) {
			addr[i] -= subtract;
		}
	}

	return subtract;
}

u32 shrinkBss(const tNDSHeader* ndsHeader, u32 bssEnd, u32 bssPartStart, u32 newPartStart) {
	return relocateBssPart(ndsHeader, bssEnd, bssPartStart, bssEnd, newPartStart);
}

void patchHiHeapDSiWare(u32 addr, u32 heapEnd) {
	//*(u32*)(addr) = opCode; // mov r0, #0x????????
	if (*(u32*)(addr+4) == 0xEA000043) { // debuggerSdk
		*(u32*)(addr) = 0xE59F0134; // ldr r0, =0x????????
		*(u32*)(addr+0x28) = 0xE3560001; // cmp r6, #1
		*(u32*)(addr+0x30) = 0xE3A00627; // mov r0, #0x2700000

		// Convert ldr to mov instruction
		if (*(u32*)(addr+0x13C) != 0) {
			if (*(u8*)(addr+0x13C) != 0) {
				*(u32*)(addr+0x58) = 0xE3A00D00; // mov r0, #*(u32*)(addr+0x13C)
				for (u32 i = 0; i < *(u32*)(addr+0x13C); i += 0x40) {
					*(u32*)(addr+0x58) += 1;
				}
			} else {
				*(u32*)(addr+0x58) = 0xE3A00C00; // mov r0, #*(u32*)(addr+0x13C)
				for (u32 i = 0; i < *(u32*)(addr+0x13C); i += 0x100) {
					*(u32*)(addr+0x58) += 1;
				}
			}
		} else {
			*(u32*)(addr+0x58) = 0xE3A00000; // mov r0, #*(u32*)(addr+0x13C)
		}

		*(u32*)(addr+0x13C) = heapEnd;
		return;
	}

	*(u32*)(addr) = 0xE59F0094; // ldr r0, =0x????????
	*(u32*)(addr+0x24) = 0xE3500001; // cmp r0, #1
	*(u32*)(addr+0x2C) = 0x13A00627; // movne r0, #0x2700000

	// Convert ldr to mov instruction
	if (*(u32*)(addr+0x9C) != 0) {
		if (*(u8*)(addr+0x9C) != 0) {
			*(u32*)(addr+0x40) = 0xE3A01D00; // mov r1, #*(u32*)(addr+0x9C)
			for (u32 i = 0; i < *(u32*)(addr+0x9C); i += 0x40) {
				*(u32*)(addr+0x40) += 1;
			}
		} else {
			*(u32*)(addr+0x40) = 0xE3A01C00; // mov r1, #*(u32*)(addr+0x9C)
			for (u32 i = 0; i < *(u32*)(addr+0x9C); i += 0x100) {
				*(u32*)(addr+0x40) += 1;
			}
		}
	} else {
		*(u32*)(addr+0x40) = 0xE3A01000; // mov r1, #*(u32*)(addr+0x9C)
	}

	/*if (*(u32*)(addr+0x9C) == 0) {
		*(u32*)(addr+0x40) = 0xE3A01000; // mov r1, #0
	} else if (*(u32*)(addr+0x9C) == 0x800) {
		*(u32*)(addr+0x40) = 0xE3A01B02; // mov r1, #0x800
	}*/

	*(u32*)(addr+0x9C) = heapEnd;
}

void patchHiHeapDSiWareThumb(u32 addr, u32 newCodeAddr, u32 heapEnd) {
	*(u16*)(newCodeAddr) = 0x4800; // movs r0, #0x????????
	*(u16*)(newCodeAddr+2) = 0x4770; // bx lr
	*(u32*)(newCodeAddr+4) = heapEnd;

	setBLThumb(addr, newCodeAddr);

	*(u16*)(addr+0x1A) = 0x2801; // cmp r0, #1
	*(u16*)(addr+0x22) = 0x2027; // movne r0, #0x2700000
}

void patchUserSettingsReadDSiWare(u32 addr) {
	if (*(u16*)(addr) == 0x4806) { // ldr r0, =0x2FFFDFC (THUMB)
		*(u16*)(addr) = 0x1C28; // movs r0, r5
		*(u16*)(addr+4) = 0x3054; // adds r0, #0x54
		*(u16*)(addr+6) = 0x3464; // adds r4, #0x64
		*(u16*)(addr+8) = 0x7001; // strb r1, [r0]
		*(u16*)(addr+0xA) = 0x8820; // ldrh r0, [r4]
		*(u16*)(addr+0xC) = 0x0740; // lsls r0, r0, #0x1D
		*(u16*)(addr+0xE) = 0x0F40; // lsrs r0, r0, #0x1D
		// *(u16*)(addr+0x10) = 0x7028; // strb r0, [r5]
		// *(u16*)(addr+0x12) = 0xBD38; // pop {r3-r5, pc}
	} else if (*(u16*)(addr) == 0x4805) { // ldr r0, =0x2FFFDFC (THUMB)
		*(u16*)(addr) = 0x2100; // movs r1, #0
		*(u16*)(addr+2) = 0x3054; // adds r0, #0x54
		*(u16*)(addr+4) = 0x7001; // strb r1, [r0]
		*(u16*)(addr+6) = 0x4803; // ldr r0, =0x2FFFC80
		*(u16*)(addr+8) = 0x3064; // adds r0, #0x64
		*(u16*)(addr+0xA) = 0x8800; // ldrh r0, [r0]
		*(u16*)(addr+0xC) = 0x0740; // lsls r0, r0, #0x1D
		*(u16*)(addr+0xE) = 0x0F40; // lsrs r0, r0, #0x1D
		*(u16*)(addr+0x10) = 0x7020; // strb r0, [r4]
		*(u16*)(addr+0x12) = 0xBD10; // pop {r4, pc}
	} else if (*(u32*)(addr) == 0xE594117C) { // ldr r1, [r4,#0x17C]
		*(u32*)(addr) = 0xE5C50054; // strb r0, [r5,#0x54]
		*(u32*)(addr+4) = 0xE1D406B4; // ldrh r0, [r4,#0x64]
		*(u32*)(addr+8) = 0xE1A00E80; // mov r0, r0,lsl#29
		*(u32*)(addr+0xC) = 0xE1A00EA0; // mov r0, r0,lsr#29
	} else {
		*(u32*)(addr) -= 4; // ldr r0, =0x2FFFC80
		*(u32*)(addr+8) = 0xE5C41054; // strb r1, [r4,#0x54]
		*(u32*)(addr+0xC) = 0xE1D006B4; // ldrh r0, [r0,#0x64]
		*(u32*)(addr+0x10) = 0xE1A00E80; // mov r0, r0,lsl#29
		*(u32*)(addr+0x14) = 0xE1A00EA0; // mov r0, r0,lsr#29
	}
}

void patchInitDSiWare(u32 addr, u32 heapEnd) {
	bool debuggerSdk = false;
	u32* func = getOffsetFromBL((u32*)(addr+0x10));
	if (func[8] == 0xE92D4010) { // STMFD SP!, {R4,LR}
		debuggerSdk = true;
		func[6] = 0xE1A00000; // nop
		patchHiHeapDSiWare(((u32)func+0x300), heapEnd);
	} else {
		bool heapInitPatched = false;
		for (int i = 0; i < 64; i++) {
			if (func[i] == 0xE1A00100 && !heapInitPatched) {
				func[i-2] = 0xE1A00000; // nop
				i += 16;
				heapInitPatched = true;
			}

			if (func[i] == 0xE3A007BE) {
				patchHiHeapDSiWare(((u32)func+(i*sizeof(u32))), heapEnd);
				break;
			}
		}
	}

	func = getOffsetFromBL((u32*)(addr+0x20));
	func[3] = 0xE1A00000; // nop

	if (debuggerSdk) {
		*(u32*)(addr+0x3C) = 0xE1A00000; // nop
		*(u32*)(addr+0x40) = 0xE1A00000; // nop
		*(u32*)(addr+0x4C) = 0xE1A00000; // nop
	} else {
		*(u32*)(addr+0x8C) = 0xE1A00000; // nop
		*(u32*)(addr+0x90) = 0xE1A00000; // nop
		*(u32*)(addr+0x9C) = 0xE1A00000; // nop
	}
}

void patchVolumeGetDSiWare(u32 addr) {
	*(u32*)(addr) = 0xE59F0004; // ldr r0, =0x02FFFDF0
	*(u32*)(addr+4) = 0xE5900000; // ldr r0, [r0]
	*(u32*)(addr+8) = 0xE12FFF1E; // bx lr
	*(u32*)(addr+0xC) = 0x02FFFDF0;
}

void patchTwlSleepMode(const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	extern u32 arm7mbk;
	if (arm7mbk != 0x080037C0 || moduleParams->sdk_version < 0x5010000) {
		return;
	}

	u32* offset = patchOffsetCache.twlSleepModeEndOffset;
	if (!patchOffsetCache.twlSleepModeEndChecked) {
		offset = findTwlSleepModeEndOffset(ndsHeader);
		if (offset) {
			patchOffsetCache.twlSleepModeEndOffset = offset;
		}
		patchOffsetCache.twlSleepModeEndChecked = true;
	}

	if (!offset) {
		return;
	}

	dbg_printf("twlSleepModeEnd location : ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");

	if (*offset == 0xE2855001 || *offset == 0xE2866001) {
		offset--;
		*offset = 0xE1A00000; // nop
	} else {
		u16* offsetThumb = (u16*)offset;
		offsetThumb--;
		offsetThumb--;
		offsetThumb[0] = 0x46C0; // nop
		offsetThumb[1] = 0x46C0; // nop
	}
}

void patchInitLock(const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	u32* offset = patchOffsetCache.initLockEndOffset;
	if (!patchOffsetCache.initLockEndOffset) {
		offset = findInitLockEndOffset(ndsHeader);
		if (offset) {
			patchOffsetCache.initLockEndOffset = offset;
		}
	}

	if (!offset) {
		return;
	}

	dbg_printf("initLockEnd location : ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");

	if (offset[1] == 0x02FFFFB4) { // Debug
		u32* newBranchOffset2 = (u32*)((u32)offset + 0x160);

		for (int i = 0; i < 0x100/sizeof(u32); i++) {
			newBranchOffset2++;
			if (newBranchOffset2[0] == 0xE92D000F && newBranchOffset2[1] == 0xE92D4008) {
				break;
			}
		}

		u32* newBranchOffset1 = (u32*)((u32)newBranchOffset2 + 0x80);

		for (int i = 0; i < 0x100/sizeof(u32); i++) {
			newBranchOffset1++;
			if (newBranchOffset1[0] == 0xE92D000F && newBranchOffset1[1] == 0xE92D4008) {
				break;
			}
		}

		if (*newBranchOffset1 == 0xE92D000F) {
			dbg_printf("newBranch location 1 : ");
			dbg_hexa((u32)newBranchOffset1);
			dbg_printf("\n\n");
		}

		if (*newBranchOffset2 == 0xE92D000F) {
			dbg_printf("newBranch location 2 : ");
			dbg_hexa((u32)newBranchOffset2);
			dbg_printf("\n\n");
		}

		u32 startOffset = (u32)ndsHeader->arm9executeAddress;
		u32* newBranchPrepOffset1 = (u32*)(startOffset+0x60);
		u32* newBranchPrepOffset2 = (u32*)(startOffset+0x74);
		if (moduleParams->sdk_version > 0x5050000) {
			newBranchPrepOffset1 = (u32*)(startOffset+0x70);
			newBranchPrepOffset2 = (u32*)(startOffset+0x84);
		}

		newBranchPrepOffset1[0] = 0xE92D4000; // push {lr}
		newBranchPrepOffset1[1] = 0xE3A0007E; // mov r0, #0x7E
		newBranchPrepOffset1[2] = 0xE3A02000; // mov r2, #0
		setBL(((u32)newBranchPrepOffset1 + 3*sizeof(u32)), (u32)newBranchOffset1);
		newBranchPrepOffset1[4] = 0xE8BD8000; // pop {pc}

		newBranchPrepOffset2[0] = 0xE92D4000; // push {lr}
		newBranchPrepOffset2[1] = 0xE3A0007F; // mov r0, #0x7F
		newBranchPrepOffset2[2] = 0xE3A02000; // mov r2, #0
		setBL(((u32)newBranchPrepOffset2 + 3*sizeof(u32)), (u32)newBranchOffset2);
		newBranchPrepOffset2[4] = 0xE8BD8000; // pop {pc}

		setBL(((u32)offset - 17*sizeof(u32)), (u32)newBranchPrepOffset1);
		setBL(((u32)offset - 14*sizeof(u32)), (u32)newBranchPrepOffset2);
	} else if (offset[1] == 0xFFFF0000) { // THUMB
		u16* newBranchOffset2 = (u16*)offset;

		for (int i = 0; i < 32; i++) {
			newBranchOffset2++;
			if (newBranchOffset2[0] == 0xB508 && newBranchOffset2[1] == 0x2300) {
				break;
			}
		}

		u16* newBranchOffset1 = newBranchOffset2;
		newBranchOffset1 += 6;

		for (int i = 0; i < 40; i++) {
			newBranchOffset1++;
			if (newBranchOffset1[0] == 0xB508 && newBranchOffset1[1] == 0x2300) {
				break;
			}
		}

		if (*newBranchOffset1 == 0xB508) {
			dbg_printf("newBranch location 1 : ");
			dbg_hexa((u32)newBranchOffset1);
			dbg_printf("\n\n");
		}

		if (*newBranchOffset2 == 0xB508) {
			dbg_printf("newBranch location 2 : ");
			dbg_hexa((u32)newBranchOffset2);
			dbg_printf("\n\n");
		}

		u32 startOffset = (u32)ndsHeader->arm9executeAddress;
		u16* newBranchPrepOffset1 = (u16*)(startOffset+0x60);
		u16* newBranchPrepOffset2 = (u16*)(startOffset+0x6C);
		if (moduleParams->sdk_version > 0x5050000) {
			newBranchPrepOffset1 = (u16*)(startOffset+0x70);
			newBranchPrepOffset2 = (u16*)(startOffset+0x7C);
		}

		newBranchPrepOffset1[0] = 0xB500; // push {lr}
		newBranchPrepOffset1[1] = 0x207E; // movs r0, #0x7E
		newBranchPrepOffset1[2] = 0x2200; // movs r2, #0
		setBLThumb(((u32)newBranchPrepOffset1 + 3*sizeof(u16)), (u32)newBranchOffset1);
		newBranchPrepOffset1[5] = 0xBD00; // pop {pc}

		newBranchPrepOffset2[0] = 0xB500; // push {lr}
		newBranchPrepOffset2[1] = 0x207F; // movs r0, #0x7F
		newBranchPrepOffset2[2] = 0x2200; // movs r2, #0
		setBLThumb(((u32)newBranchPrepOffset2 + 3*sizeof(u16)), (u32)newBranchOffset2);
		newBranchPrepOffset2[5] = 0xBD00; // pop {pc}

		setBLThumb(((u32)offset - 10*sizeof(u16)), (u32)newBranchPrepOffset1);
		setBLThumb(((u32)offset - 6*sizeof(u16)), (u32)newBranchPrepOffset2);
	} else {
		u32* newBranchOffset2 = offset;

		for (int i = 0; i < 32; i++) {
			newBranchOffset2++;
			if (*newBranchOffset2 == 0xE59FC004) {
				break;
			}
		}

		u32* newBranchOffset1 = newBranchOffset2;
		newBranchOffset1 += 4;

		for (int i = 0; i < 40; i++) {
			newBranchOffset1++;
			if (*newBranchOffset1 == 0xE59FC004) {
				break;
			}
		}

		if (*newBranchOffset1 == 0xE59FC004) {
			dbg_printf("newBranch location 1 : ");
			dbg_hexa((u32)newBranchOffset1);
			dbg_printf("\n\n");
		}

		if (*newBranchOffset2 == 0xE59FC004) {
			dbg_printf("newBranch location 2 : ");
			dbg_hexa((u32)newBranchOffset2);
			dbg_printf("\n\n");
		}

		u32 startOffset = (u32)ndsHeader->arm9executeAddress;
		u32* newBranchPrepOffset1 = (u32*)(startOffset+0x60);
		u32* newBranchPrepOffset2 = (u32*)(startOffset+0x74);
		if (moduleParams->sdk_version > 0x5050000) {
			newBranchPrepOffset1 = (u32*)(startOffset+0x70);
			newBranchPrepOffset2 = (u32*)(startOffset+0x84);
		}

		newBranchPrepOffset1[0] = 0xE92D4000; // push {lr}
		newBranchPrepOffset1[1] = 0xE3A0007E; // mov r0, #0x7E
		newBranchPrepOffset1[2] = 0xE3A02000; // mov r2, #0
		setBL(((u32)newBranchPrepOffset1 + 3*sizeof(u32)), (u32)newBranchOffset1);
		newBranchPrepOffset1[4] = 0xE8BD8000; // pop {pc}

		newBranchPrepOffset2[0] = 0xE92D4000; // push {lr}
		newBranchPrepOffset2[1] = 0xE3A0007F; // mov r0, #0x7F
		newBranchPrepOffset2[2] = 0xE3A02000; // mov r2, #0
		setBL(((u32)newBranchPrepOffset2 + 3*sizeof(u32)), (u32)newBranchOffset2);
		newBranchPrepOffset2[4] = 0xE8BD8000; // pop {pc}

		setBL(((u32)offset - 6*sizeof(u32)), (u32)newBranchPrepOffset1);
		setBL(((u32)offset - 3*sizeof(u32)), (u32)newBranchPrepOffset2);
	}
}

bool useSharedFont = false;

void patchSharedFontPath(const cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	//extern u32 accessControl;
	extern u32 arm9ibinarySize;
	if (/* !(accessControl & BIT(4)) || *(u32*)0x023B8000 != 0 */ !useSharedFont) {
		toncset((u32*)0x023B8000, 0, arm9ibinarySize > 0x7000 ? 0x7000 : arm9ibinarySize);
		return;
	}

	u32* offset = patchOffsetCache.sharedFontPathOffset;
	if (!patchOffsetCache.sharedFontPathChecked) {
		offset = findSharedFontPathOffset(ndsHeader);
		if (offset) {
			patchOffsetCache.sharedFontPathOffset = offset;
		}
		patchOffsetCache.sharedFontPathChecked = true;
	}

	if (!offset) {
		return;
	}

	dbg_printf("sharedFontPath location : ");
	dbg_hexa((u32)offset);
	dbg_printf("\n\n");

	const u32* dsiSaveOpen = ce9->patches->dsiSaveOpen;
	const u32* dsiSaveClose = ce9->patches->dsiSaveClose;
	const u32* dsiSaveSeek = ce9->patches->dsiSaveSeek;
	const u32* dsiSaveRead = ce9->patches->dsiSaveRead;

	u32* fileIoOpen = patchOffsetCache.fileIoOpenOffset;
	if (!patchOffsetCache.fileIoOpenOffset) {
		fileIoOpen = findFileIoOpenOffset(ndsHeader, moduleParams);
		if (fileIoOpen) {
			patchOffsetCache.fileIoOpenOffset = fileIoOpen;
		}
	}
	if (!fileIoOpen) {
		toncset((u32*)0x023B8000, 0, arm9ibinarySize);
		return;
	}

	dbg_printf("fileIoOpen location : ");
	dbg_hexa((u32)fileIoOpen);
	dbg_printf("\n\n");

	if (*(u16*)fileIoOpen != 0xB5F8) { // ARM
		u32* fileIoClose = patchOffsetCache.fileIoCloseOffset;
		if (!patchOffsetCache.fileIoCloseOffset) {
			fileIoClose = findFileIoCloseOffset(fileIoOpen);
			if (fileIoClose) {
				patchOffsetCache.fileIoCloseOffset = fileIoClose;
			}
		}
		if (!fileIoClose) {
			toncset((u32*)0x023B8000, 0, arm9ibinarySize);
			return;
		}

		dbg_printf("fileIoClose location : ");
		dbg_hexa((u32)fileIoClose);
		dbg_printf("\n\n");

		u32* fileIoSeek = patchOffsetCache.fileIoSeekOffset;
		if (!patchOffsetCache.fileIoSeekOffset) {
			fileIoSeek = findFileIoSeekOffset(fileIoClose, moduleParams);
			if (fileIoSeek) {
				patchOffsetCache.fileIoSeekOffset = fileIoSeek;
			}
		}
		if (!fileIoSeek) {
			toncset((u32*)0x023B8000, 0, arm9ibinarySize);
			return;
		}

		dbg_printf("fileIoSeek location : ");
		dbg_hexa((u32)fileIoSeek);
		dbg_printf("\n\n");

		u32* fileIoRead = patchOffsetCache.fileIoReadOffset;
		if (!patchOffsetCache.fileIoReadOffset) {
			fileIoRead = findFileIoReadOffset(fileIoSeek, moduleParams);
			if (fileIoRead) {
				patchOffsetCache.fileIoReadOffset = fileIoRead;
			}
		}
		if (!fileIoRead) {
			toncset((u32*)0x023B8000, 0, arm9ibinarySize);
			return;
		}

		dbg_printf("fileIoRead location : ");
		dbg_hexa((u32)fileIoRead);
		dbg_printf("\n\n");

		ltd_module_params_t* ltdModuleParams = (ltd_module_params_t*)patchOffsetCache.ltdModuleParamsOffset;
		if (!ltdModuleParams) {
			extern u32* findLtdModuleParamsOffset(const tNDSHeader* ndsHeader);
			ltdModuleParams = (ltd_module_params_t*)(findLtdModuleParamsOffset(ndsHeader) - 4);
			if (ltdModuleParams) {
				patchOffsetCache.ltdModuleParamsOffset = (u32*)ltdModuleParams;
			}
		}

		if (ltdModuleParams) {
			dbg_printf("Ltd module params offset: ");
			dbg_hexa((u32)ltdModuleParams);
			dbg_printf("\n");
		}

		u32 iUncompressedSizei = arm9ibinarySize;

		if (ltdModuleParams->compressed_static_end) {
			dbg_printf("arm9i is compressed");
			extern u32 decompressIBinary(unsigned char *pak_buffer, unsigned int pak_len);
			iUncompressedSizei = decompressIBinary((unsigned char*)0x023B8000, arm9ibinarySize);
		} else {
			dbg_printf("arm9i is not compressed");
		}
		dbg_printf("\n\n");

		tonccpy(moduleParams->static_bss_end, (u32*)0x023B8004, iUncompressedSizei-4);
		toncset((u32*)0x023B8000, 0, iUncompressedSizei);

		//bool armFound = false;
		//bool dsiBiosFuncsIn9i = false;
		bool relocOffsets = (strncmp(ndsHeader->gameCode, "KPT", 3) == 0);
		int zerosFound = 0;
		u32* arm9idst = moduleParams->static_bss_end;
		for (u32 i = 0; i < (iUncompressedSizei-4)/4; i++) {
			if (arm9idst[i] == (u32)offset) {
				for (int i2 = 0; i2 < 0x100/4; i2++) {
					u32* blOffset = (arm9idst + i - i2);
					u32* fileIoPtr = getOffsetFromBL(blOffset);
					if (fileIoPtr == fileIoOpen) {
						setBL((u32)blOffset, (u32)dsiSaveOpen);

						//dbg_printf("fileIoOpen bl found : ");
						//dbg_hexa((u32)blOffset);
						//dbg_printf("\n\n");
						//armFound = true;
					} else if (fileIoPtr == fileIoClose) {
						setBL((u32)blOffset, (u32)dsiSaveClose);

						//dbg_printf("fileIoClose bl found : ");
						//dbg_hexa((u32)blOffset);
						//dbg_printf("\n\n");
						//armFound = true;
					} else if (fileIoPtr == fileIoSeek) {
						setBL((u32)blOffset, (u32)dsiSaveSeek);

						//dbg_printf("fileIoSeek bl found : ");
						//dbg_hexa((u32)blOffset);
						//dbg_printf("\n\n");
						//armFound = true;
					} else if (fileIoPtr == fileIoRead) {
						setBL((u32)blOffset, (u32)dsiSaveRead);

						//dbg_printf("fileIoRead bl found : ");
						//dbg_hexa((u32)blOffset);
						//dbg_printf("\n\n");
						//armFound = true;
					} /*else if ((u32)fileIoPtr >= ((u32)ndsHeader->arm9destination)+ndsHeader->arm9binarySize && (u32)fileIoPtr < ((u32)moduleParams->static_bss_end)+0x7FFC) {
						*blOffset = 0xE3A00001; // mov r0, #1
					}*/
				}
			} else if (relocOffsets && arm9idst[i] >= ((u32)moduleParams->static_bss_end)+0x30000 && arm9idst[i] < ((u32)moduleParams->static_bss_end)+0x40000) {
				arm9idst[i] -= 0x30000;
			} else if (arm9idst[i] == 0x4770DF20 || arm9idst[i] == 0x4770DF21 || arm9idst[i] == 0x4770DF22 || arm9idst[i] == 0x4770DF23
					|| arm9idst[i] == 0x4770DF24 || arm9idst[i] == 0x4770DF25 || arm9idst[i] == 0x4770DF26 || arm9idst[i] == 0x4770DF27
					|| arm9idst[i] == 0x4770DF28 || arm9idst[i] == 0x4770DF29) {
				// Stub out DSi BIOS functions
				arm9idst[i] = 0x47702001;
				//dsiBiosFuncsIn9i = true;
			} else if (arm9idst[i] == 0) {
				zerosFound++;
				if (zerosFound == 0x200/4) {
					break;
				}
			}
		}
		/*if (!dsiBiosFuncsIn9i) {
			extern u32 iUncompressedSize;
			u32* arm9dst = ndsHeader->arm9destination;
			for (u32 i = 0; i < iUncompressedSize/4; i++) {
				if (arm9dst[i] == 0x4770DF20 || arm9dst[i] == 0x4770DF21 || arm9dst[i] == 0x4770DF22 || arm9dst[i] == 0x4770DF23
				 || arm9dst[i] == 0x4770DF24 || arm9dst[i] == 0x4770DF25 || arm9dst[i] == 0x4770DF26 || arm9dst[i] == 0x4770DF27
				 || arm9dst[i] == 0x4770DF28 || arm9dst[i] == 0x4770DF29) {
					// Stub out DSi BIOS functions
					arm9dst[i] = 0x47702001;
				}
			}
		}*/
		/*if (!armFound) {
			const u32 dsiSaveOpenT = 0x02000200;
			const u32 dsiSaveCloseT = 0x02000210;
			const u32 dsiSaveSeekT = 0x02000220;
			const u32 dsiSaveReadT = 0x02000230;

			bool openBlFound = false;
			bool closeBlFound = false;
			bool seekBlFound = false;
			bool readBlFound = false;

			u16* arm9idstT = (u16*)moduleParams->static_bss_end;
			for (u32 i = 0; i < iUncompressedSizei/4; i++) {
				if (arm9idst[i] == (u32)offset) {
					for (int i2 = 0; i2 < 0xC0/2; i2++) {
						u16* blOffset = (arm9idstT + (i*2) - i2);
						u32* fileIoPtr = (u32*)getOffsetFromBLXThumb(blOffset); // TODO: Implement getOffsetFromBLXThumb
						if (fileIoPtr == fileIoOpen) {
							setBLThumb((u32)blOffset, dsiSaveOpenT);

							//dbg_printf("fileIoOpen bl found : ");
							//dbg_hexa((u32)blOffset);
							//dbg_printf("\n\n");
							openBlFound = true;
						} else if (fileIoPtr == fileIoClose) {
							setBLThumb((u32)blOffset, dsiSaveCloseT);

							//dbg_printf("fileIoClose bl found : ");
							//dbg_hexa((u32)blOffset);
							//dbg_printf("\n\n");
							closeBlFound = true;
						} else if (fileIoPtr == fileIoSeek) {
							setBLThumb((u32)blOffset, dsiSaveSeekT);

							//dbg_printf("fileIoSeek bl found : ");
							//dbg_hexa((u32)blOffset);
							//dbg_printf("\n\n");
							seekBlFound = true;
						} else if (fileIoPtr == fileIoRead) {
							setBLThumb((u32)blOffset, dsiSaveReadT);

							//dbg_printf("fileIoRead bl found : ");
							//dbg_hexa((u32)blOffset);
							//dbg_printf("\n\n");
							readBlFound = true;
						}
					}
				}
			}
			if (openBlFound) {
				*(u16*)dsiSaveOpenT = 0x4778; // bx pc
				tonccpy((u32*)(dsiSaveOpenT + 4), dsiSaveOpen, 0xC);
			}
			if (closeBlFound) {
				*(u16*)dsiSaveCloseT = 0x4778; // bx pc
				tonccpy((u32*)(dsiSaveCloseT + 4), dsiSaveClose, 0xC);
			}
			if (seekBlFound) {
				*(u16*)dsiSaveSeekT = 0x4778; // bx pc
				tonccpy((u32*)(dsiSaveSeekT + 4), dsiSaveSeek, 0xC);
			}
			if (readBlFound) {
				*(u16*)dsiSaveReadT = 0x4778; // bx pc
				tonccpy((u32*)(dsiSaveReadT + 4), dsiSaveRead, 0xC);
			}
		}*/
	} else { // THUMB
		toncset((u32*)0x023B8000, 0, 0x7000);

		u16* fileIoClose = (u16*)patchOffsetCache.fileIoCloseOffset;
		if (!patchOffsetCache.fileIoCloseOffset) {
			fileIoClose = findFileIoCloseOffsetThumb((u16*)fileIoOpen);
			if (fileIoClose) {
				patchOffsetCache.fileIoCloseOffset = (u32*)fileIoClose;
			}
		}
		if (!fileIoClose) {
			return;
		}

		dbg_printf("fileIoClose location : ");
		dbg_hexa((u32)fileIoClose);
		dbg_printf("\n\n");

		u16* fileIoSeek = (u16*)patchOffsetCache.fileIoSeekOffset;
		if (!patchOffsetCache.fileIoSeekOffset) {
			fileIoSeek = findFileIoSeekOffsetThumb(fileIoClose);
			if (fileIoSeek) {
				patchOffsetCache.fileIoSeekOffset = (u32*)fileIoSeek;
			}
		}
		if (!fileIoSeek) {
			return;
		}

		dbg_printf("fileIoSeek location : ");
		dbg_hexa((u32)fileIoSeek);
		dbg_printf("\n\n");

		u16* fileIoRead = (u16*)patchOffsetCache.fileIoReadOffset;
		if (!patchOffsetCache.fileIoReadOffset) {
			fileIoRead = findFileIoReadOffsetThumb(fileIoSeek, moduleParams);
			if (fileIoRead) {
				patchOffsetCache.fileIoReadOffset = (u32*)fileIoRead;
			}
		}
		if (!fileIoRead) {
			return;
		}

		dbg_printf("fileIoRead location : ");
		dbg_hexa((u32)fileIoRead);
		dbg_printf("\n\n");

		return; // getOffsetFromBLThumb currently doesn't get backward offsets correctly

		tonccpy(moduleParams->static_bss_end, (u32*)0x023B8004, 0x6FFC);
		toncset((u32*)0x023B8000, 0, 0x7000);

		const u32 dsiSaveOpenT = 0x02000200;
		const u32 dsiSaveCloseT = 0x02000210;
		const u32 dsiSaveSeekT = 0x02000220;
		const u32 dsiSaveReadT = 0x02000230;

		bool openBlFound = false;
		bool closeBlFound = false;
		bool seekBlFound = false;
		bool readBlFound = false;

		u32* arm9idst = moduleParams->static_bss_end;
		u16* arm9idstT = (u16*)moduleParams->static_bss_end;
		for (u32 i = 0; i < 0x7FFC/4; i++) {
			if (arm9idst[i] == (u32)offset) {
				for (int i2 = 0; i2 < 0x100/2; i2++) {
					u16* blOffset = (arm9idstT + (i*2) - i2);
					u16* fileIoPtr = getOffsetFromBLThumb(blOffset);
					if (fileIoPtr == (u16*)fileIoOpen) {
						setBLThumb((u32)blOffset, dsiSaveOpenT);

						dbg_printf("fileIoOpen bl found : ");
						dbg_hexa((u32)blOffset);
						dbg_printf("\n\n");
						openBlFound = true;
					} else if (fileIoPtr == fileIoClose) {
						setBLThumb((u32)blOffset, dsiSaveCloseT);

						dbg_printf("fileIoClose bl found : ");
						dbg_hexa((u32)blOffset);
						dbg_printf("\n\n");
						closeBlFound = true;
					} else if (fileIoPtr == fileIoSeek) {
						setBLThumb((u32)blOffset, dsiSaveSeekT);

						dbg_printf("fileIoSeek bl found : ");
						dbg_hexa((u32)blOffset);
						dbg_printf("\n\n");
						seekBlFound = true;
					} else if (fileIoPtr == fileIoRead) {
						setBLThumb((u32)blOffset, dsiSaveReadT);

						dbg_printf("fileIoRead bl found : ");
						dbg_hexa((u32)blOffset);
						dbg_printf("\n\n");
						readBlFound = true;
					}
				}
			}
		}
		if (openBlFound) {
			*(u16*)dsiSaveOpenT = 0x4778; // bx pc
			tonccpy((u32*)(dsiSaveOpenT + 4), dsiSaveOpen, 0xC);
		}
		if (closeBlFound) {
			*(u16*)dsiSaveCloseT = 0x4778; // bx pc
			tonccpy((u32*)(dsiSaveCloseT + 4), dsiSaveClose, 0xC);
		}
		if (seekBlFound) {
			*(u16*)dsiSaveSeekT = 0x4778; // bx pc
			tonccpy((u32*)(dsiSaveSeekT + 4), dsiSaveSeek, 0xC);
		}
		if (readBlFound) {
			*(u16*)dsiSaveReadT = 0x4778; // bx pc
			tonccpy((u32*)(dsiSaveReadT + 4), dsiSaveRead, 0xC);
		}
	}
}

void patchTwlFontLoad(bool late, u32 heapAllocAddr, u32 newCodeAddr) {
	extern bool expansionPakFound;
	extern bool sharedFontInMep;
	if (expansionPakFound) {
		extern u32 clusterCache;
		extern u32* earlyTwlFontHeapAlloc;
		extern u32 earlyTwlFontHeapAllocSize;
		extern u32* twlFontHeapAlloc;
		extern u32 twlFontHeapAllocSize;

		if (late) {
			tonccpy((u32*)newCodeAddr, twlFontHeapAlloc, twlFontHeapAllocSize);
		} else {
			tonccpy((u32*)newCodeAddr, earlyTwlFontHeapAlloc, earlyTwlFontHeapAllocSize);
		}
		*(u32*)newCodeAddr = clusterCache-0x200000;
		setBL(heapAllocAddr, newCodeAddr+4);

		sharedFontInMep = true;
		return;
	}

	extern u32* twlFontHeapAllocNoMep;

	tonccpy((u32*)newCodeAddr, twlFontHeapAllocNoMep, 0xC);
	setBL(heapAllocAddr, newCodeAddr);
}

static u32 twlSaveThumbBranchOffset = 0;

static void twlSaveSetTBranch(const u32 patchData0, const u32 patchData1) {
	static u32 twlSaveThumbBranchOffsetCache[13] = {0};
	static bool offsetSet[13] = {false};

	int i = 0;
	for (i = 0; i < 13; i++) {
		if (!offsetSet[i] || twlSaveThumbBranchOffsetCache[i] == patchData1) {
			break;
		}
	}

	if (!offsetSet[i]) {
		twlSaveThumbBranchOffsetCache[i] = twlSaveThumbBranchOffset;
		twlSaveThumbBranchOffset += 4;

		setB(twlSaveThumbBranchOffsetCache[i], (u32)patchData1);
		offsetSet[i] = true;
	}

	setBLXThumb(patchData0, twlSaveThumbBranchOffsetCache[i]);
}

void patchTwlSaveFuncs(const cardengineArm9* ce9) {
	extern u32 dsi2dsSavePatchFileCluster;
	extern u32 dsi2dsSavePatchOffset;
	extern u32 dsi2dsSavePatchSize;
	if (dsi2dsSavePatchFileCluster == CLUSTER_FREE || dsi2dsSavePatchOffset == 0 || dsi2dsSavePatchSize == 0) {
		return;
	}

	const u32* dsiSaveGetResultCode = ce9->patches->dsiSaveGetResultCode;
	const u32* dsiSaveCreate = ce9->patches->dsiSaveCreate;
	const u32* dsiSaveDelete = ce9->patches->dsiSaveDelete;
	const u32* dsiSaveGetInfo = ce9->patches->dsiSaveGetInfo;
	const u32* dsiSaveSetLength = ce9->patches->dsiSaveSetLength;
	const u32* dsiSaveOpen = ce9->patches->dsiSaveOpen;
	const u32* dsiSaveOpenR = ce9->patches->dsiSaveOpenR;
	const u32* dsiSaveClose = ce9->patches->dsiSaveClose;
	const u32* dsiSaveGetLength = ce9->patches->dsiSaveGetLength;
	const u32* dsiSaveGetPosition = ce9->patches->dsiSaveGetPosition;
	const u32* dsiSaveSeek = ce9->patches->dsiSaveSeek;
	const u32* dsiSaveRead = ce9->patches->dsiSaveRead;
	const u32* dsiSaveWrite = ce9->patches->dsiSaveWrite;

	u32* ovlPatcher = NULL;
	const u8 codeLen = patchOffsetCache.a9IsThumb ? 0x54 : 0x44;
	u32* ovlPatch = NULL;

	aFile file;
	getFileFromCluster(&file, dsi2dsSavePatchFileCluster);

	for (u32 i = 0; i < dsi2dsSavePatchSize; i += 8) {
		u32 patchData[2];
		fileRead((char*)patchData, &file, dsi2dsSavePatchOffset+i, 8);
		const bool ovlPatcherAvailable = ovlPatcher;
		const bool patchIsThumb = (patchData[0] >= 0x10000000 && patchData[0] < 0x20000000);
		if (patchIsThumb) {
			patchData[0] -= 0x10000000;
		}

		switch (patchData[1]) {
			case 0x394C564F: { // 'OVL9'
				postCardReadCodeOffset = (u32)patchOffsetCache.cardReadStartOffset;
				postCardReadCodeOffset += patchOffsetCache.a9IsThumb ? 0xC : 8;
				ovlPatcher = (u32*)postCardReadCodeOffset;

				extern u32 applyDSi2DSSaveCodeToOverlay[];
				extern u32 applyDSi2DSThumbSaveCodeToOverlay[];
				tonccpy(ovlPatcher, patchOffsetCache.a9IsThumb ? applyDSi2DSThumbSaveCodeToOverlay : applyDSi2DSSaveCodeToOverlay, codeLen);

				const u32 ovlOffset = patchData[0];
				i += 8;
				fileRead((char*)patchData, &file, dsi2dsSavePatchOffset+i, 8);

				ovlPatcher[codeLen/4] = ovlOffset; // dsi2dsSaveOverlayOffset
				ovlPatcher[(codeLen+4)/4] = patchData[0]; // dsi2dsSaveOverlayCode
				ovlPatcher[(codeLen+8)/4] = 0; // dsi2dsSaveOverlayPatchLines
				ovlPatcher[(codeLen+0xC)/4] = patchData[1] ? patchData[1] : postCardReadCodeOffset+codeLen+0x10; // dsi2dsSaveOverlayPatch
				ovlPatch = (u32*)(ovlPatcher[(codeLen+0xC)/4]);
			}	break;
			case 0x4E494254: // 'TBIN'
				twlSaveThumbBranchOffset = patchData[0];
				break;
			case 0x52544547: // 'GETR'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveGetResultCode);
				} else if (ovlPatcherAvailable || (*(u32*)(patchData[0]) >= 0xEB000000 && *(u32*)(patchData[0]) < 0xEC000000)) {
					setBL(patchData[0], (u32)dsiSaveGetResultCode);
				} else {
					tonccpy((u32*)(patchData[0]), dsiSaveGetResultCode, 8);
				}
				break;
			case 0x41455243: // 'CREA'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveCreate);
				} else if (ovlPatcherAvailable || (*(u32*)(patchData[0]) >= 0xEB000000 && *(u32*)(patchData[0]) < 0xEC000000)) {
					setBL(patchData[0], (u32)dsiSaveCreate);
				} else if (*(u32*)(patchData[0]-4) == 0xE12FFF1C || *(u32*)(patchData[0]-8) == 0xE12FFF1C) {
					*(u32*)(patchData[0]) = (u32)dsiSaveCreate;
				}
				break;
			case 0x454C4544: // 'DELE'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveDelete);
				} else {
					setBL(patchData[0], (u32)dsiSaveDelete);
				}
				break;
			case 0x49544547: // 'GETI'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveGetInfo);
				} else {
					setBL(patchData[0], (u32)dsiSaveGetInfo);
				}
				break;
			case 0x4C544553: // 'SETL'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveSetLength);
				} else {
					setBL(patchData[0], (u32)dsiSaveSetLength);
				}
				break;
			case 0x4E45504F: // 'OPEN'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveOpen);
				} else if (ovlPatcherAvailable || (*(u32*)(patchData[0]) >= 0xEB000000 && *(u32*)(patchData[0]) < 0xEC000000)) {
					setBL(patchData[0], (u32)dsiSaveOpen);
				} else if (*(u32*)(patchData[0]-4) == 0xE12FFF1C || *(u32*)(patchData[0]-8) == 0xE12FFF1C) {
					*(u32*)(patchData[0]) = (u32)dsiSaveOpen;
				}
				break;
			case 0x5245504F: // 'OPER'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveOpenR);
				} else {
					setBL(patchData[0], (u32)dsiSaveOpenR);
				}
				break;
			case 0x534F4C43: // 'CLOS'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveClose);
				} else {
					setBL(patchData[0], (u32)dsiSaveClose);
				}
				break;
			case 0x4C544547: // 'GETL'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveGetLength);
				} else {
					setBL(patchData[0], (u32)dsiSaveGetLength);
				}
				break;
			case 0x50544547: // 'GETP'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveGetPosition);
				} else {
					setBL(patchData[0], (u32)dsiSaveGetPosition);
				}
				break;
			case 0x4B454553: // 'SEEK'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveSeek);
				} else {
					setBL(patchData[0], (u32)dsiSaveSeek);
				}
				break;
			case 0x44414552: // 'READ'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveRead);
				} else {
					setBL(patchData[0], (u32)dsiSaveRead);
				}
				break;
			case 0x54495257: // 'WRIT'
				if (patchIsThumb) {
					twlSaveSetTBranch(patchData[0], (u32)dsiSaveWrite);
				} else if (ovlPatcherAvailable || (*(u32*)(patchData[0]) >= 0xEB000000 && *(u32*)(patchData[0]) < 0xEC000000)) {
					setBL(patchData[0], (u32)dsiSaveWrite);
				} else if (*(u32*)(patchData[0]-4) == 0xE12FFF1C || *(u32*)(patchData[0]-8) == 0xE12FFF1C) {
					*(u32*)(patchData[0]) = (u32)dsiSaveWrite;
				}
				break;
			default:
				if (patchIsThumb) {
					const u32 patchData1 = patchData[1];
					const u16* patchData1T = (u16*)&patchData1;
					*(u16*)(patchData[0]) = patchData1T[0];
					*(u16*)(patchData[0]+2) = patchData1T[1];
				} else {
					*(u32*)(patchData[0]) = patchData[1];
				}
				break;
		}
		if (ovlPatcherAvailable) {
			ovlPatcher[(codeLen+8)/4]++;
			*ovlPatch++ = patchData[0];
			tonccpy(ovlPatch++, (void*)(patchData[0]), 4);
		}
	}
}

void codeCopy(u32* dst, u32* src, u32 len) {
	tonccpy(dst, src, len);

	u32 srci = (u32)src;
	u32 dsti = (u32)dst;
	for (int i = 0; i < len/sizeof(u32); i++) {
		if (*(u8*)(srci+3) == 0xEB) { // If bl instruction...
			// then fix it
			u32* offsetByBl = getOffsetFromBL((u32*)srci);
			setBL(dsti, (u32)offsetByBl);
		} else if (*(u8*)(srci+3) == 0xFB) { // If blx instruction...
			// then fix it
			u32* offsetByBlx = getOffsetFromBLX((u32*)srci);
			setBLX(dsti, (u32)offsetByBlx);
		}
		srci += 4;
		dsti += 4;
	}
}

/*void relocate_ce9(u32 default_location, u32 current_location, u32 size) {
    dbg_printf("relocate_ce9\n");

    u32 location_sig[1] = {default_location};

    u32* firstCardLocation =  findOffset(current_location, size, location_sig, 1);
	if (!firstCardLocation) {
		return;
	}
    dbg_printf("firstCardLocation ");
	dbg_hexa((u32)firstCardLocation);
    dbg_printf(" : ");
    dbg_hexa((u32)*firstCardLocation);
    dbg_printf("\n\n");

    *firstCardLocation = current_location;

	u32* armReadCardLocation = findOffset(current_location, size, location_sig, 1);
	if (!armReadCardLocation) {
		return;
	}
    dbg_printf("armReadCardLocation ");
	dbg_hexa((u32)armReadCardLocation);
    dbg_printf(" : ");
    dbg_hexa((u32)*armReadCardLocation);
    dbg_printf("\n\n");

    *armReadCardLocation = current_location;

    u32* thumbReadCardLocation =  findOffset(current_location, size, location_sig, 1);
	if (!thumbReadCardLocation) {
		return;
	}
    dbg_printf("thumbReadCardLocation ");
	dbg_hexa((u32)thumbReadCardLocation);
    dbg_printf(" : ");
    dbg_hexa((u32)*thumbReadCardLocation);
    dbg_printf("\n\n");

    *thumbReadCardLocation = current_location;

    u32* globalCardLocation =  findOffset(current_location, size, location_sig, 1);
	if (!globalCardLocation) {
		return;
	}
    dbg_printf("globalCardLocation ");
	dbg_hexa((u32)globalCardLocation);
    dbg_printf(" : ");
    dbg_hexa((u32)*globalCardLocation);
    dbg_printf("\n\n");

    *globalCardLocation = current_location;

    // fix the header pointer
    cardengineArm9* ce9 = (cardengineArm9*) current_location;
    ce9->patches = (cardengineArm9Patches*)((u32)ce9->patches - default_location + current_location);

    dbg_printf(" ce9->patches ");
	dbg_hexa((u32) ce9->patches);
    dbg_printf("\n\n");

    ce9->thumbPatches = (cardengineArm9ThumbPatches*)((u32)ce9->thumbPatches - default_location + current_location);
    ce9->patches->card_read_arm9 = (u32*)((u32)ce9->patches->card_read_arm9 - default_location + current_location);
    ce9->patches->card_pull_out_arm9 = (u32*)((u32)ce9->patches->card_pull_out_arm9 - default_location + current_location);
    ce9->patches->card_id_arm9 = (u32*)((u32)ce9->patches->card_id_arm9 - default_location + current_location);
    ce9->patches->card_dma_arm9 = (u32*)((u32)ce9->patches->card_dma_arm9 - default_location + current_location);
    ce9->patches->cardStructArm9 = (u32*)((u32)ce9->patches->cardStructArm9 - default_location + current_location);
    ce9->patches->card_pull = (u32*)((u32)ce9->patches->card_pull - default_location + current_location);
    ce9->patches->cacheFlushRef = (u32*)((u32)ce9->patches->cacheFlushRef - default_location + current_location);
    ce9->patches->readCachedRef = (u32*)((u32)ce9->patches->readCachedRef - default_location + current_location);
    ce9->thumbPatches->card_read_arm9 = (u32*)((u32)ce9->thumbPatches->card_read_arm9 - default_location + current_location);
    ce9->thumbPatches->card_pull_out_arm9 = (u32*)((u32)ce9->thumbPatches->card_pull_out_arm9 - default_location + current_location);
    ce9->thumbPatches->card_id_arm9 = (u32*)((u32)ce9->thumbPatches->card_id_arm9 - default_location + current_location);
    ce9->thumbPatches->card_dma_arm9 = (u32*)((u32)ce9->thumbPatches->card_dma_arm9 - default_location + current_location);
    ce9->thumbPatches->cardStructArm9 = (u32*)((u32)ce9->thumbPatches->cardStructArm9 - default_location + current_location);
    ce9->thumbPatches->card_pull = (u32*)((u32)ce9->thumbPatches->card_pull - default_location + current_location);
    ce9->thumbPatches->cacheFlushRef = (u32*)((u32)ce9->thumbPatches->cacheFlushRef - default_location + current_location);
    ce9->thumbPatches->readCachedRef = (u32*)((u32)ce9->thumbPatches->readCachedRef - default_location + current_location);
}*/

static void randomPatch(const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	const char* romTid = getRomTid(ndsHeader);

	// Random patch
	if (moduleParams->sdk_version > 0x3000000
	&& strncmp(romTid, "AKT", 3) != 0  // Doctor Tendo
	&& strncmp(romTid, "ACZ", 3) != 0  // Cars
	&& strncmp(romTid, "ABC", 3) != 0  // Harvest Moon DS
	&& strncmp(romTid, "AWL", 3) != 0) // TWEWY
	{
		u32* randomPatchOffset = patchOffsetCache.randomPatchOffset;
		if (!patchOffsetCache.randomPatchChecked) {
			randomPatchOffset = findRandomPatchOffset(ndsHeader);
			if (randomPatchOffset) {
				patchOffsetCache.randomPatchOffset = randomPatchOffset;
			}
			patchOffsetCache.randomPatchChecked = true;
		}
		if (randomPatchOffset) {
			// Patch
			//*(u32*)((u32)randomPatchOffset + 0xC) = 0x0;
			*(randomPatchOffset + 3) = 0x0;
		}
	}
}

static u32 iSpeed = 1;

static u32 CalculateOffset(u16* anAddress,u32 aShift)
{
  u32 ptr=(u32)(anAddress+2+aShift);
  ptr&=0xfffffffc;
  ptr+=(anAddress[aShift]&0xff)*sizeof(u32);
  return ptr;
}

void patchDownloadplayArm(u32* aPtr)
{
	*(aPtr) = 0xe59f0000; //ldr r0, [pc]
	*(aPtr+1) = 0xea000000; //b pc
	*(aPtr+2) = iSpeed;
}

void patchDownloadplay(const tNDSHeader* ndsHeader)
{
  u16* top=(u16*)ndsHeader->arm9destination;
  u16* bottom=(u16*)(ndsHeader->arm9destination+0x300000);
  for(u16* ii=top;ii<bottom;++ii)
  {
    //31 6E ?? 48 0? 43 3? 66
    //ldr     r1, [r6,#0x60]
    //ldr     r0, =0x406000
    //orrs    rx, ry
    //str     rx, [r6,#0x60]
    //3245 - 42 All-Time Classics (Europe) (En,Fr,De,Es,It) (Rev 1)
    if(ii[0]==0x6e31&&(ii[1]&0xff00)==0x4800&&(ii[2]&0xfff6)==0x4300&&(ii[3]&0xfffe)==0x6630)
    {
      u32 ptr=CalculateOffset(ii,1);
		*(ii+2) = 0x46c0; //nop
		*(ii+3) = 0x6630; //str r0, [r6,#0x60]
		*(u32*)(ptr) = iSpeed;
      break;
    }
    //01 98 01 6E ?? 48 01 43 01 98 01 66
    //ldr     r0, [sp,#4]
    //ldr     r1, [r0,#0x60]
    //ldr     r0, =0x406000
    //orrs    r1, r0
    //ldr     r0, [sp,#4]
    //str     r1, [r0,#0x60]
    else if(ii[0]==0x9801&&ii[1]==0x6e01&&(ii[2]&0xff00)==0x4800&&ii[3]==0x4301&&ii[4]==0x9801&&ii[5]==0x6601)
    {
      u32 ptr=CalculateOffset(ii,2);
		*(ii+3) = 0x1c01; //mov r1, r0
		*(u32*)(ptr) = iSpeed;
      break;
    }
    else if(0==(((u32)ii)&1))
    {
      u32* buffer32=(u32*)ii;
      //60 00 99 E5 06 0A 80 E3 01 05 80 E3 60 00 89 E5
      //ldr     r0, [r9,#0x60]
      //orr     r0, r0, #0x6000
      //orr     r0, r0, #0x400000
      //str     r0, [r9,#0x60]
      //1606 - Cars - Mater-National Championship (USA)
      //2119 - Nanostray 2 (USA)
      //4512 - Might & Magic - Clash of Heroes (USA) (En,Fr,Es)
      if(buffer32[0]==0xe5990060&&buffer32[1]==0xe3800a06&&buffer32[2]==0xe3800501&&buffer32[3]==0xe5890060)
      {
        patchDownloadplayArm(buffer32);
        break;
      }
      //60 10 99 E5 ?? 0? 9F E5 00 00 81 E1 60 00 89 E5
      //ldr     r1, [r9,#0x60]
      //ldr     r0, =0x406000
      //orr     r0, r1, r0
      //str     r0, [r9,#0x60]
      //3969 - Power Play Pool (Europe) (En,Fr,De,Es,It)
      else if(buffer32[0]==0xe5991060&&(buffer32[1]&0xfffff000)==0xe59f0000&&buffer32[2]==0xe1810000&&buffer32[3]==0xe5890060)
      {
        patchDownloadplayArm(buffer32);
        break;
      }
      //60 20 8? E2 00 ?0 92 E5 ?? 0? 9F E5 00 00 81 E1 00 00 82 E5
      //add     r2, rx, #0x60
      //ldr     ry, [r2]
      //ldr     rz, =0x406000
      //orr     r0, ry, rz
      //str     r0, [r2]
      //0118 - GoldenEye - Rogue Agent (Europe)
      else if((buffer32[0]&0xfff0ffff)==0xe2802060&&(buffer32[1]&0xffffefff)==0xe5920000&&(buffer32[2]&0xffffe000)==0xe59f0000&&(buffer32[3]&0xfffefffe)==0xe1800000&&buffer32[4]==0xe5820000)
      {
        patchDownloadplayArm(buffer32+1);
        break;
      }
    }
  }
}

// SDK 5
static void randomPatch5Second(const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	if (moduleParams->sdk_version < 0x5000000) {
		return;
	}

	// Random patch SDK 5 second
	u32* randomPatchOffset5Second = patchOffsetCache.randomPatch5SecondOffset;
	if (!patchOffsetCache.randomPatch5SecondChecked) {
		randomPatchOffset5Second = findRandomPatchOffset5Second(ndsHeader);
		if (randomPatchOffset5Second) {
			patchOffsetCache.randomPatch5SecondOffset = randomPatchOffset5Second;
		}
		patchOffsetCache.randomPatch5SecondChecked = true;
	}
	if (!randomPatchOffset5Second) {
		return;
	}
	// Patch
	*randomPatchOffset5Second = 0xE3A00000;
	//*(u32*)((u32)randomPatchOffset5Second + 4) = 0xE12FFF1E;
	*(randomPatchOffset5Second + 1) = 0xE12FFF1E;
}

static void nandSavePatch(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
    u32 sdPatchEntry = 0;

	const char* romTid = getRomTid(ndsHeader);

    // WarioWare: D.I.Y. (USA)
	if (strcmp(romTid, "UORE") == 0) {
		sdPatchEntry = 0x2002c04;
	}
    // WarioWare: Do It Yourself (Europe)
    if (strcmp(romTid, "UORP") == 0) {
		sdPatchEntry = 0x2002ca4;
	}
    // Made in Ore (Japan)
    if (strcmp(romTid, "UORJ") == 0) {
		sdPatchEntry = 0x2002be4;
	}

	if (sdPatchEntry) {
		//u32 gNandInit(void* data)
		*(u32*)((u8*)sdPatchEntry+0x50C) = 0xe3a00001; //mov r0, #1
		*(u32*)((u8*)sdPatchEntry+0x510) = 0xe12fff1e; //bx lr

		//u32 gNandWait(void)
		*(u32*)((u8*)sdPatchEntry+0xC9C) = 0xe12fff1e; //bx lr

		//u32 gNandState(void)
		*(u32*)((u8*)sdPatchEntry+0xEB0) = 0xe3a00003; //mov r0, #3
		*(u32*)((u8*)sdPatchEntry+0xEB4) = 0xe12fff1e; //bx lr

		//u32 gNandError(void)
		*(u32*)((u8*)sdPatchEntry+0xEC8) = 0xe3a00000; //mov r0, #0
		*(u32*)((u8*)sdPatchEntry+0xECC) = 0xe12fff1e; //bx lr

		//u32 gNandWrite(void* memory,void* flash,u32 size,u32 dma_channel)
		*(u32*)(sdPatchEntry+0x958) = 0xE51FF004; // ldr pc, =gNandWrite
		*(u32*)(sdPatchEntry+0x95C) = (u32)ce9->patches->nand_write_arm9;

		//u32 gNandRead(void* memory,void* flash,u32 size,u32 dma_channel)
		*(u32*)(sdPatchEntry+0xD24) = 0xE51FF004; // ldr pc, =gNandRead
		*(u32*)(sdPatchEntry+0xD28) = (u32)ce9->patches->nand_read_arm9;
	} else
	// Jam with the Band (Europe)
	if (strcmp(romTid, "UXBP") == 0) {
		//u32 gNandInit(void* data)
		*(u32*)(0x020613CC) = 0xe3a00001; //mov r0, #1
		*(u32*)(0x020613D0) = 0xe12fff1e; //bx lr

		//u32 gNandResume(void)
		*(u32*)(0x02061A4C) = 0xe3a00000; //mov r0, #0
		*(u32*)(0x02061A50) = 0xe12fff1e; //bx lr

		//u32 gNandError(void)
		*(u32*)(0x02061C24) = 0xe3a00000; //mov r0, #0
		*(u32*)(0x02061C28) = 0xe12fff1e; //bx lr

		//u32 gNandWrite(void* memory,void* flash,u32 size,u32 dma_channel)
		*(u32*)0x0206176C = 0xE51FF004; // ldr pc, =gNandWrite
		*(u32*)0x02061770 = (u32)ce9->patches->nand_write_arm9;

		//u32 gNandRead(void* memory,void* flash,u32 size,u32 dma_channel)
		*(u32*)0x02061AC4 = 0xE51FF004; // ldr pc, =gNandRead
		*(u32*)0x02061AC8 = (u32)ce9->patches->nand_read_arm9;
	} else
	// Nintendo DS Guide (World)
	if (strncmp(romTid, "UGD", 3) == 0) {
		//u32 gNandInit(void* data)
		*(u32*)(0x02009298) = 0xe3a00001; //mov r0, #1
		*(u32*)(0x0200929C) = 0xe12fff1e; //bx lr

		//u32 gNandResume(void)
		*(u32*)(0x020098C8) = 0xe3a00000; //mov r0, #0
		*(u32*)(0x020098CC) = 0xe12fff1e; //bx lr

		//u32 gNandState(void)
		*(u32*)(0x02009AA8) = 0xe1a00000; //nop
		*(u32*)(0x02009AB0) = 0x06600000;

		//u32 gNandError(void)
		*(u32*)(0x02009AB4) = 0xe3a00000; //mov r0, #0
		*(u32*)(0x02009AB8) = 0xe12fff1e; //bx lr

		//u32 gNandWrite(void* memory,void* flash,u32 size,u32 dma_channel)
		*(u32*)0x0200961C = 0xE51FF004; // ldr pc, =gNandWrite
		*(u32*)0x02009620 = (u32)ce9->patches->nand_write_arm9;

		//u32 gNandRead(void* memory,void* flash,u32 size,u32 dma_channel)
		*(u32*)0x02009940 = 0xE51FF004; // ldr pc, =gNandRead
		*(u32*)0x02009944 = (u32)ce9->patches->nand_read_arm9;
	}
}

static void patchCardReadPdash(cardengineArm9* ce9, const tNDSHeader* ndsHeader) {
    u32 sdPatchEntry = 0;

	const char* romTid = getRomTid(ndsHeader);
	if (strncmp(romTid, "APD", 3) != 0 && strncmp(romTid, "A24", 3) != 0) return;

	if (strncmp(romTid, "A24", 3) == 0) { // Kiosk Demo
		if (ndsHeader->gameCode[3] == 'E') {
			*(u32*)0x0206DA88 = 0xE12FFF1E; // bx lr
			*(u32*)0x0206DFCC = 0; // Fix card set DMA function find

			sdPatchEntry = 0x206DB28;
		}
	} else if (ndsHeader->gameCode[3] == 'E') {
		*(u32*)0x0206CF48 = 0xE12FFF1E; // bx lr
		*(u32*)0x0206D48C = 0; // Fix card set DMA function find

		sdPatchEntry = 0x206CFE8;
        // target 206CFE8, more similar to cardread
        // r0 cardstruct 218A6E0 ptr 20D6120
        // r1 src
        // r2 dst
        // r3 len
        // return r0=number of time executed ?? 206D1A8
	} else if (ndsHeader->gameCode[3] == 'P') {
		*(u32*)0x0206CF60 = 0xE12FFF1E; // bx lr
		*(u32*)0x0206D4A4 = 0; // Fix card set DMA function find

		sdPatchEntry = 0x206D000;
	} else if (ndsHeader->gameCode[3] == 'J') {
		*(u32*)0x0206AAF4 = 0xE12FFF1E; // bx lr
		*(u32*)0x0206B038 = 0; // Fix card set DMA function find

		sdPatchEntry = 0x206AB94;
	} else if (ndsHeader->gameCode[3] == 'K') {
		*(u32*)0x0206DFCC = 0xE12FFF1E; // bx lr
		*(u32*)0x0206E510 = 0; // Fix card set DMA function find

		sdPatchEntry = 0x206E06C;
	}

    if(sdPatchEntry) {
     	// Patch
    	u32* pDashReadPatch = ce9->patches->pdash_read;
    	tonccpy((u32*)sdPatchEntry, pDashReadPatch, 0x40);
    }
}

static void operaRamPatch(void) {
	if (dsDebugRam || !extendedMemory) {
		return;
	}

	extern u32 ROMinRAM;

	// Opera RAM patch (ARM9)
	*(u32*)0x02003D48 = 0xC400000;
	*(u32*)0x02003D4C = 0xC400004;

	*(u32*)0x02010FF0 = 0xC400000;
	*(u32*)0x02010FF4 = 0xC4000CE;

	*(u32*)0x020112AC = 0xC400080;

	*(u32*)0x020402BC = 0xC4000C2;
	*(u32*)0x020402C0 = 0xC4000C0;
	if (ROMinRAM) {
		*(u32*)0x020402CC = 0xD7FFFFE;
		*(u32*)0x020402D0 = 0xD000000;
		*(u32*)0x020402D4 = 0xD1FFFFF;
		*(u32*)0x020402D8 = 0xD3FFFFF;
		*(u32*)0x020402DC = 0xD7FFFFF;
		*(u32*)0x020402E0 = 0xDFFFFFF;	// ???
		toncset((char*)0xD000000, 0xFF, 0x800000);		// Fill fake MEP with FFs
	} else {
		*(u32*)0x020402CC = 0xCFFFFFE;
		*(u32*)0x020402D0 = 0xC800000;
		*(u32*)0x020402D4 = 0xC9FFFFF;
		*(u32*)0x020402D8 = 0xCBFFFFF;
		*(u32*)0x020402DC = 0xCFFFFFF;
		*(u32*)0x020402E0 = 0xD7FFFFF;	// ???
		toncset((char*)0xC800000, 0xFF, 0x800000);		// Fill fake MEP with FFs
	}
}

static void operaRamPatchJpn(void) {
	if (dsDebugRam || !extendedMemory) {
		return;
	}

	extern u32 ROMinRAM;

	// Opera RAM patch (ARM9)
	*(u32*)0x02003D70 = 0xC400000;
	*(u32*)0x02003D78 = 0xC400004;

	*(u32*)0x02011018 = 0xC400000;
	*(u32*)0x0201101C = 0xC4000CE;

	*(u32*)0x020112D4 = 0xC400080;

	*(u32*)0x0203FF74 = 0xC4000C2;
	*(u32*)0x0203FF78 = 0xC4000C0;
	if (ROMinRAM) {
		*(u32*)0x0203FF84 = 0xD7FFFFE;
		*(u32*)0x0203FF88 = 0xD000000;
		*(u32*)0x0203FF8C = 0xD1FFFFF;
		*(u32*)0x0203FF90 = 0xD3FFFFF;
		*(u32*)0x0203FF94 = 0xD7FFFFF;
		*(u32*)0x0203FF98 = 0xDFFFFFF;	// ???
		toncset((char*)0xD000000, 0xFF, 0x800000);		// Fill fake MEP with FFs
	} else {
		*(u32*)0x0203FF84 = 0xCFFFFFE;
		*(u32*)0x0203FF88 = 0xC800000;
		*(u32*)0x0203FF8C = 0xC9FFFFF;
		*(u32*)0x0203FF90 = 0xCBFFFFF;
		*(u32*)0x0203FF94 = 0xCFFFFFF;
		*(u32*)0x0203FF98 = 0xD7FFFFF;	// ???
		toncset((char*)0xC800000, 0xFF, 0x800000);		// Fill fake MEP with FFs
	}
}

u32 patchCardNdsArm9(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams, u32 patchMpuRegion, const bool usesCloneboot, const bool buttonsRemapped) {
	bool usesThumb;
	int readType;
	int sdk5ReadType; // SDK 5
	u32* cardReadEndOffset;
	u32* cardPullOutOffset;

	if (!patchCardIrqEnable(ce9, ndsHeader, moduleParams)) {
		dbg_printf("ERR_LOAD_OTHR\n\n");
		return ERR_LOAD_OTHR;
	}

    const char* romTid = getRomTid(ndsHeader);

    u32 startOffset = (u32)ndsHeader->arm9destination;
    if (strncmp(romTid, "UOR", 3) == 0) { // Start at 0x2003800 for "WarioWare: DIY"
        startOffset = (u32)ndsHeader->arm9destination + 0x3800;
    } else if (strncmp(romTid, "UXB", 3) == 0) { // Start at 0x2080000 for "Jam with the Band"
        startOffset = (u32)ndsHeader->arm9destination + 0x80000;
    } else if (strncmp(romTid, "USK", 3) == 0) { // Start at 0x20E8000 for "Face Training"
        startOffset = (u32)ndsHeader->arm9destination + 0xE4000;
    } else if (strncmp(romTid, "UGD", 3) == 0) { // Start at 0x2010000 for "Nintendo DS Guide"
		startOffset = (u32)ndsHeader->arm9destination + 0x10000;
	}

    dbg_printf("startOffset : ");
    dbg_hexa(startOffset);
    dbg_printf("\n\n");

	if (ndsHeader->unitCode > 0) {
		extern u32 donorFileCluster;
		extern u32 arm7mbk;
		extern u32 accessControl;

		if (((accessControl & BIT(4))
		   || strncmp(romTid, "DMF", 3) == 0
		   || (strncmp(romTid, "DME", 3) == 0 && extendedMemory)
		   || (strncmp(romTid, "DMD", 3) == 0 && extendedMemory)
		   || strncmp(romTid, "DMP", 3) == 0
		   || strncmp(romTid, "DHS", 3) == 0
		   || (strncmp(romTid, "DSY", 3) == 0 && extendedMemory)
		)	&& arm7mbk == 0x080037C0 && donorFileCluster != CLUSTER_FREE) {
			const u32 startOffset = (u32)ndsHeader->arm9executeAddress;
			if (moduleParams->sdk_version > 0x5050000) {
				*(u32*)(startOffset+0x38) = 0xE1A00000; // nop
				*(u32*)(startOffset+0x19C) = 0xE1A00000; // nop
			} else if (moduleParams->sdk_version > 0x5020000 && moduleParams->sdk_version < 0x5050000) {
				*(u32*)(startOffset+0x18C) = 0xE1A00000; // nop
			}

			patchTwlSleepMode(ndsHeader, moduleParams);
		} else if ((accessControl & BIT(4)) && arm7mbk != 0x080037C0 && moduleParams->sdk_version > 0x5050000) {
			unpatchedFunctions* unpatchedFuncs = (unpatchedFunctions*)UNPATCHED_FUNCTION_LOCATION;
			u32 startOffset = (u32)ndsHeader->arm9executeAddress;
			startOffset += 0x38;

			unpatchedFuncs->exeCodeOffset = (u32*)startOffset;
			unpatchedFuncs->exeCode = *(u32*)startOffset;

			*(u32*)startOffset = 0xE1A00000; // nop
		}
	}

	patchMpu(ndsHeader, moduleParams, patchMpuRegion);
	patchMpu2(ndsHeader, moduleParams, usesCloneboot);
	patchSlot2IoBlock(ndsHeader, moduleParams);
	patchMpuChange(ndsHeader, moduleParams);

	patchHiHeapPointer(ce9, moduleParams, ndsHeader);

	if (buttonsRemapped) {
		patchKeyInputs(ndsHeader, moduleParams);
	}

	if (isPawsAndClaws(ndsHeader)) {
		patchCardId(ce9, ndsHeader, moduleParams, false, NULL); // Patch card ID first
	}

	if (!patchCardRead(ce9, ndsHeader, moduleParams, &usesThumb, &readType, &sdk5ReadType, &cardReadEndOffset, startOffset)) {
		dbg_printf("ERR_LOAD_OTHR\n\n");
		return ERR_LOAD_OTHR;
	}

	patchCardSaveCmd(ce9, ndsHeader, moduleParams);

	if (strncmp(romTid, "V2G", 3) == 0) {
		// try to patch card read a second time
		dbg_printf("patch card read a second time\n");
		dbg_printf("startOffset : 0x02030000\n\n");
		if (!patchCardReadMvDK4(0x02030000)) {
			dbg_printf("ERR_LOAD_OTHR\n\n");
			return ERR_LOAD_OTHR;
		}
	}

	patchCardReadPdash(ce9, ndsHeader);

	patchCardPullOut(ce9, ndsHeader, moduleParams, usesThumb, sdk5ReadType, &cardPullOutOffset);

	patchCacheFlush(ce9, usesThumb, cardPullOutOffset);

	//patchForceToPowerOff(ce9, ndsHeader, usesThumb);

	if (!isPawsAndClaws(ndsHeader)) {
		if (!patchCardId(ce9, ndsHeader, moduleParams, usesThumb, cardReadEndOffset)) {
			dbg_printf("ERR_LOAD_OTHR\n\n");
			return ERR_LOAD_OTHR;
		}
	}

	if (!patchCardSetDma(ce9, ndsHeader, moduleParams, usesThumb)) {
		patchCardReadDma(ce9, ndsHeader, moduleParams, usesThumb);
	}
	if (!patchCardEndReadDma(ce9, ndsHeader, moduleParams, usesThumb)) {
		randomPatch(ndsHeader, moduleParams);
		randomPatch5Second(ndsHeader, moduleParams);
	}

	//patchDownloadplay(ndsHeader);

	patchReset(ce9, ndsHeader, moduleParams);
	patchResetTwl(ce9, ndsHeader, moduleParams);

	if (strncmp(romTid, "UBR", 3) == 0) {
		if (romTid[3] == 'J') {
			if (ndsHeader->romversion == 0) operaRamPatchJpn();
		} else {
			operaRamPatch();
		}
	}

	nandSavePatch(ce9, ndsHeader, moduleParams);

	dbg_printf("ERR_NONE\n\n");
	return ERR_NONE;
}

void patchCardNdsArm9Cont(cardengineArm9* ce9, const tNDSHeader* ndsHeader, const module_params_t* moduleParams) {
	if (ndsHeader->unitCode == 0) {
		return;
	}

	patchSharedFontPath(ce9, ndsHeader, moduleParams);

	patchTwlSaveFuncs(ce9);

	// Further patching in order for DSiWare to boot with NTR ARM7 binary
	extern u8 arm7newUnitCode;
	extern u32 arm7mbk;
	extern u32 donorFileCluster;
	if (arm7newUnitCode == 0 && arm7mbk == 0x080037C0 && donorFileCluster != CLUSTER_FREE) {
		const u32 startOffset = (u32)ndsHeader->arm9executeAddress;
		if (moduleParams->sdk_version > 0x5050000) {
			setB(startOffset+0x6C, startOffset+0xF0);
		} else {
			setB(startOffset+0x5C, (moduleParams->sdk_version > 0x5020000) ? startOffset+0xE0 : startOffset+0xD8);
		}

		patchInitLock(ndsHeader, moduleParams);
	}
}

// Slot-2 SDK patches
#ifdef LOADERTYPE0

#define SLOT2_NO_OFFSET 0xFFFFFFFFu
#define MAX_SLOT2_PATCHES 128u

typedef struct {
	unsigned functions, geometry;
} Slot2FlashDriverLayout;

static const Slot2FlashDriverLayout slot2FlashDriverLayout2 = {3, 0x14}; // 3 driver pointers; geometry +0x14
static const Slot2FlashDriverLayout slot2FlashDriverLayout31 = {6, 0x20}; // sync + async, polling, time, geometry

typedef struct {
	u32 offset, after;
} Slot2PatchWord;
typedef struct {
	u8* data;
	u32 size, ram, count, setTask;
	const VirtualSlot2Patches* patches;
	Slot2PatchWord jobs[MAX_SLOT2_PATCHES];
	bool twlSharedInfo;
} Slot2PatchContext;

static int isAddressValid(const Slot2PatchContext* v, u32 address, u32 len) {
	return address >= v->ram && len <= v->size && address - v->ram <= v->size - len;
}
static u32 readWord(const Slot2PatchContext* v, u32 p) {
	const u8* q = v->data + p;
	return (u32)q[0] | (u32)q[1] << 8 | (u32)q[2] << 16 | (u32)q[3] << 24;
}
static u16 readHalfword(const Slot2PatchContext* v, u32 p) {
	return (u16)v->data[p] | (u16)v->data[p + 1] << 8;
}
static u32 thumbLiteral(const Slot2PatchContext* v, u32 p) {
	if (p > v->size - 2 || (readHalfword(v, p) & 0xF800) != 0x4800)
		return SLOT2_NO_OFFSET;
	u32 address = ((v->ram + p + 4) & ~3u) + (readHalfword(v, p) & 255) * 4;
	return isAddressValid(v, address, 4) ? address - v->ram : SLOT2_NO_OFFSET;
}
static u32 thumbTarget(const Slot2PatchContext* v, u32 p) {
	if (p > v->size - 4)
		return SLOT2_NO_OFFSET;
	u16 a = readHalfword(v, p), b = readHalfword(v, p + 2);
	if ((a & 0xF800) != 0xF000 || (b & 0xE800) != 0xE800)
		return SLOT2_NO_OFFSET;
	s32 delta = ((s32)((u32)(a & 0x7FF) << 21) >> 9) + ((b & 0x7FF) << 1);
	u32 target = v->ram + p + 4 + delta;
	return b & 0x1000 ? target | 1 : target & ~3u;
}
static int isArmFunctionStart(u32 w) {
	return (w & 0xFFFF4000u) == 0xE92D4000u;
}
static u32 findArmLiteralOffset(const Slot2PatchContext* v, u32 p) {
	u32 w = readWord(v, p), target;
	if ((w & 0x0F7F0000u) != 0x051F0000u)
		return SLOT2_NO_OFFSET;
	if (w & 0x800000u)
		target = p + 8 + (w & 0xFFFu);
	else {
		if (p + 8 < (w & 0xFFFu))
			return SLOT2_NO_OFFSET;
		target = p + 8 - (w & 0xFFFu);
	}
	return target <= v->size - 4 && !(target & 3) ? target : SLOT2_NO_OFFSET;
}
static int addPatchWord(Slot2PatchContext* v, u32 p, u32 value) {
	if ((p & 3) || p > v->size - 4)
		return 0;
	for (u32 i = 0; i < v->count; i++)
		if (v->jobs[i].offset == p)
			return v->jobs[i].after == value;
	if (v->count == MAX_SLOT2_PATCHES)
		return 0;
	v->jobs[v->count].offset = p;
	v->jobs[v->count++].after = value;
	return 1;
}
static int addThumb(Slot2PatchContext* v, u32 p, const u16* values, unsigned count) {
	if ((p & 1) || count * 2 > v->size || p > v->size - count * 2)
		return 0;
	u32 end = (p + count * 2 + 3) & ~3u;
	if (end > v->size)
		return 0;
	/* Pack a complete span into the existing readWord jobs. Never mutate an
	 * earlier job, so the existing per-entry rollback remains sufficient. */
	for (u32 q = p & ~3u; q < end; q += 4) {
		u32 before = readWord(v, q), after = before;
		for (unsigned i = 0; i < count; i++) {
			u32 at = p + i * 2;
			if (at >= q && at < q + 4) {
				unsigned shift = (at & 2) * 8;
				after = (after & ~(0xFFFFu << shift)) | (u32)values[i] << shift;
			}
		}
		if (after != before && !addPatchWord(v, q, after))
			return 0;
	}
	return 1;
}
static int patchArmJump(Slot2PatchContext* v, u32 address, u32 target) {
	if (!target || !isAddressValid(v, address, 12) || !isArmFunctionStart(readWord(v, address - v->ram)))
		return 0;
	u32 p = address - v->ram;
	return addPatchWord(v, p, 0xE59FC000u) && addPatchWord(v, p + 4, 0xE12FFF1Cu) && addPatchWord(v, p + 8, target);
}
static int patchThumbJump(Slot2PatchContext* v, u32 address, u32 target) {
	u32 p = address - v->ram;
	unsigned pad = (address & 2) != 0;
	if (!target || !isAddressValid(v, address, 8 + pad * 2) || (readHalfword(v, p) & 0xFF00) != 0xB500)
		return 0;
	/* Presence and the three-argument pulled callback can use scratch r3. */
	u16 code[5] = {0x4B00 | pad, 0x4718, 0x46C0, 0, 0};
	code[2 + pad] = target;
	code[3 + pad] = target >> 16;
	return addThumb(v, p, code, 4 + pad);
}
static int patchArmCall(Slot2PatchContext* v, u32 p, u32 target) {
	if (!target)
		return 0;
	s32 delta = (s32)((target & ~1u) - (v->ram + p + 8));
	if (delta < -0x02000000 || delta > 0x01FFFFFE || (delta & 1))
		return 0;
	if (!(target & 1) && (delta & 3))
		return 0;
	u32 branch = generateA7Instr(v->ram + p, target & ~1u);
	if (target & 1)
		branch = (branch & 0xFFFFFFu) | 0xFA000000u | ((delta & 2) << 23);
	return addPatchWord(v, p, branch);
}
static int singleThumbLiteral(const Slot2PatchContext* v, u32 pool, u32 ref) {
	u32 start = pool > 0x1008 ? pool - 0x1008 : 0;
	u32 end = v->size - pool > 0x1008 ? pool + 0x1008 : v->size;
	for (u32 p = start & ~1u; p + 2 <= end; p += 2) {
		if (p == ref)
			continue;
		if (thumbLiteral(v, p) == pool || (!(p & 3) && p + 4 <= end && findArmLiteralOffset(v, p) == pool))
			return 0;
	}
	return 1;
}

static int patchSlot2ModuleInfo(Slot2PatchContext* v, u32 start, u32 headerOffset, bool usesThumb) {
	u32 pool = usesThumb ? thumbLiteral(v, start + 4) : findArmLiteralOffset(v, start + 8);
	if (!headerOffset || pool == SLOT2_NO_OFFSET || pool < start + headerOffset + 4)
		return 0;
	u32 available = v->size - pool;
	bool twlSharedInfo = findOffset((const u32*)(v->data + pool), available > 0x40 ? 0x40 : available,
								  slot2ModuleInfoSharedSignature5, 1) != NULL;
	u32 copy = start + headerOffset;
	if (usesThumb) {
		u32 target = v->patches->moduleInfo.thumb, end = pool;
		if (!target || readHalfword(v, start + 4) >> 8 != 0x48 || end < start + 16)
			return 0;
		if (readHalfword(v, end - 2) == 0x46C0)
			end -= 2;
		bool interwork = readHalfword(v, end - 2) == 0x4718;
		u32 finish = end - (interwork ? 12 : 8), sourcePool = SLOT2_NO_OFFSET, sourceRef = 0, irqPool = SLOT2_NO_OFFSET;
		u16 stack = readHalfword(v, start + 2), push = readHalfword(v, start);
		if (finish < copy + 4 || thumbTarget(v, copy) == SLOT2_NO_OFFSET || thumbTarget(v, finish) == SLOT2_NO_OFFSET ||
			(interwork ? !findOffsetThumb((const u16*)(v->data + end - 8), 2, slot2ModuleInfoEndSignatureThumb2, 4) :
			 (readHalfword(v, end - 4) != (stack & ~0x80) || readHalfword(v, end - 2) != (push | 0x800))))
			return 0;
		// The header LDR and IRQ-restore LDR stay close to their native calls.
		for (u32 p = copy - 14; p < copy; p += 2) {
			u32 q = thumbLiteral(v, p);
			if (readHalfword(v, p) >> 8 == 0x49 && q != SLOT2_NO_OFFSET && readWord(v, q) == 0x08000080) {
				sourcePool = q;
				sourceRef = p;
			}
		}
		for (u32 p = finish - 10; p < finish; p += 2) {
			u32 q = thumbLiteral(v, p);
			if (readHalfword(v, p) >> 8 == 0x49 && q != SLOT2_NO_OFFSET && readWord(v, q) == 0x04000208)
				irqPool = q;
		}
		if (sourcePool == SLOT2_NO_OFFSET || irqPool == SLOT2_NO_OFFSET || !singleThumbLiteral(v, sourcePool, sourceRef) ||
			!findOffsetThumb((const u16*)(v->data + copy - 14), 14, slot2ModuleInfoSizeSignatureThumb, 1) ||
			!findOffsetThumb((const u16*)(v->data + copy - 14), 14, slot2ModuleInfoDestinationSignatureThumb, 1))
			return 0;
		u32 pc = (v->ram + finish + 4) & ~3u;
		if (v->ram + sourcePool < pc || v->ram + sourcePool - pc > 1020)
			return 0;
		// Both calls fit in place; native early returns and stack cleanup remain intact.
		const u16 header[] = {0x4788, 0x46C0}; // BLX r1; NOP
		const u16 tail[] = {0x4B00 | ((v->ram + sourcePool - pc) >> 2), 0x4798}; // LDR r3; BLX r3
		if (!addPatchWord(v, sourcePool, target) || !addThumb(v, copy, header, 2) || !addThumb(v, finish, tail, 2))
			return 0;
	} else {
		u32 push = readWord(v, start), frame = readWord(v, start + 4);
		bool early = (readWord(v, start + 8) & 0xFFFFF000) == 0xE59F5000;
		bool wide = push == 0xE92D4FF0 || push == 0xE92D4FF8;
		u32 source = copy - 16, destination = copy - 20, dma = copy - 12, check = copy - 8;
		const u32* signature = slot2ModuleInfoHeaderSignature3;
		if (early) {
			destination = copy - 12;
			dma = copy - 20;
			signature = slot2ModuleInfoHeaderSignature2;
		} else if (headerOffset == 0xA8 && !wide) {
			source = copy - 24;
			destination = copy - 32;
			dma = copy - 20;
			signature = slot2ModuleInfoHeaderSignature2;
		} else if (wide) {
			source = copy - (headerOffset == 0xAC ? 20 : 16);
			destination = start + 0x80;
			dma = copy - (headerOffset == 0xAC ? 4 : 12);
			check = copy - (headerOffset == 0xAC ? 16 : 8);
			signature = slot2ModuleInfoHeaderSignature5;
		} else if (headerOffset == 0xB4 && frame == 0xE24DD014) {
			destination = copy - 24;
			dma = copy - 20;
			check = copy - 12;
		}
		u32 sourcePool = findArmLiteralOffset(v, source), destinationPool = findArmLiteralOffset(v, destination);
		u32 channel = readWord(v, dma);
		if (sourcePool == SLOT2_NO_OFFSET || destinationPool == SLOT2_NO_OFFSET ||
			(wide ? channel != 0xE1A00007 || readWord(v, start + 0x20) != 0xE3A07001 :
			 (channel < 0xE3A00000 || channel > 0xE3A00003)) ||
			(readWord(v, source) & 0xFFFFF000) != 0xE59F1000 ||
			(readWord(v, destination) & 0xFFFFF000) != (wide ? 0xE59F5000 : 0xE59F2000) ||
			readWord(v, sourcePool) != 0x08000080 || readWord(v, destinationPool) < 0x02000000 ||
			readWord(v, destinationPool) > 0x03000000 - 0xC0 ||
			!findOffset((const u32*)(v->data + check), 4, signature, 2))
			return 0;
		// The entry LDR points at the pool immediately after the native epilogue.
		bool interwork = readWord(v, pool - 4) == 0xE12FFF1E;
		u32 finish = pool - (interwork ? 16 : 12);
		u32 pop = 0xE8BD0000 | (push & 0xFFFF);
		if (!interwork)
			pop = (pop & ~0x4000) | 0x8000;
		u32 headerCall = readWord(v, copy), finishCall = readWord(v, finish);
		if (copy + 4 > finish || ((headerCall >> 24) != 0xEB && (headerCall & 0xFE000000) != 0xFA000000) ||
			((finishCall >> 24) != 0xEB && (finishCall & 0xFE000000) != 0xFA000000) ||
			readWord(v, finish + 4) != frame + 0x400000 || readWord(v, finish + 8) != pop)
			return 0;
		if (!patchArmCall(v, copy, v->patches->moduleInfo.copyHeader) || !patchArmCall(v, finish, v->patches->moduleInfo.finish))
			return 0;
	}
	v->twlSharedInfo = twlSharedInfo;
	return 1;
}

static int patchSlot2FlashTask(Slot2PatchContext* v, u32 address, u32 replacement) {
	if (!replacement || !isAddressValid(v, address, 0x50))
		return 0;
	u32 start = address - v->ram, pool = SLOT2_NO_OFFSET;
	if (!isArmFunctionStart(readWord(v, start)) || readWord(v, start + 4) != 0xE24DD024u)
		return 0;
	for (u32 p = start + 8; p < start + 0x50; p += 4) {
		u32 w = readWord(v, p), q = findArmLiteralOffset(v, p);
		if (q != SLOT2_NO_OFFSET && ((w >> 12) & 15) == 1)
			pool = q;
		if ((w >> 24) == 0xEB) {
			s32 disp = (s32)(w << 8) >> 6;
			u32 target = v->ram + p + 8 + disp;
			if (pool == SLOT2_NO_OFFSET || !isAddressValid(v, target, 4) || !isAddressValid(v, readWord(v, pool), 12) ||
				!isArmFunctionStart(readWord(v, readWord(v, pool) - v->ram)))
				return 0;
			if (v->setTask && v->setTask != target)
				return 0;
			v->setTask = target;
			return addPatchWord(v, pool, replacement);
		}
		if (w == 0xE12FFF1Eu || (w & 0xFFFF8000u) == 0xE8BD8000u)
			return 0;
	}
	return 0;
}
static unsigned slot2FlashDriverCount(const Slot2PatchContext* v, u32 address) {
	if (address & 3)
		return 0;
	const Slot2FlashDriverLayout* layouts[] = {&slot2FlashDriverLayout2, &slot2FlashDriverLayout31};
	for (unsigned variant = 0; variant < sizeof(layouts) / sizeof(layouts[0]); variant++) {
		unsigned count = layouts[variant]->functions, geometry = layouts[variant]->geometry;
		if (!isAddressValid(v, address, geometry + 12))
			continue;
		u32 p = address - v->ram, size = readWord(v, p + geometry);
		if ((size != 0x10000 && size != 0x20000) || readWord(v, p + geometry + 4) != 0x1000 ||
			readWord(v, p + geometry + 8) != ((size >> 12) << 16 | 12))
			continue;
		unsigned i;
		for (i = 0; i < count; i++) {
			u32 fn = readWord(v, p + i * 4);
			if ((fn & 3) || !isAddressValid(v, fn, 12) || !isArmFunctionStart(readWord(v, fn - v->ram)))
				break;
		}
		if (i == count)
			return count;
	}
	return 0;
}

static int patchSlot2FlashIdentify(Slot2PatchContext* v, u32 start, u32 idAddress) {
	u32 end = v->size - start > 0x600 ? start + 0x600 : v->size;
	for (u32 p = start; p + 4 <= end && p < start + 0x40; p += 4) {
		u32 pool = findArmLiteralOffset(v, p);
		if (pool > p && pool < end)
			end = pool;
	}
	const u32* signatures[] = {slot2FlashMakerSignature2, slot2FlashMakerSignature21, slot2FlashMakerSignature31,
							   slot2FlashMakerSignature31Alt};
	u32 found = 0;
	if (end - start < 12)
		return 0;
	const u32* stop = (const u32*)(v->data + end - 8);
	for (u32 i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++) {
		const u32* cursor = (const u32*)(v->data + start);
		while (cursor < stop) {
			const u32* match = findOffset(cursor, (stop - cursor) * 4, signatures[i], 2);
			if (!match)
				break;
			cursor = match + 1;
			if (match[2] >> 24 != 0x0A)
				continue;
			u32 off = ((match[0] >> 4) & 0xF0) | (match[0] & 15);
			if (found && found != off)
				return 0;
			found = off;
		}
	}
	u32 seen[16], count = 0, ids = 0, maker = found;
	if (!maker)
		return 0;
	for (u32 p = start; p < end; p += 4) {
		u32 pool = findArmLiteralOffset(v, p);
		if (pool == SLOT2_NO_OFFSET)
			continue;
		u32 table = readWord(v, pool);
		if (!isAddressValid(v, table, 4))
			continue;
		for (unsigned i = 0; i < 8; i++) {
			if (!isAddressValid(v, table + i * 4, 4))
				return 0;
			u32 info = readWord(v, table + i * 4 - v->ram);
			unsigned functions = slot2FlashDriverCount(v, info);
			if (!functions || !isAddressValid(v, info, maker + 4)) {
				if (i)
					return 0;
				break;
			}
			u32 off = info - v->ram, id = readWord(v, off + maker) & 0xFFFF;
			if (id > 255 || maker < (functions + 2) * 4 + 12)
				return 0;
			unsigned j;
			for (j = 0; j < count && seen[j] != info; j++)
				;
			if (j < count) {
				if (!id)
					break;
				continue;
			}
			if (count == 16)
				return 0;
			seen[count++] = info;
			u32 shift = readWord(v, off + (functions + 2) * 4) == 0x20000 ? 8 : 0;
			if (id && !(ids & (255u << shift)))
				ids |= id << shift;
			// Flash table order: sector write, chip erase, sector erase; synchronous then task entries.
			const u32 replacement[] = {v->patches->card.flash.write[0], v->patches->card.flash.eraseChip[0],
									   v->patches->card.flash.eraseSector[0]};
			const u32 task[] = {v->patches->card.flash.write[1], v->patches->card.flash.eraseChip[1],
								v->patches->card.flash.eraseSector[1]};
			for (j = 0; j < 3; j++) {
				if (!replacement[j] ||
					(functions == 6 && !patchSlot2FlashTask(v, readWord(v, off + (j + 3) * 4), task[j])) ||
					!addPatchWord(v, off + j * 4, replacement[j]))
					return 0;
			}
			if (!id)
				break;
			if (i == 7)
				return 0;
		}
	}
	if (!ids || !v->patches->card.flash.id || !isAddressValid(v, idAddress, 20) ||
		!isArmFunctionStart(readWord(v, idAddress - v->ram)))
		return 0;

	u32 p = idAddress - v->ram;
	/* Choose a chip ID from each capacity's native table. Game/maker codes
	 * in the virtual GBA header are left to the game itself. */
	return addPatchWord(v, p, 0xE59F0008u) && addPatchWord(v, p + 4, 0xE59FC000u) &&
		   addPatchWord(v, p + 8, 0xE12FFF1Cu) && addPatchWord(v, p + 12, v->patches->card.flash.id) &&
		   addPatchWord(v, p + 16, ids);
}

static __attribute__((noinline)) u32 armTarget(const Slot2PatchContext* v, u32 p) {
	if ((readWord(v, p) >> 24) != 0xEB)
		return SLOT2_NO_OFFSET;
	return v->ram + p + 8 + ((s32)(readWord(v, p) << 8) >> 6);
}
static u32 findSlot2GpioOffset(const Slot2PatchContext* v, const u32* signature, unsigned count, u32 back, u32 port) {
	// Include the preceding call and all 48 bytes of the I/O region in the view.
	const u32* cursor = (const u32*)(v->data + back + 4);
	const u32* stop = cursor + (v->size / 4 - 12);
	u32 result = SLOT2_NO_OFFSET;
	while (cursor < stop) {
		const u32* hit = findOffset(cursor, (stop - cursor) * 4, signature, count);
		if (!hit)
			break;
		cursor = hit + 1;
		u32 offset = (const u8*)hit - v->data;
		u32 p = offset - back, pool = findArmLiteralOffset(v, p);
		if ((readWord(v, p) & 0xFF7FF000u) != 0xE51F2000u || pool == SLOT2_NO_OFFSET || readWord(v, pool) != port)
			continue;
		if (result != SLOT2_NO_OFFSET)
			return SLOT2_NO_OFFSET;
		result = p;
	}
	return result;
}
static __attribute__((noinline)) int patchSlot2Gpio(Slot2PatchContext* v) {
	u32 shadow = v->patches->gpioRegisters;
	if (v->size < 48 || !shadow || !v->patches->gpio.begin || !v->patches->gpio.end)
		return 0;
	u32 data = findSlot2GpioOffset(v, slot2GpioDataSignature31, 7, 8, 0x080000C4u);
	u32 init = findSlot2GpioOffset(v, slot2GpioInitSignature31, 3, 4, 0x080000C6u);
	if (data == SLOT2_NO_OFFSET || init == SLOT2_NO_OFFSET)
		return 0;
	if ((readWord(v, data + 4) & 0xFF7FF000u) != 0xE51F0000u || readWord(v, data + 36) != 0xE1D000B6u ||
		(readWord(v, init + 16) & 0xFF7FF000u) != 0xE51F0000u || readWord(v, init + 20) != 0xE1C210B2u ||
		readWord(v, init + 24) != 0xE1D000B6u)
		return 0;
	u32 a = findArmLiteralOffset(v, data), b = findArmLiteralOffset(v, init);
	u32 workA = findArmLiteralOffset(v, data + 4), workB = findArmLiteralOffset(v, init + 16);
	u32 begin = armTarget(v, data - 4), end = armTarget(v, data + 40);
	if (workA == SLOT2_NO_OFFSET || workB == SLOT2_NO_OFFSET || readWord(v, workA) != readWord(v, workB) ||
		readWord(v, workA) < 0x02000000u || readWord(v, workA) > 0x03000000u - 0x1C || begin == end ||
		!isAddressValid(v, begin, 4) || !isAddressValid(v, end, 4) || armTarget(v, init - 4) != begin ||
		armTarget(v, init + 28) != end || !singleThumbLiteral(v, a, data) || !singleThumbLiteral(v, b, init))
		return 0;
	return addPatchWord(v, a, shadow) && addPatchWord(v, b, shadow + 2) &&
		   patchArmCall(v, data - 4, v->patches->gpio.begin) && patchArmCall(v, data + 40, v->patches->gpio.end) &&
		   patchArmCall(v, init - 4, v->patches->gpio.begin) && patchArmCall(v, init + 28, v->patches->gpio.end);
}

static int patchSlot2Sdk(u8* image, u32 size, u32 ram, const VirtualSlot2Patches* patches, bool* twlSharedInfo) {
	if (!image || size < 16 || (ram & 3) || !patches || patches->magic != SLOT2_MAGIC)
		return 0;
	Slot2PatchContext v = {.data = image, .size = size, .ram = ram, .patches = patches};
	const u32* start = (const u32*)image;
	u32* offset;
	u32 count;
	int type = 0;
	bool usesThumb = false;
	// Module info
	u32 headerOffset;
	u32* module = findSlot2InitOffset(start, size, &headerOffset, &usesThumb);
	if (module) {
		count = v.count;
		u32 begin = (u8*)module - image;
		if (!patchSlot2ModuleInfo(&v, begin, headerOffset, usesThumb)) {
			v.count = count;
			dbg_printf("Slot-2 init patch skipped\n\n");
		}
	}
	// Cartridge presence
	int presence = findSlot2IsExistingOffset(image, size);
	count = v.count;
	if (presence >= 0 && !((presence & 1) ? patchThumbJump(&v, ram + (presence & ~1), patches->card.isExisting)
										  : patchArmJump(&v, ram + presence, patches->card.isExisting))) {
		v.count = count;
		dbg_printf("Slot-2 presence patch skipped\n\n");
	}

	// Copy and access
	u32* copyOffset;
	u32* accessOffset;
	findSlot2CopyOffset(start, size, &copyOffset, &accessOffset);
	count = v.count;
	if (copyOffset && !patchArmJump(&v, ram + ((u8*)copyOffset - image), patches->card.copy)) {
		v.count = count;
		dbg_printf("Slot-2 copy patch skipped\n\n");
	}
	count = v.count;
	if (accessOffset && !patchArmJump(&v, ram + ((u8*)accessOffset - image), patches->card.access)) {
		v.count = count;
		dbg_printf("Slot-2 access patch skipped\n\n");
	}

	// FlashRead
	offset = findSlot2FlashReadOffset(start, size, &type);
	count = v.count;
	if (offset && !patchArmJump(&v, ram + ((u8*)offset - image), patches->card.flash.read[type])) {
		v.count = count;
		dbg_printf("Slot-2 Flash read patch skipped\n\n");
	}

	// FlashVerify
	offset = findSlot2FlashVerifyOffset(start, size, &type);
	count = v.count;
	if (offset && !patchArmJump(&v, ram + ((u8*)offset - image), patches->card.flash.verify[type])) {
		v.count = count;
		dbg_printf("Slot-2 Flash verify patch skipped\n\n");
	}

	// FlashWriteVerify
	offset = findSlot2FlashWriteVerifyOffset(start, size, &type);
	count = v.count;
	if (offset && !patchArmJump(&v, ram + ((u8*)offset - image), patches->card.flash.writeVerify[type])) {
		v.count = count;
		dbg_printf("Slot-2 Flash write/verify patch skipped\n\n");
	}

	// SramRead
	offset = findSlot2SramReadOffset(start, size);
	count = v.count;
	if (offset && !patchArmJump(&v, ram + ((u8*)offset - image), patches->card.readSram)) {
		v.count = count;
		dbg_printf("Slot-2 SRAM read patch skipped\n\n");
	}

	// Cartridge callback
	offset = findSlot2PulledCallbackOffset(start, size, &usesThumb);
	count = v.count;
	if (offset && !(usesThumb ? patchThumbJump(&v, ram + ((u8*)offset - image), patches->card.pulledCallback)
							   : patchArmJump(&v, ram + ((u8*)offset - image), patches->card.pulledCallback))) {
		v.count = count;
		dbg_printf("Slot-2 pulled callback patch skipped\n\n");
	}
	// Flash identify
	u32* identify = findSlot2FlashIdentifyOffset(start, size);
	// Flash ID
	u32* id = findSlot2FlashIdOffset(start, size);
	// Flash drivers
	if (identify && id) {
		count = v.count;
		u32 setTask = v.setTask;
		if (!patchSlot2FlashIdentify(&v, (u8*)identify - image, ram + ((u8*)id - image))) {
			v.count = count;
			v.setTask = setTask;
			dbg_printf("Slot-2 Flash driver patches skipped\n\n");
		} else {
			dbg_printf("Slot-2 Flash driver tables patched\n\n");
		}
	}
	count = v.count;
	if (!patchSlot2Gpio(&v)) {
		v.count = count;
		dbg_printf("Slot-2 GPIO not patched\n\n");
	} else {
		dbg_printf("Slot-2 GPIO patched\n\n");
	}
	for (u32 i = 0; i < v.count; i++)
		*(u32*)(image + v.jobs[i].offset) = v.jobs[i].after;
	if (twlSharedInfo)
		*twlSharedInfo = v.twlSharedInfo;
	dbg_printf("Slot-2 SDK patch words : ");
	dbg_hexa(v.count);
	dbg_printf("\n\n");
	return v.count;
}

extern u32 iUncompressedSize;
extern u32 newArm7binarySize;

int patchVirtualSlot2(cardengineArm9* ce9, const tNDSHeader* ndsHeader) {
	if (virtualSlot2Magic != SLOT2_MAGIC || !(virtualSlot2Flags & (SLOT2_ENABLED | SLOT2_EMPTY)))
		return 0;
	const VirtualSlot2Patches* patches = (const VirtualSlot2Patches*)ce9->patches->virtualSlot2Patches;
	if (patches && (patches->magic != SLOT2_MAGIC || patches->config < 0x02000000u ||
					patches->config > 0x03000000u - sizeof(VirtualSlot2Config))) {
		patches = 0;
	}
	dbg_printf("Slot-2 ARM7 presence scan:\n");
	int arm7Offset =
		patches ? findSlot2IsExistingOffset((const u8*)ndsHeader->arm7destination, newArm7binarySize) : -1;
	bool twlSharedInfo = false;
	dbg_printf("Slot-2 ARM9 SDK scan:\n");
	int count =
		patchSlot2Sdk((u8*)ndsHeader->arm9destination, iUncompressedSize, (u32)ndsHeader->arm9destination, patches, &twlSharedInfo);
	if (count <= 0) {
		dbg_printf("Slot-2 SDK patches not applied\n\n");
		return 0;
	}
	/* Patch the loaded source before the game's ARM7 autoloader copies it.
	 * The MOV/BX stub is independent of that source's final RAM address. */
	if (arm7Offset >= 0) {
		u8* presence = (u8*)ndsHeader->arm7destination + (arm7Offset & ~1);
		dbg_printf("Slot-2 ARM7 presence location : ");
		dbg_hexa((u32)presence);
		dbg_printf("\n\n");
		const u32 inserted = (virtualSlot2Flags & SLOT2_ENABLED) != 0;
		if (arm7Offset & 1) {
			((u16*)presence)[0] = 0x2000 | inserted; // MOV r0, inserted
			((u16*)presence)[1] = 0x4770; // BX lr
		} else {
			((u32*)presence)[0] = 0xE3A00000u | inserted;
			((u32*)presence)[1] = 0xE12FFF1Eu;
		}
	}
	VirtualSlot2Config* config = (VirtualSlot2Config*)patches->config;
	config->magic = virtualSlot2Magic;
	config->flags = (virtualSlot2Flags & ~SLOT2_TWL_SHARED_INFO) | (twlSharedInfo ? SLOT2_TWL_SHARED_INFO : 0);
	config->romCluster = virtualSlot2RomCluster;
	config->saveCluster = virtualSlot2SaveCluster;
	config->romSize = virtualSlot2RomSize;
	config->saveSize = virtualSlot2SaveSize;
	config->saveType = virtualSlot2SaveType;
	tonccpy(config->header, virtualSlot2Header, sizeof(config->header));
	dbg_printf("Virtual Slot-2 patched\n\n");
	return 1;
}
#endif
