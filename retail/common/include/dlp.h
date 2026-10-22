#ifndef DLP_H
#define DLP_H

#include <nds/ndstypes.h>
#include <stddef.h>
#include "tonccpy.h"

/* State owned by the ARM7 loader and resident patcher. */
extern bool dlpSessionActive;
extern bool dlpPhysicalSlot1Active;
extern bool dlpVirtualSlot1Active;
extern bool dlpVirtualSlot1Empty;

/* Initial receivers keep native card functions until a downloaded SRL is bound. */
static inline bool dlpIsInitialReceiver(const char* gameCode) {
	return dlpSessionActive && !dlpPhysicalSlot1Active && !dlpVirtualSlot1Active
		&& gameCode[0] == 'H' && gameCode[1] == 'N' && gameCode[2] == 'D';
}

/* CARD_Init copies the executing program's header only for ROM boots.
 * Download Play must publish the virtual card's header separately from
 * the downloaded program's own header. Use VRAM-safe copying on ARM7. */
static inline void dlpVirtualSlot1SetCardHeader(const void* header, bool sdk5) {
	tonccpy((void*)(uintptr_t)(sdk5 ? 0x02FFFA80u : 0x027FFA80u), header, 0x160);
}

/* The downloaded header occupies [0x2BFE00, 0x2BFF60); ARM7 starts at
 * 0x2C0000. Preserve this record outside both images across multiboot handoffs.
 * Clusters refer to the mounted volume in flags. */
#define DLP_SESSION_PAGE_OFFSET 0x2BFF60
#define DLP_SESSION_MAGIC 0x31534C44u /* DLS1 */
#define DLP_SESSION_VERSION 1
#define DLP_SLOT1_PHYSICAL 0
#define DLP_SLOT1_VIRTUAL 1
#define DLP_VIRTUAL_SLOT1_ROM_FAT (1u << 0)
#define DLP_VIRTUAL_SLOT1_SAVE_FAT (1u << 1)
#define DLP_VIRTUAL_SLOT1_EMPTY (1u << 2)

typedef struct {
	u32 magic;
	u32 version;
	u32 mode;
	u32 flags;
	u32 romCluster;
	u32 saveCluster;
	u32 romSize;
	u32 saveSize;
	u32 generation;
	u32 reloadCluster;
	u32 reloadMagic;
	u32 reserved[4];
	u32 checksum;
} DlpSession;

typedef char dlpSessionFits[(sizeof(DlpSession) == 0x40) ? 1 : -1];

static inline u32 dlpSessionChecksum(const DlpSession* session) {
	const u8* bytes = (const u8*)session;
	u32 hash = 2166136261u;
	for (u32 i = 0; i < offsetof(DlpSession, checksum); ++i) {
		hash = (hash ^ bytes[i]) * 16777619u;
	}
	return hash;
}

static inline bool dlpSessionValid(const DlpSession* session) {
	if (session->magic != DLP_SESSION_MAGIC || session->version != DLP_SESSION_VERSION
		|| session->mode > DLP_SLOT1_VIRTUAL
		|| (session->flags & ~(DLP_VIRTUAL_SLOT1_ROM_FAT | DLP_VIRTUAL_SLOT1_SAVE_FAT | DLP_VIRTUAL_SLOT1_EMPTY))
		|| session->checksum != dlpSessionChecksum(session)) return 0;
	if (session->mode == DLP_SLOT1_VIRTUAL) {
		if (session->flags & DLP_VIRTUAL_SLOT1_EMPTY) {
			return session->flags == DLP_VIRTUAL_SLOT1_EMPTY && !session->romCluster
				&& !session->saveCluster && !session->romSize && !session->saveSize;
		}
		return session->romCluster >= 2 && session->romCluster < 0x0FFFFFF0u
			&& session->saveCluster >= 2 && session->saveCluster < 0x0FFFFFF0u
			&& session->romSize >= 0x4000 && session->romSize <= 0x80000000u && session->saveSize >= 0x200
			&& (session->romCluster != session->saveCluster
				|| !!(session->flags & DLP_VIRTUAL_SLOT1_ROM_FAT) != !!(session->flags & DLP_VIRTUAL_SLOT1_SAVE_FAT));
	}
	return !session->flags && !session->romCluster && !session->saveCluster
		&& !session->romSize && !session->saveSize;
}

static inline void dlpSetSession(const DlpSession* session, bool downloadedSrl) {
	dlpSessionActive = dlpSessionValid(session);
	dlpPhysicalSlot1Active = dlpSessionActive && downloadedSrl && session->mode == DLP_SLOT1_PHYSICAL;
	dlpVirtualSlot1Active = dlpSessionActive && downloadedSrl && session->mode == DLP_SLOT1_VIRTUAL;
	dlpVirtualSlot1Empty = dlpVirtualSlot1Active && (session->flags & DLP_VIRTUAL_SLOT1_EMPTY);
}

/* Bootstrap reload resources are prepared before reception. Each received
 * program is patched separately for its SDK. */
#define DLP_RELOAD_MAGIC 0x31524C44u /* DLR1 */
#define DLP_RELOAD_SESSION_OFFSET 0x2BFF84
#define DLP_RELOAD_ARM9_OFFSET 0x200
#define DLP_RELOAD_ARM7_OFFSET 0x400
#define DLP_RELOAD_CE9_OFFSET 0x40400
#define DLP_RELOAD_CE7_OFFSET 0x50400
#define DLP_RELOAD_CHEAT_OFFSET 0x51480
#define DLP_RELOAD_FILE_SIZE 0x80000
#define DLP_RAM_MAGIC 0x314D5244u /* DRM1 */
#define DLP_RAM_PAGE_OFFSET 0x2BFFA0
#define DLP_RAM_HEADER_FLAG 0x02FFFEF0

typedef struct {
	u32 magic;
	u32 arm9Source;
	u32 arm7Source;
} DlpRamBoot;

static inline bool dlpRamRange(u32 address, u32 size, u32 begin, u32 end) {
	return address >= begin && address < end && size && size <= end-address;
}

/* ARM7 is copied first. Keep both sources below its destination and the small
 * ARM9 wait stub. Reject backwards-overlapping ARM9 copies. */
static inline bool dlpRamBootValid(const u32* header, const DlpRamBoot* boot) {
	return boot->magic == DLP_RAM_MAGIC
		&& dlpRamRange(boot->arm9Source, header[11], 0x02000000, 0x02370000)
		&& dlpRamRange(boot->arm7Source, header[15], 0x02000000, 0x02370000)
		&& dlpRamRange(header[10], header[11], 0x02000000, 0x02370000)
		&& dlpRamRange(header[14], header[15], 0x02380000, 0x023B8000)
		&& (boot->arm9Source >= header[10] || header[10]-boot->arm9Source >= header[11]);
}

#if defined(ARM9) && !defined(FOTO) && !defined(GSDD) && !defined(NODSIWARE) && !defined(DLP_STAGING_DISABLED)
#define DLP_STAGING_ARM9 1
#else
#define DLP_STAGING_ARM9 0
#endif

#define DLP_STATION_HANDOFF 0x4F4F4544u /* DEOO */
typedef struct {
	u32 arm9Source;
	u32 arm7Source;
} DlpStageImage;

/* Resolve Station ARM9/ARM7 sources and restore the multiboot context.
 * ARM7 still runs the storage driver; use its received-image buffer.
 * Return 0 for a direct SRL, 1 for an unwrapped SRL, or -1 for invalid layout. */
int dlpStageStation(u32* header, const u32* bootInfo,
	u8* mbContext, u8* mbBootContext, DlpStageImage* image);

#endif // DLP_H
