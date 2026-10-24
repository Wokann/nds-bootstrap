#ifdef VIRTUAL_SLOT2
#include "virtual_slot2.h"
#include "tonccpy.h"
#ifdef ARM9
#include <stddef.h>
#include <nds/interrupts.h>

VirtualSlot2Config virtualSlot2Config9;
static volatile struct {
	u16 data, direction, control;
	u8 sample, input;
	u16 counter, clock;
	u32 irq;
} gpio = {.sample = 0xFF, .input = 0xFF};
typedef struct Slot2Task {
	u32 task, callback, result, data, address, offset, size, dst;
	u16 sector;
	u8 busy, param;
} Slot2Task;
typedef char Slot2_task_size_check[sizeof(Slot2Task) == 0x24 ? 1 : -1];

static int slot2IsExisting(void) {
	return virtualSlot2Config9.magic == SLOT2_MAGIC && (virtualSlot2Config9.flags & SLOT2_ENABLED);
}
static void slot2GpioBegin(u32 lockId) {
	(void)lockId;
	gpio.irq = enterCriticalSection();
	u16 input = gpio.counter >= gpio.sample ? 8 : 0;
	gpio.data =
		slot2IsExisting() && (gpio.control & 1) ? (gpio.data & gpio.direction & 15) | (input & ~gpio.direction) : 0;
}
static void slot2GpioEnd(u32 lockId) {
	(void)lockId;
	u16 pins = gpio.data & gpio.direction & 7;
	if (slot2IsExisting() && !(pins & 4)) {
		/* Solar reset, then rising-clock count. With no external light
		 * source the sample remains 0xFF, as in the reference emulator. */
		if (pins & 2) {
			gpio.counter = 0;
			gpio.clock = 0;
			gpio.sample = gpio.input;
		}
		if ((pins & 1) && !gpio.clock)
			gpio.counter++;
		gpio.clock = pins & 1;
	}
	leaveCriticalSection(gpio.irq);
}
static void slot2CopyHeader(u32 dma, const void* src, void* dst, u32 len) {
	(void)dma;
	/* Keep the original ModuleInfo/PXI handshake. ARM7 must finish cartridge
	 * initialization before it advertises its CARD callback to ARM9. */
	if (!slot2IsExisting() || (u32)src != 0x08000080u || len != 0x40)
		return;
	tonccpy(dst, virtualSlot2Config9.header + 0x80, len);
}
static u32 slot2InitEnd(u32 mask) {
	/* Finish the native ARM7 handshake, then make the shared identity match
	 * the virtual cart. Restore IE with the native OS_SetIrqMask return ABI. */
	const int ime = enterCriticalSection();
	*(volatile u8*)0x027FFC35 = slot2IsExisting() ? 1 : 0;
	u32 previous = REG_IE;
	REG_IE = mask;
	leaveCriticalSection(ime);
	return previous;
}
static u32 slot2InitThumb(u32 arg, const void* src, void* dst, u32 len) {
	/* SDK2 Thumb ModuleInfo shares its header literal between both calls.
	 * The first passes this entry in r1; the final call passes NULL. */
	if (!src)
		return slot2InitEnd(arg);
	if ((u32)src == (u32)slot2InitThumb)
		slot2CopyHeader(arg, (const void*)0x08000080u, dst, len);
	return 0;
}
static void slot2PulledCallback(u32 tag, u32 data, u32 error) {
	(void)tag;
	(void)data;
	(void)error;
}
static int slot2CardCopy(u32 dma, const void* src, void* dst, u32 len, u32 type) {
	(void)dma;
	if (type != 1 && type != 0x10 && type != 0x11 && type != 0x20 && type != 0x21)
		return 0;
	u32 address = (u32)src;
	if (address >= 0x08000000u && address < 0x0A000000u)
		return virtualSlot2Transfer(VIRTUAL_SLOT2_ROM_READ, address - 0x08000000u, dst, len);
	if (address >= 0x0A000000u && address < 0x0A010000u)
		return virtualSlot2Transfer(VIRTUAL_SLOT2_SAVE_READ, address - 0x0A000000u, dst, len);
	address = (u32)dst;
	if (address >= 0x0A000000u && address < 0x0A010000u && virtualSlot2Config9.saveType == GBA_SAVE_SRAM)
		return virtualSlot2Transfer(VIRTUAL_SLOT2_SAVE_WRITE, address - 0x0A000000u, (void*)src, len);
	return 0;
}
static int slot2CardAccess(void* address, u32 value, void* result, u32 type) {
	u32 len = (type & ~1u) == 0x10 ? 1 : (type & ~1u) == 0x20 ? 2 : (type & ~1u) == 0x40 ? 4 : 0;
	if (!len)
		return 0;
	if (type & 1) {
		if (!result)
			return slot2IsExisting();
		return slot2CardCopy(0, address, result, len, 1);
	}
	return slot2CardCopy(0, &value, address, len, 1);
}
static u32 slot2FlashRead(u32 sector, u32 off, void* dst, u32 size) {
	if (sector >= virtualSlot2Config9.saveSize / GBA_FLASH_SECTOR_SIZE)
		return 0x8000;
	u32 offset = sector * GBA_FLASH_SECTOR_SIZE;
	if (off > virtualSlot2Config9.saveSize - offset)
		return 0x8000;
	return virtualSlot2Transfer(VIRTUAL_SLOT2_SAVE_READ, offset + off, dst, size) ? 0 : 0x8000;
}
static u32 slot2FlashReadTask(const Slot2Task* t) {
	return t ? slot2FlashRead(t->sector, t->offset, (void*)t->dst, t->size) : 0x8000;
}
static u32 slot2SramReadTask(const Slot2Task* t) {
	/* SDK 3/4 SRAM uses an absolute source in task.offset; Flash uses a
	 * sector plus relative offset. Both share the existing save transport. */
	if (!t || t->offset < 0x0A000000u || t->offset >= 0x0A010000u || t->size > 0x0A010000u - t->offset)
		return 0x8000;
	return slot2FlashRead(0, t->offset - 0x0A000000u, (void*)t->dst, t->size);
}
static u32 slot2FlashVerify(u32 sector, const u8* src, u32 size) {
	u8 buffer[128];
	if (sector >= virtualSlot2Config9.saveSize / GBA_FLASH_SECTOR_SIZE || size > GBA_FLASH_SECTOR_SIZE)
		return 0x8000;
	u32 base = sector * GBA_FLASH_SECTOR_SIZE;
	for (u32 pos = 0; pos < size;) {
		u32 n = size - pos;
		if (n > sizeof(buffer))
			n = sizeof(buffer);
		if (!virtualSlot2Transfer(VIRTUAL_SLOT2_SAVE_READ, base + pos, buffer, n))
			return 0x8000;
		for (u32 i = 0; i < n; i++)
			if (buffer[i] != src[pos + i])
				return 0x0A000000u + (base & 0xFFFFu) + pos + i;
		pos += n;
	}
	return 0;
}
static u32 slot2FlashVerifyTask(const Slot2Task* t) {
	return t ? slot2FlashVerify(t->sector, (const u8*)t->data, t->size) : 0x8000;
}
static u32 slot2FlashVerifySector(u32 sector, const u8* src) {
	return slot2FlashVerify(sector, src, GBA_FLASH_SECTOR_SIZE);
}
static u32 slot2FlashWrite(u32 sector, const void* src) {
	return virtualSlot2Transfer(VIRTUAL_SLOT2_FLASH_PROGRAM, sector, (void*)src, GBA_FLASH_SECTOR_SIZE) ? 0 : 0x8000;
}
static u32 slot2FlashEraseChip(void) {
	return virtualSlot2Transfer(VIRTUAL_SLOT2_FLASH_ERASE, 0, NULL, virtualSlot2Config9.saveSize) ? 0 : 0x8000;
}
static u32 slot2FlashEraseSector(u32 sector) {
	if (sector >= virtualSlot2Config9.saveSize / GBA_FLASH_SECTOR_SIZE)
		return 0x8000;
	return virtualSlot2Transfer(VIRTUAL_SLOT2_FLASH_ERASE, sector * GBA_FLASH_SECTOR_SIZE, NULL, GBA_FLASH_SECTOR_SIZE) ? 0 : 0x8000;
}
static u32 slot2FlashWriteTask(const Slot2Task* t) {
	return t ? slot2FlashWrite(t->sector, (void*)t->data) : 0x8000;
}
static u32 slot2FlashEraseChipTask(const Slot2Task* t) {
	(void)t;
	return slot2FlashEraseChip();
}
static u32 slot2FlashEraseSectorTask(const Slot2Task* t) {
	return t ? slot2FlashEraseSector(t->sector) : 0x8000;
}
static u32 slot2FlashWriteVerify(u32 sector, const u8* src, u32 size) {
	if (size > GBA_FLASH_SECTOR_SIZE || sector >= virtualSlot2Config9.saveSize / GBA_FLASH_SECTOR_SIZE)
		return 0x8000;
	u32 result = 0x8000;
	for (unsigned retry = 0; retry < 3; retry++) {
		result = slot2FlashWrite(sector, src);
		if (!result)
			result = slot2FlashVerify(sector, src, size);
		if (!result)
			break;
	}
	return result;
}
static u32 slot2FlashWriteVerifyTask(const Slot2Task* t) {
	return t ? slot2FlashWriteVerify(t->sector, (const u8*)t->data, t->size) : 0x8000;
}
static u32 slot2FlashWriteVerifySector(u32 sector, const u8* src) {
	return slot2FlashWriteVerify(sector, src, GBA_FLASH_SECTOR_SIZE);
}
/* Native Identify keeps installing the original, coherent geometry/queue table.
 * The loader replaces every selected slot2FlashWrite/erase path before this can run. */
