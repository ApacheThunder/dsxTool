#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <nds/arm9/dldi.h>
#include <nds/fifocommon.h>
#include <nds/fifomessages.h>

#include "bootsplash.h"
#include "dsx_dldi.h"
#include "my_sd.h"
#include "tonccpy.h"
#include "read_card.h"

#define CONSOLE_SCREEN_WIDTH 32
#define CONSOLE_SCREEN_HEIGHT 24

#define SECTOR_SIZE 512
// #define SECTOR_START 0
// Full Hidden Region
#define NUM_SECTORS 24576 // Most stuff after 5264 sector count is built in skin/theme files and other stuff
#define USED_NUMSECTORS 5264 // Reduced dump size to the original region not including extra sectors for TWL banner as that's unofficial.

// Arm9 Binary Region
#define ARM9SECTORSTART 8
#define ARM9SECTORCOUNT 4710
#define ARM9BUFFERSIZE 0x24CC00
// Arm7 Binary Region
#define ARM7SECTORSTART 5128
#define ARM7SECTORCOUNT 128
#define ARM7BUFFERSIZE 0x10000

// Banner Region 
// (All data after this in the rom region is unused dummy data so a larger TWL banner can be used!
// (although only DSi System Menu will bother to read it as one)
#define BANNERSECTORSTART 5256
#define BANNERSECTORCOUNT 18
#define BANNERBUFFERSIZE 0x2400

// Incase I ever find it...it appears to either be encrypted or not present on nand
#define HEADERSECTORSTART 11245
#define HEADERSECTORCOUNT 1
// #define HEADERBufSize 0x200 (currently same as standard read buffer size)

#define StatRefreshRate 41

static ALIGN(4) sNDSHeaderExt cartHeader;

u8 CopyBuffer[SECTOR_SIZE*USED_NUMSECTORS];
u8 BannerBuffer[BANNERBUFFERSIZE];
u8 ReadBuffer[SECTOR_SIZE];


bool ErrorState = false;

bool dsxMounted = false;
bool sdMounted = false;

char gameTitle[13] = {0};

static const char* textBuffer = "X------------------------------X\nX------------------------------X";
static const char* textProgressBuffer = "X------------------------------X\nX------------------------------X";

int ProgressTracker;
bool UpdateProgressText;

void DoWait(int waitTime = 30) {
	if (waitTime > 0)for (int i = 0; i < waitTime; i++) { swiWaitForVBlank(); }
}

void DoFATerror(bool isFatel) {
	consoleClear();
	printf("FAT Init Failed!\n");
	ErrorState = isFatel;
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) return;
	}
}

void CardInit(bool Silent, bool SCFGUnlocked, bool SkipSlotReset = false) {
	if (!Silent) { consoleClear(); }
	DoWait(30);
	// Do cart init stuff to wake cart up. DLDI init may fail otherwise!
	cardInit(&cartHeader, SkipSlotReset);
	char gameCode[7] = {0};
	tonccpy(gameTitle, cartHeader.gameTitle, 12);
	tonccpy(gameCode, cartHeader.gameCode, 6);
	DoWait(60);
	if (!Silent) {
		if (SCFGUnlocked) { iprintf("SCFG_MC Status:  %2x \n\n", REG_SCFG_MC); }
		iprintf("Detected Cart Name: %12s \n\n", gameTitle);
		iprintf("Detected Cart Game Id: %6s \n\n", gameCode);
	}
}

void MountFATDevices(bool mountSD) {
	if (mountSD && !sdMounted) {
		// Important to set this else SD init will hang/fail!
		fifoSetValue32Handler(FIFO_USER_04, sdStatusHandler, nullptr);
		DoWait();
		sdMounted = fatMountSimple("sd", __my_io_dsisd());
		consoleClear();
	}
	if (!dsxMounted)dsxMounted = fatMountSimple("dsx", &io_dsx_);
}

