#include "dlp.h"

#if DLP_STAGING_ARM9 || (defined(ARM7) && defined(TWOCARD) && !defined(TWLSDK))
int dlpStageStation(u32* header, const u32* bootInfo,
	u8* mbContext, u8* mbBootContext, DlpStageImage* image) {
	// DS Download Station Volume 1's eoo descriptor supplies the ARM7 source.
	// Keep its storage driver running until the received images are staged.
	if (!dlpRamRange(bootInfo[11], header[15], 0x02000000, 0x02400000)) return -1;
	image->arm7Source = bootInfo[11];
	image->arm9Source = header[10];
	int envelope = 0;
	if (header[8] == 0x180 && header[10] == 0x02332C40 && header[11] == 0x100) {
		const u32* wrapper = (const u32*)(uintptr_t)header[10];
		static const u32 prefix[] = {
			0xE59F3044, 0xE593002C, 0xE59F102C, 0xE0400001,
			0xE59F1028, 0xE0800001, 0xE59F1024, 0xE59F2024,
			0xE59FE028, 0xE12FFF3E
		};
		for (u32 i = 0; i < sizeof(prefix)/sizeof(prefix[0]); ++i) {
			if (wrapper[i] != prefix[i]) return -1;
		}
		const u32 arm7Offset = wrapper[15];
		const u32 arm9Offset = wrapper[16];
		const u32 arm9Dest = wrapper[17];
		const u32 arm9Size = wrapper[18];
		if (wrapper[19] != 0x023FE840 || arm7Offset < 0x160 || arm9Offset < 0x160
			|| bootInfo[11] < arm7Offset || arm9Size > 0x280000
			|| !dlpRamRange(arm9Dest, arm9Size, 0x02000000, 0x02400000)) return -1;
		const u32 fileBase = bootInfo[11] - arm7Offset;
		if (!dlpRamRange(fileBase, arm9Offset, 0x02000000, 0x02400000)
			|| !dlpRamRange(fileBase + arm9Offset, arm9Size, 0x02000000, 0x02400000)
			|| (bootInfo[0] & ~1u) < arm9Dest || (bootInfo[0] & ~1u) - arm9Dest >= arm9Size) return -1;
		image->arm9Source = fileBase + arm9Offset;
		header[8] = arm9Offset;
		header[10] = arm9Dest;
		header[11] = arm9Size;
		envelope = 1;
	} else if (!dlpRamRange(header[10], header[11], 0x02000000, 0x02400000)) {
		return -1;
	}
	header[9] = bootInfo[0];
	header[13] = bootInfo[1];
	*(u16*)(mbBootContext - 2) = 2;
	tonccpy(mbBootContext, mbContext, 0x3C);
	register u32 crc __asm__("r0") = 0xFFFF;
	register const void* crcData __asm__("r1") = header;
	register u32 crcLength __asm__("r2") = 0x15E;
#ifdef __thumb__
	__asm__ volatile("swi 0x0E" : "+r"(crc), "+r"(crcData), "+r"(crcLength) : : "r3", "r12", "cc", "memory");
#else
	__asm__ volatile("swi 0x0E0000" : "+r"(crc), "+r"(crcData), "+r"(crcLength) : : "r3", "r12", "cc", "memory");
#endif
	*(u16*)((u8*)header + 0x15E) = crc;
	return envelope;
}
#endif