static u32 slot2FlashReadId(u32 ids) {
	if (!slot2IsExisting())
		return 0;
	if (virtualSlot2Config9.saveType == GBA_SAVE_FLASH512)
		return ids & 255;
	if (virtualSlot2Config9.saveType == GBA_SAVE_FLASH1M)
		return (ids >> 8) & 255;
	return 0;
}

const VirtualSlot2Patches virtualSlot2Patches = {
	.magic = SLOT2_MAGIC,
	.config = (u32)&virtualSlot2Config9,
	.card = {
		.isExisting = (u32)slot2IsExisting,
		.pulledCallback = (u32)slot2PulledCallback,
		.copy = (u32)slot2CardCopy,
		.access = (u32)slot2CardAccess,
		.readSram = (u32)slot2SramReadTask,
		.flash = {
			.id = (u32)slot2FlashReadId,
			.read = {(u32)slot2FlashRead, (u32)slot2FlashReadTask},
			.verify = {(u32)slot2FlashVerify, (u32)slot2FlashVerifyTask, (u32)slot2FlashVerifySector},
			.write = {(u32)slot2FlashWrite, (u32)slot2FlashWriteTask},
			.writeVerify = {(u32)slot2FlashWriteVerify, (u32)slot2FlashWriteVerifyTask, (u32)slot2FlashWriteVerifySector},
			.eraseChip = {(u32)slot2FlashEraseChip, (u32)slot2FlashEraseChipTask},
			.eraseSector = {(u32)slot2FlashEraseSector, (u32)slot2FlashEraseSectorTask},
		},
	},
	.moduleInfo = {(u32)slot2CopyHeader, (u32)slot2InitEnd, (u32)slot2InitThumb},
	.gpio = {(u32)slot2GpioBegin, (u32)slot2GpioEnd},
	.gpioRegisters = (u32)&gpio,
};

