#ifndef VIRTUAL_SLOT2_H
#define VIRTUAL_SLOT2_H

#include <stdint.h>
#include <nds/ndstypes.h>

#define SLOT2_MAGIC 0x32534C56u
#define SLOT2_ENABLED (1u << 0)
#define SLOT2_ROM_ON_FLASHCARD (1u << 1)
#define SLOT2_SAVE_ON_FLASHCARD (1u << 2)
#define SLOT2_SAVE_WRITABLE (1u << 3)
#define SLOT2_EMPTY (1u << 4)
#define SLOT2_TWL_SHARED_INFO (1u << 5)
#define GBA_FLASH_SECTOR_SIZE 0x1000u

enum GbaSaveType {
	GBA_SAVE_AUTO = 0,
	GBA_SAVE_NONE = 1,
	GBA_SAVE_SRAM = 2,
	GBA_SAVE_FLASH512 = 3,
	GBA_SAVE_FLASH1M = 4,
	GBA_SAVE_EEPROM = 5
};

/* Runtime configuration shared by the card engines and file transport. */
typedef struct VirtualSlot2Config {
	u32 magic;
	u32 flags;
	u32 romCluster;
	u32 saveCluster;
	u32 romSize;
	u32 saveSize;
	u32 saveType;
	u8 header[0x40]; // GBA header bytes 0x80-0xBF used by ModuleInfo.
} VirtualSlot2Config;
typedef char VirtualSlot2Config_size_check[sizeof(VirtualSlot2Config) == 92 ? 1 : -1];

// Virtual GBA files and their ARM9/ARM7 transport.

#define SLOT2_IPC_COMMAND 0x32534C56u
enum VirtualSlot2Operation {
	VIRTUAL_SLOT2_ROM_READ,
	VIRTUAL_SLOT2_SAVE_READ,
	VIRTUAL_SLOT2_SAVE_WRITE,
	VIRTUAL_SLOT2_FLASH_PROGRAM,
	VIRTUAL_SLOT2_FLASH_ERASE
};
typedef struct VirtualSlot2Request {
	u32 config;
	u32 operation, offset, length, buffer;
} VirtualSlot2Request;

int __attribute__((noinline)) virtualSlot2Transfer(u32, u32, void*, u32);
int virtualSlot2FileIO(const VirtualSlot2Config*, u32, u32, void*, u32);

// DS Slot-2 SDK replacements. File operations are not SDK patch entries.
typedef struct VirtualSlot2Patches {
	u32 magic, config;
	struct {
		u32 isExisting, pulledCallback, copy, access, readSram;
		struct {
			u32 id;
			// Direct and task adapters; verify entries also have a whole-sector ABI.
			u32 read[2], verify[3], write[2], writeVerify[3], eraseChip[2], eraseSector[2];
		} flash;
	} card;
	// Internal call sites in native ModuleInfo; retain its IRQ/PXI handshake.
	struct {
		u32 copyHeader, finish, thumb;
	} moduleInfo;
	struct {
		u32 begin, end;
	} gpio;
	u32 gpioRegisters;
} VirtualSlot2Patches;

extern VirtualSlot2Config virtualSlot2Config9;
extern const VirtualSlot2Patches virtualSlot2Patches;

#endif