bool DumpSectors(u32 sectorStart, u32 sectorCount, void* buffer, bool allowHiddenRegion) {
	consoleClear();
	if (!dsxMounted) {
		printf("ERROR! DS-Xtreme DLDI Init\nfailed!\n");
		while(1) {
			swiWaitForVBlank();
			scanKeys();
			if(keysDown() & KEY_A)break;
		}
		ErrorState = true;
		return false;
	}
	DoWait(80);
	iprintf("About to dump %d sectors.\n\nPress A to begin!\n", USED_NUMSECTORS);
	printf("Press B to abort!\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) break;
		if(keysDown() & KEY_B) return false;
	}
	consoleClear();
	printf("Reading sectors to ram...\n");
	if (allowHiddenRegion) {
		dsx2ReadSectors(sectorStart, sectorCount, buffer);
	} else {
		dsxReadSectors(sectorStart, sectorCount, buffer);
	}
	DoWait(80);
	return true;
}

void DoFullDump(bool SCFGUnlocked) {
	consoleClear();
	DoWait(60);	
	iprintf("About to dump %d sectors.\n\n", NUM_SECTORS);
	printf("Press [A] to continue\n");
	printf("Press [B] to abort\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A)break;
		if(keysDown() & KEY_B)return;
	}
	if (SCFGUnlocked & !sdMounted) {
		MountFATDevices(SCFGUnlocked);
		if (!sdMounted) { DoFATerror(true); return; }
	} 
	FILE *dest;
	if (sdMounted) { dest = fopen("sd:/dsxFiles/dsx_rom.bin", "wb"); } else { dest = fopen("dsx:/dsxFiles/dsx_rom.bin", "wb"); }
	textBuffer = "Dumping sectors to dsx_rom.bin.\nPlease Wait...\n\n\n";
	textProgressBuffer = "Sectors Remaining: ";
	ProgressTracker = NUM_SECTORS;
	for (int i = 0; i < NUM_SECTORS; i++){
		dsx2ReadSectors(i, 1, ReadBuffer);
		fwrite(ReadBuffer, 0x200, 1, dest); // Used Region
		if (ProgressTracker >= 0)ProgressTracker--;
		UpdateProgressText = true;
	}
	fclose(dest);
	while (UpdateProgressText)swiWaitForVBlank();
	consoleClear();
	printf("Sector dump finished!\n\n");
	printf("Press [A] to return to main menu\n");
	printf("Press [B] to exit\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) return;
		if(keysDown() & KEY_B) { 
			ErrorState = true;
			return;
		}
	}
}

void DoNormalDump(bool SCFGUnlocked) {
	consoleClear();
	DoWait(60);
	iprintf("About to dump %d sectors.\n\n", USED_NUMSECTORS);
	printf("Press [A] to continue\n");
	printf("Press [B] to abort\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A)break;
		if(keysDown() & KEY_B)return;
	}
	if (SCFGUnlocked & !sdMounted) {
		MountFATDevices(SCFGUnlocked);
		if (!sdMounted) {
			DoFATerror(true);
			return;
		}
	} 
	FILE *dest;
	if (sdMounted) { dest = fopen("sd:/dsxFiles/dsx_rom_used.bin", "wb"); } else { dest = fopen("dsx:/dsxFiles/dsx_rom_used.bin", "wb"); }
	textBuffer = "Dumping sectors to\ndsx_rom_used.bin\nPlease Wait...\n\n\n";
	textProgressBuffer = "Sectors Remaining: ";
	ProgressTracker = USED_NUMSECTORS;
	for (int i = 0; i < USED_NUMSECTORS; i++){ 
		dsx2ReadSectors(i, 1, ReadBuffer);
		fwrite(ReadBuffer, 0x200, 1, dest); // Used Region
		if (ProgressTracker >= 0)ProgressTracker--;
		UpdateProgressText = true;
	}
	fclose(dest);
	while (UpdateProgressText)swiWaitForVBlank();
	consoleClear();
	printf("Sector dump finished!\n\n");
	printf("Press [A] to return to main menu\n");
	printf("Press [B] to exit\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) return;
		if(keysDown() & KEY_B) { 
			ErrorState = true;
			return;
		}
	}
}