#endif

// Independent GBA files use the same FAT operations as the NDS ROM/save.
#if defined(ARM7) || defined(B4DS) || defined(DLDI)
#include "my_fat.h"

int virtualSlot2FileIO(const VirtualSlot2Config* cfg, u32 op, u32 offset, void* buffer, u32 len) {
	static aFile virtualSlot2RomFile, virtualSlot2SavFile;
	static u32 romCluster, saveCluster, volumeFlags;
	static bool opened;
	if (!cfg || cfg->magic != SLOT2_MAGIC || !(cfg->flags & SLOT2_ENABLED) || op > VIRTUAL_SLOT2_FLASH_ERASE)
		return 0;
	const bool rom = op == VIRTUAL_SLOT2_ROM_READ;
	const bool writing = op >= VIRTUAL_SLOT2_SAVE_WRITE;
	const u32 size = rom ? cfg->romSize : cfg->saveSize;
	if (!rom && (cfg->saveCluster < 2 || cfg->saveCluster >= 0x0FFFFFF0 || cfg->saveType < GBA_SAVE_SRAM ||
				 cfg->saveType > GBA_SAVE_FLASH1M))
		return 0;
	if (op >= VIRTUAL_SLOT2_FLASH_PROGRAM) {
		if (!((cfg->saveType == GBA_SAVE_FLASH512 && size == 0x10000) ||
			  (cfg->saveType == GBA_SAVE_FLASH1M && size == 0x20000)))
			return 0;
		if (op == VIRTUAL_SLOT2_FLASH_PROGRAM) {
			if (offset >= size / GBA_FLASH_SECTOR_SIZE || len != GBA_FLASH_SECTOR_SIZE)
				return 0;
			offset *= GBA_FLASH_SECTOR_SIZE;
		} else if ((offset | len) & (GBA_FLASH_SECTOR_SIZE - 1))
			return 0;
	}
	if (offset > size || len > size - offset || (!buffer && len && op != VIRTUAL_SLOT2_FLASH_ERASE) ||
		(writing && !(cfg->flags & SLOT2_SAVE_WRITABLE)))
		return 0;
	if (!len)
		return 1;
	const u32 volumes = cfg->flags & (SLOT2_ROM_ON_FLASHCARD | SLOT2_SAVE_ON_FLASHCARD);
	if (!opened || romCluster != cfg->romCluster || saveCluster != cfg->saveCluster || volumeFlags != volumes) {
#ifdef TWOCARD
		if (!FAT_InitFiles(false, !!(cfg->flags & SLOT2_ROM_ON_FLASHCARD)))
			return 0;
		if (cfg->saveCluster && !!(cfg->flags & SLOT2_ROM_ON_FLASHCARD) != !!(cfg->flags & SLOT2_SAVE_ON_FLASHCARD) &&
			!FAT_InitFiles(false, !!(cfg->flags & SLOT2_SAVE_ON_FLASHCARD)))
			return 0;
		getFileFromCluster(&virtualSlot2RomFile, cfg->romCluster, !!(cfg->flags & SLOT2_ROM_ON_FLASHCARD));
		getFileFromCluster(&virtualSlot2SavFile, cfg->saveCluster, !!(cfg->flags & SLOT2_SAVE_ON_FLASHCARD));
#else
		if (!(cfg->flags & SLOT2_ROM_ON_FLASHCARD) || (cfg->saveCluster && !(cfg->flags & SLOT2_SAVE_ON_FLASHCARD)))
			return 0;
		getFileFromCluster(&virtualSlot2RomFile, cfg->romCluster);
		getFileFromCluster(&virtualSlot2SavFile, cfg->saveCluster);
#endif
		virtualSlot2RomFile.fatTableSettings |= fatCheckErrors;
		virtualSlot2SavFile.fatTableSettings |= fatCheckErrors;
		romCluster = cfg->romCluster;
		saveCluster = cfg->saveCluster;
		volumeFlags = volumes;
		opened = true;
	}
	aFile* file = rom ? &virtualSlot2RomFile : &virtualSlot2SavFile;
	resetPrevSect(file);
	u32 transferred = 0;
	if (op == VIRTUAL_SLOT2_FLASH_ERASE) {
		u8 erased[128];
		toncset(erased, 0xFF, sizeof(erased));
		while (transferred < len) {
			u32 count = len - transferred;
			if (count > sizeof(erased))
				count = sizeof(erased);
			u32 result = fileWrite((const char*)erased, file, offset + transferred, count);
			transferred += result;
			if (result != count)
				break;
		}
	} else if (writing) {
		transferred = fileWrite(buffer, file, offset, len);
	} else {
		transferred = fileRead(buffer, file, offset, len);
	}
	if (transferred != len) {
		file->currentCluster = file->firstCluster;
		file->currentOffset = 0;
	}
	resetPrevSect(file);
	return transferred == len;
}
#endif

#endif