void DoCartSwap() {
	consoleClear();
	DoWait(80);
	if (!DumpSectors(0, USED_NUMSECTORS, CopyBuffer, true))return;
	if (dsxMounted) {
		fatUnmount("dsx");
		dsxMounted = false;
	}
	consoleClear();
	printf("Swap carts now.\n\n");
	printf("Press [A] once done");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) break;
	}
	consoleClear();
	printf("Please wait...\n\n");
	DoWait(60);
	CardInit(false, false, true);
	printf("Press [A] to continue\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) break;
	}
	consoleClear();
	DoWait(30);
	if (!fatInitDefault()) {
		DoFATerror(true);
		return;
	}
	FILE* dest = fopen("/dsx_rom.bin", "wb");
	consoleClear();
	iprintf("Writing to dsx_rom.bin...\n\nPlease wait...\n");
	fwrite(CopyBuffer, 0x292000, 1, dest); // Used Region
	fclose(dest);
	fflush(dest);
	consoleClear();
	iprintf("Sector dump finished!\n\n");
	iprintf("Press [A] to exit\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) {
			ErrorState = true; 
			return;
		}
	}
}

void DoBannerWrite(bool SCFGUnlocked) {
	consoleClear();
	printf("About to write custom banner!\n\n");
	DoWait(60);
	printf("Press [A] to begin\nPress [B] to abort\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) break;
		if(keysDown() & KEY_B) return;
	}
	DoWait();
	if (!sdMounted && !dsxMounted) {		
		MountFATDevices(SCFGUnlocked);
		if (!sdMounted && !dsxMounted) {
			DoFATerror(true);
			return;
		}
	}
	consoleClear();
	printf("Reading dsx_banner.bin...\n");
	FILE *bannerFile;
	if (sdMounted) { bannerFile = fopen("sd:/dsxFiles/dsx_banner.bin", "rb"); } else { bannerFile = fopen("dsx:/dsxFiles/dsx_banner.bin", "rb"); }
	if (bannerFile) {
		fread(BannerBuffer, 1, BANNERBUFFERSIZE, bannerFile);
		consoleClear();
		ProgressTracker = BANNERSECTORCOUNT;
		textBuffer = "Do not power off!\nWriting new banner to cart...\n";
		textProgressBuffer = "Sectors Remaining: ";
		printf("Do not power off!\n");
		printf("Writing new banner to cart...\n");
		DoWait(60);
		fclose(bannerFile);
		dsx2WriteSectors(BANNERSECTORSTART, BANNERSECTORCOUNT, BannerBuffer);
		while(UpdateProgressText)swiWaitForVBlank();
		consoleClear();
		printf("Finished!\n\nPress [A] to return to main menu\n");
	} else {
		consoleClear();
		printf("Banner file not found!\n\nPress [A] to return to main menu\n");
	}
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) return;
	}
}

/*void DoHeaderWrite(bool SCFGUnlocked) {
	consoleClear();
	printf("About to write custom header.\n");
	DoWait(60);
	printf("Press A to begin!\nPress B to abort!\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) break;
		if(keysDown() & KEY_B) return;
	}
	DoWait();
	if (!sdMounted && !dsxMounted) {		
		MountFATDevices(SCFGUnlocked);
		if (!sdMounted && !dsxMounted) {
			DoFATerror(true);
			return;
		}
	}
	consoleClear();
	printf("Reading dsx_header.bin...\n");
	FILE *headerFile;
	if (sdMounted) { headerFile = fopen("sd:/dsxFiles/dsx_header.bin", "rb"); } else { headerFile = fopen("dsx:/dsxFiles/dsx_header.bin", "rb"); }
	if (headerFile) {
		fread(ReadBuffer, 1, 0x200, headerFile);
		consoleClear();
		printf("Do not power off!\n");
		printf("Writing new header to cart...\n");
		DoWait(60);
		fclose(headerFile);
		tempSectorTracker = HEADERSECTORCOUNT;
		dsx2WriteSectors(HEADERSECTORSTART, HEADERSECTORCOUNT, ReadBuffer);
		consoleClear();
		printf("Finished!\n\nPress A to return to main menu!\n");
	} else {
		consoleClear();
		printf("Header file not found!\n\nPress A to return to main menu!\n");
	}
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) return;
	}
}*/

// This function courtasy of lifehackerhansol
void DoArmBinaryWrites(bool SCFGUnlocked) {
	consoleClear();
	printf("About to write custom arm\nbinaries!\n\n");
	DoWait(60);
	printf("Press A to begin!\nPress B to abort!\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) break;
		if(keysDown() & KEY_B) return;
	}
	DoWait();
	if (!sdMounted && !dsxMounted) {		
		MountFATDevices(SCFGUnlocked);
		if (!sdMounted && !dsxMounted) {
			DoFATerror(true);
			return;
		}
	}
	consoleClear();
	printf("Reading dsx_firmware.nds...\n");
	FILE *ndsFile;
	if (sdMounted) {
		ndsFile = fopen("sd:/dsxFiles/dsx_firmware.nds", "rb");
	} else { 
		ndsFile = fopen("dsx:/dsxFiles/dsx_firmware.nds", "rb"); 
	}
	if (!ndsFile) {
		consoleClear();
		printf("Error: dsx_firmware.nds is\nmissing!\n\n");
		printf("Press [A] to return to Main Menu\n");
		while(1) {
			swiWaitForVBlank();
			scanKeys();
			if(keysDown() & KEY_A)return;			
		}
	}
	consoleClear();
	printf("Reading dsx_firmware.nds header...\n");
	ALIGN(4) tNDSHeader* srcNdsHeader = (tNDSHeader*)malloc(sizeof(tNDSHeader));
	fread(srcNdsHeader, sizeof(tNDSHeader), 1, ndsFile);
	// sanity check the binary sizes. We do have limits, after all
	if(srcNdsHeader->arm9binarySize > ARM9BUFFERSIZE || srcNdsHeader->arm7binarySize > ARM7BUFFERSIZE)
	{
		consoleClear();
		printf("Error! The ARM9 or ARM7 binary is\ntoo large!\n");
		if(srcNdsHeader->arm9binarySize > ARM9BUFFERSIZE)
			printf("ARM9 size must be under\n%d bytes!\n", ARM9BUFFERSIZE);
		if(srcNdsHeader->arm7binarySize > ARM7BUFFERSIZE)
			printf("ARM7 size must be under\n%d bytes!\n", ARM7BUFFERSIZE);
		printf("\nPress [A] to return to Main Menu\n");
		fclose(ndsFile);
		free(srcNdsHeader);
		while(1) {
			swiWaitForVBlank();
			scanKeys();
			if(keysDown() & KEY_A)return;
		}
	}
	/*
		sanity check the load/execute addresses.
		These must match the original DS-Xtreme header, which, as of 1.1.3, are the following:
		ARM9 = 0x02000000, ARM7 = 0x03800000
	*/
	if (
		(u32)srcNdsHeader->arm9executeAddress != 0x02000000 ||
		(u32)srcNdsHeader->arm9destination != 0x02000000 ||
		(u32)srcNdsHeader->arm7executeAddress != 0x03800000 ||
		(u32)srcNdsHeader->arm7destination != 0x03800000
	)
	{
		consoleClear();
		printf("Error: The ARM9 or ARM7 binary\ncannot boot on this flashcart!\n");
		if((u32)srcNdsHeader->arm9executeAddress != 0x02000000 || (u32)srcNdsHeader->arm9destination != 0x02000000)
			printf("ARM9 must be located at\naddress 0x02000000!\n");
		if((u32)srcNdsHeader->arm7executeAddress != 0x03800000 || (u32)srcNdsHeader->arm7destination != 0x03800000)
			printf("ARM7 must be located at\naddress 0x03800000!\n");
		printf("\nPress [A] to return to Main Menu\n");
		fclose(ndsFile);
		free(srcNdsHeader);
		while(1) {
			swiWaitForVBlank();
			scanKeys();
			if(keysDown() & KEY_A)return;			
		}
	}
	printf("Reading ARM9...\n");
	fseek(ndsFile, srcNdsHeader->arm9romOffset, SEEK_SET);
	fread(CopyBuffer, 1, srcNdsHeader->arm9binarySize, ndsFile);
	consoleClear();
	printf("Writing new arm9 binary to cart.\nDo not power off!\n");
	textBuffer = "Writing new arm9 binary to cart.\nDo not power off...\n";
	textProgressBuffer = "Sectors Remaining: ";
	ProgressTracker = ARM9SECTORCOUNT;
	DoWait(60);
	dsx2WriteSectors(ARM9SECTORSTART, ARM9SECTORCOUNT, CopyBuffer);
	DoWait(60);
	consoleClear();
	printf("Reading ARM7...\n");
	fseek(ndsFile, srcNdsHeader->arm7romOffset, SEEK_SET);
	fread(CopyBuffer, 1, srcNdsHeader->arm7binarySize, ndsFile);
	consoleClear();
	printf("Writing new arm7 binary to cart.\nDo not power off...\n");
	textBuffer = "Writing new arm7 binary to cart.\nDo not power off...\n";
	textProgressBuffer = "Sectors Remaining: ";
	ProgressTracker = ARM7SECTORCOUNT;
	fclose(ndsFile);
	free(srcNdsHeader);
	DoWait(60);
	consoleClear();
	dsx2WriteSectors(ARM7SECTORSTART, ARM7SECTORCOUNT, CopyBuffer);
	while(UpdateProgressText)swiWaitForVBlank();
	consoleClear();
	printf("Finished!\n\nPress [A] to return to main menu\n");
	while(1) {
		swiWaitForVBlank();
		scanKeys();
		if(keysDown() & KEY_A) return;
	}
}


int MainMenu(bool SCFGUnlocked) {
	int Value = -1;
	consoleClear();
	printf("Press [A] to dump used hidden\nregion\n\n");
	printf("Press [Y] to dump full hidden\nregion\n\n");
	printf("Press [X] to write new banner\n\n");
	printf("Press [START] to write new Arm\nbinaries\n\n");
	if (!SCFGUnlocked) { printf("Press [SELECT] to switch DLDI\ntarget\n\n\n"); } else { printf("\n\n\n\n\n"); }
	printf("\nPress [B] to abort and exit\n");
	// printf("DPAD DOWN to write custom header\n");
	while(Value == -1) {
		swiWaitForVBlank();
		scanKeys();
		switch (keysDown()){
			case KEY_A: 	{ Value = 0; } break;
			case KEY_Y: 	{ Value = 1; } break;
			case KEY_X:		{ Value = 2; } break;
			case KEY_START:	{ Value = 3; } break;
			case KEY_SELECT:{ if (!SCFGUnlocked)Value = 4; } break;
			case KEY_B: 	{ Value = 5; } break;
			// case KEY_DOWN:	{ Value = 6;} break;
		}
	}
	return Value;
}

void vblankHandler (void) {
	if (UpdateProgressText) {
		consoleClear();
		printf(textBuffer);
		printf(textProgressBuffer);
		iprintf("%d \n", ProgressTracker);
		UpdateProgressText = false;
	}
}

int main() {
	// Wait till Arm7 is ready
	// Some SCFG values may need updating by arm7. Wait till that's done.
	bool SCFGUnlocked = false;
	if ((REG_SCFG_EXT & BIT(31))) { 
		// Set NTR clocks. (DSx Does not play nice at the higher clock speeds. You've been warned!
		REG_SCFG_CLK = 0x80;
		REG_SCFG_EXT &= ~(1UL << 13); // Disable VRAM boost. Another thing that might throw off DSX write wait timers.
		SCFGUnlocked = true;
		DoWait(10);
	}
	defaultExceptionHandler();
	BootSplashInit();
	sysSetCartOwner (BUS_OWNER_ARM9);
	sysSetCardOwner (BUS_OWNER_ARM9);
	MountFATDevices(SCFGUnlocked);
	if (!dsxMounted) {
		DoFATerror(true);
		consoleClear();
		fifoSendValue32(FIFO_USER_03, 1);
		return 0;
	}
	
	if (sdMounted) { 
		if(access("sd:/dsxFiles", F_OK) != 0)mkdir("sd:/dsxFiles", 0777); 
	} else if (dsxMounted) { 
		if(access("dsx:/dsxFiles", F_OK) != 0)mkdir("dsx:/dsxFiles", 0777); 
	}
	
	// Enable vblank handler
	irqSet(IRQ_VBLANK, vblankHandler);
	while(1) {
		switch (MainMenu(SCFGUnlocked)) {
			case 0: { DoNormalDump(SCFGUnlocked); } break;
			case 1: { DoFullDump(SCFGUnlocked); } break;
			case 2: { DoBannerWrite(SCFGUnlocked); } break;
			case 3: { DoArmBinaryWrites(SCFGUnlocked); } break;
			case 4: { if (!SCFGUnlocked)DoCartSwap(); } break;
			case 5: { ErrorState = true; } break;
			// case 6: { DoHeaderWrite(SCFGUnlocked); } break;
		}
		if (ErrorState) {
			consoleClear();
			fifoSendValue32(FIFO_USER_03, 1);
			break;
		}
		swiWaitForVBlank();
    }
	return 0;
}

