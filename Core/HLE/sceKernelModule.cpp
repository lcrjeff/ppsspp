// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <fstream>
#include <algorithm>

#include "HLE.h"

#include "Common/FileUtil.h"
#include "../Host.h"
#include "../MIPS/MIPS.h"
#include "../MIPS/MIPSAnalyst.h"
#include "../ELF/ElfReader.h"
#include "../ELF/PrxDecrypter.h"
#include "../Debugger/SymbolMap.h"
#include "../FileSystems/FileSystem.h"
#include "../FileSystems/MetaFileSystem.h"
#include "../Util/BlockAllocator.h"
#include "../PSPLoaders.h"
#include "../System.h"
#include "../MemMap.h"
#include "../Debugger/SymbolMap.h"

#include "sceKernel.h"
#include "sceKernelModule.h"
#include "sceKernelThread.h"
#include "sceKernelMemory.h"

enum {
	PSP_THREAD_ATTR_USER = 0x80000000
};

static const char *blacklistedModules[] = {
	"LIBFONT",
	"sc_sascore",
	"audiocodec",
	"libatrac3plus",
	"videocodec",
	"mpegbase",
	"mpeg",
	"psmf",
	"pspnet",
	"pspnet_adhoc",
	"pspnet_adhocctl",
	"pspnet_adhoc_matching",
	"pspnet_adhoc_download",
	"pspnet_apctl",
	"pspnet_resolver",
	"pspnet_ap_dialog_dummy",
	"libparse_uri",
	"libparse_http",
	"libhttp_rfc",
	"libssl",
	"libsuppreacc",	
};

struct Module : public KernelObject
{
	const char *GetName() {return name;}
	const char *GetTypeName() {return "Module";}
	void GetQuickInfo(char *ptr, int size)
	{
		// ignore size
		sprintf(ptr, "name=%s gp=%08x entry=%08x",
			name,
			gp_value,
			entry_addr);
	}
	static u32 GetMissingErrorCode() { return SCE_KERNEL_ERROR_UNKNOWN_MODULE; }
	int GetIDType() const { return 0; }

	SceSize size;
	char nsegment;
	char reserved[3];
	int segmentaddr[4];
	int segmentsize[4];
	unsigned int entry_addr;
	unsigned int gp_value;
	unsigned int text_addr;
	unsigned int text_size;
	unsigned int data_size;
	unsigned int bss_size;
	// The following is only available in the v1.5 firmware and above,
	// but as sceKernelQueryModuleInfo is broken in v1.0 it doesn't matter.
	unsigned short attribute;
	unsigned char version[2];
	char name[28];
};

//////////////////////////////////////////////////////////////////////////
// MODULES
//////////////////////////////////////////////////////////////////////////
struct StartModuleInfo
{
	u32 size;
	u32 mpidtext;
	u32 mpiddata;
	u32 threadpriority;
	u32 threadattributes;
};

struct SceKernelLMOption {
	SceSize size;
	SceUID mpidtext;
	SceUID mpiddata;
	unsigned int flags;
	char position;
	char access;
	char creserved[2];
};

struct SceKernelSMOption {
	SceSize size;
	SceUID mpidstack;
	SceSize stacksize;
	int priority;
	unsigned int attribute;
};

//////////////////////////////////////////////////////////////////////////
// STATE BEGIN
static SceUID mainModuleID;	// hack
// STATE END
//////////////////////////////////////////////////////////////////////////

Module *__KernelLoadELFFromPtr(const u8 *ptr, u32 loadAddress, std::string *error_string)
{
	Module *module = new Module;
	kernelObjects.Create(module);

	u8 *newptr = 0;

	if (*(u32*)ptr == 0x5053507e) { // "~PSP"
	  // Decrypt module! YAY!
	  INFO_LOG(HLE, "Decrypting ~PSP file");
	  PSP_Header *head = (PSP_Header*)ptr;
	  const u8 *in = ptr;
	  newptr = new u8[std::max(head->elf_size, head->psp_size) + 0x40];
	  ptr = (u8*)(((size_t)(newptr + 0x3F) & ~0x3F));
	  pspDecryptPRX(in, (u8*)ptr, head->psp_size);
	}
	
	if (*(u32*)ptr != 0x464c457f)
	{
		ERROR_LOG(HLE, "Wrong magic number %08x",*(u32*)ptr);
		*error_string = "File corrupt";
		delete [] newptr;
		kernelObjects.Destroy<Module>(module->GetUID());
		return 0;
	}
	// Open ELF reader
	ElfReader reader((void*)ptr);

	if (!reader.LoadInto(loadAddress))
	{
		ERROR_LOG(HLE, "LoadInto failed");
		delete [] newptr;
		kernelObjects.Destroy<Module>(module->GetUID());
		return 0;
	}

	struct libent
	{
		u32 exportName; //default 0
		u16 bcdVersion;
		u16 moduleAttributes;
		u8 exportEntrySize;
		u8 numVariables;
		u16 numFunctions;
		u32 __entrytableAddr;
	};

	struct PspModuleInfo
	{
		// 0, 0, 1, 1 ?
		u16 moduleAttrs; //0x0000 User Mode, 0x1000 Kernel Mode
		u16 moduleVersion;
		// 28 bytes of module name, packed with 0's.
		char name[28];
		u32 gp;					 // ptr to MIPS GOT data	(global offset table)
		u32 libent;			 // ptr to .lib.ent section 
		u32 libentend;		// ptr to end of .lib.ent section 
		u32 libstub;			// ptr to .lib.stub section 
		u32 libstubend;	 // ptr to end of .lib.stub section 
	};

	SectionID sceModuleInfoSection = reader.GetSectionByName(".rodata.sceModuleInfo");
	PspModuleInfo *modinfo;
	if (sceModuleInfoSection != -1)
		modinfo = (PspModuleInfo *)Memory::GetPointer(reader.GetSectionAddr(sceModuleInfoSection));
	else
		modinfo = (PspModuleInfo *)reader.GetPtr(reader.GetSegmentPaddr(0) + reader.GetSegmentOffset(0));

	bool hasSymbols = false;
	bool dontadd = false;

	SectionID textSection = reader.GetSectionByName(".text");

	u32 textStart = reader.GetSectionAddr(textSection);
	u32 textSize = reader.GetSectionSize(textSection);

	if (!host->AttemptLoadSymbolMap())
	{
		hasSymbols = reader.LoadSymbols();
		if (!hasSymbols)
		{
			symbolMap.ResetSymbolMap();
			MIPSAnalyst::ScanForFunctions(textStart, textStart+textSize);
		}
	}
	else
	{
		dontadd = true;
	}

	module->gp_value = modinfo->gp;
	strncpy(module->name, modinfo->name, 28);

	DEBUG_LOG(LOADER,"Module %s: %08x %08x %08x", modinfo->name, modinfo->gp, modinfo->libent,modinfo->libstub);

	struct PspLibStubEntry
	{
		u32 name;
		u16 version;
		u16 flags;
		u16 size;
		u16 numFuncs;
		// each symbol has an associated nid; nidData is a pointer
		// (in .rodata.sceNid section) to an array of longs, one
		// for each function, which identifies the function whose
		// address is to be inserted.
		//
		// The hash is the first 4 bytes of a SHA-1 hash of the function
		// name.	(Represented as a little-endian long, so the order
		// of the bytes is reversed.)
		u32 nidData;
		// the address of the function stubs where the function address jumps
		// should be filled in
		u32 firstSymAddr;
	};

	int numModules = (modinfo->libstubend - modinfo->libstub)/sizeof(PspLibStubEntry);

	DEBUG_LOG(LOADER,"Num Modules: %i",numModules);
	DEBUG_LOG(LOADER,"===================================================");

	PspLibStubEntry *entry = (PspLibStubEntry *)Memory::GetPointer(modinfo->libstub);

	int numSyms=0;
	for (int m=0; m<numModules; m++)
	{
		const char *modulename = (const char*)Memory::GetPointer(entry[m].name);
		u32 *nidDataPtr = (u32*)Memory::GetPointer(entry[m].nidData);
		// u32 *stubs = (u32*)Memory::GetPointer(entry[m].firstSymAddr);

		DEBUG_LOG(LOADER,"Importing Module %s, stubs at %08x",modulename,entry[m].firstSymAddr);

		for (int i=0; i<entry[m].numFuncs; i++)
		{
			u32 addrToWriteSyscall = entry[m].firstSymAddr+i*8;
			DEBUG_LOG(LOADER,"%s : %08x",GetFuncName(modulename, nidDataPtr[i]), addrToWriteSyscall);
			//write a syscall here
			WriteSyscall(modulename, nidDataPtr[i], addrToWriteSyscall);
			if (!dontadd)
			{
				char temp[256];
				sprintf(temp,"zz_%s", GetFuncName(modulename, nidDataPtr[i]));
				symbolMap.AddSymbol(temp, addrToWriteSyscall, 8, ST_FUNCTION);
			}
			numSyms++;
		}
		DEBUG_LOG(LOADER,"-------------------------------------------------------------");
	}

	// Look at the exports, too.

	struct PspLibEntEntry
	{
		u32 name; /* ent's name (module name) address */
		u16 version;
		u16 flags;
		u8 size;
		u8 vcount;
		u16 fcount;
		u32 resident;
	};

	int numEnts = (modinfo->libentend - modinfo->libent)/sizeof(PspLibEntEntry);
	PspLibEntEntry *ent = (PspLibEntEntry *)Memory::GetPointer(modinfo->libent);
	for (int m=0; m<numEnts; m++)
	{
		const char *name;
		if (ent->size == 0)
		{
			continue;
		}

		if (ent->name == 0)
		{
			// ?
			name = module->name;
		}
		else
		{
			name = (const char*)Memory::GetPointer(ent->name);
		}

		INFO_LOG(HLE,"Exporting ent %d named %s, %d funcs, %d vars, resident %08x", m, name, ent->fcount, ent->vcount, ent->resident);
		
		// This appears to return bogus pointers for modules like sceAtrac3Plus (running Puzzle Bobble Pocket, name = sceAtrac3plus)
		u32 *residentPtr = (u32*)Memory::GetPointer(ent[m].resident);

		// TODO: Re-enable when it works
		for (u32 j = 0; j < ent[m].fcount; j++)
		{
			u32 nid = residentPtr[j];
			u32 exportAddr = residentPtr[ent->fcount + ent->vcount + j];
			ResolveSyscall(name, nid, exportAddr);
		}
		if (ent->size > 4)
		{
			ent = (PspLibEntEntry*)((u8*)ent + ent->size * 4);
		}
		else
		{
			ent++;
		}
	}

	module->entry_addr = reader.GetEntryPoint();

	delete [] newptr;
	return module;
}

bool __KernelLoadPBP(const char *filename, std::string *error_string)
{
	static const char *FileNames[] =
	{
		"PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "UNKNOWN.PNG",
		"PIC1.PNG", "SND0.AT3", "UNKNOWN.PSP", "UNKNOWN.PSAR"
	};

	std::ifstream in(filename, std::ios::binary);

	char temp[4];
	in.read(temp,4);

	if (memcmp(temp,"\0PBP",4) != 0)
	{
		//This is not a valid file!
		ERROR_LOG(LOADER,"%s is not a valid homebrew PSP1.0 PBP",filename);
		*error_string = "Not a valid homebrew PBP";
		return false;
	}

	u32 version, offset0, offsets[16];
	int numfiles;

	in.read((char*)&version,4);

	in.read((char*)&offset0,4);
	numfiles = (offset0 - 8) / 4;
	offsets[0] = offset0;
	for (int i = 1; i < numfiles; i++)
		in.read((char*)&offsets[i], 4);

	// The 6th is always the executable?
	in.seekg(offsets[5]);
	//in.read((char*)&id,4);
	{
		u8 *temp = new u8[1024*1024*8];
		in.read((char*)temp, 1024*1024*8);
		Module *module = __KernelLoadELFFromPtr(temp, PSP_GetDefaultLoadAddress(), error_string);
		if (!module)
			return false;
		mipsr4k.pc = module->entry_addr;
		delete [] temp;
	}
	in.close();
	return true;
}

Module *__KernelLoadModule(u8 *fileptr, SceKernelLMOption *options, std::string *error_string)
{
	Module *module = 0;
	// Check for PBP
	if (memcmp(fileptr, "\0PBP", 4) == 0)
	{
		// PBP!
		u32 version;
		memcpy(&version, fileptr + 4, 4);
		u32 offset0, offsets[16];
		int numfiles;

		memcpy(&offset0, fileptr + 8, 4);
		numfiles = (offset0 - 8)/4;
		offsets[0] = offset0;
		for (int i = 1; i < numfiles; i++)
			memcpy(&offsets[i], fileptr + 12 + 4*i, 4);
		module = __KernelLoadELFFromPtr(fileptr + offsets[5], PSP_GetDefaultLoadAddress(), error_string);
	}
	else
	{
		module = __KernelLoadELFFromPtr(fileptr, PSP_GetDefaultLoadAddress(), error_string);
	}

	return module;
}

void __KernelStartModule(Module *m, int args, const char *argp, SceKernelSMOption *options)
{
	__KernelSetupRootThread(m->GetUID(), args, argp, options->priority, options->stacksize, options->attribute);
	mainModuleID = m->GetUID();
	//TODO: if current thread, put it in wait state, waiting for the new thread
}


u32 __KernelGetModuleGP(SceUID module)
{
	u32 error;
	Module *m = kernelObjects.Get<Module>(module,error);
	if (m)
	{
		return m->gp_value;
	}
	else
	{
		return 0;
	}
}

bool __KernelLoadExec(const char *filename, SceKernelLoadExecParam *param, std::string *error_string)
{
	// Wipe kernel here, loadexec should reset the entire system
	__KernelInit();
	
	PSPFileInfo info = pspFileSystem.GetFileInfo(filename);

	u32 handle = pspFileSystem.OpenFile(filename, FILEACCESS_READ);

	u8 *temp = new u8[(int)info.size];

	pspFileSystem.ReadFile(handle, temp, (size_t)info.size);

	Module *m = __KernelLoadModule(temp, 0, error_string);

	if (!m) {
		ERROR_LOG(LOADER, "Failed to load module %s", filename);
		return false;
	}

	mipsr4k.pc = m->entry_addr;

	INFO_LOG(LOADER, "Module entry: %08x", mipsr4k.pc);

	delete [] temp;

	pspFileSystem.CloseFile(handle);

	SceKernelSMOption option;
	option.size = sizeof(SceKernelSMOption);
	option.attribute = PSP_THREAD_ATTR_USER;
	option.mpidstack = 2;
	option.priority = 0x20;
	option.stacksize = 0x40000;	// crazy? but seems to be the truth

	__KernelStartModule(m, (u32)strlen(filename), filename, &option);

	__KernelStartIdleThreads();
	return true;
}

//TODO: second param
void sceKernelLoadExec()
{
	const char *filename = Memory::GetCharPointer(PARAM(0));
	SceKernelLoadExecParam *param = 0;
	if (PARAM(1))
	{
		param = (SceKernelLoadExecParam*)Memory::GetPointer(PARAM(1));
	}

	PSPFileInfo info = pspFileSystem.GetFileInfo(filename);
	
	if (!info.exists) {
		ERROR_LOG(LOADER, "sceKernelLoadExec(%s, ...): File does not exist", filename);
		RETURN(SCE_KERNEL_ERROR_NOFILE);
		return;
	}

	s64 size = (s64)info.size;
	if (!size)
	{
		ERROR_LOG(LOADER, "sceKernelLoadExec(%s, ...): File is size 0", filename);
		RETURN(SCE_KERNEL_ERROR_ILLEGAL_OBJECT);
		return;
	}

	DEBUG_LOG(HLE,"sceKernelLoadExec(name=%s,...)", filename);
	std::string error_string;
	if (!__KernelLoadExec(filename, param, &error_string)) {
		ERROR_LOG(HLE, "sceKernelLoadExec failed: %s", error_string.c_str());
	}
}

u32 sceKernelLoadModule(const char *name, u32 flags)
{
	PSPFileInfo info = pspFileSystem.GetFileInfo(name);
	std::string error_string;
	s64 size = (s64)info.size;

	if (!info.exists) {
		ERROR_LOG(LOADER, "sceKernelLoadModule(%s, %08x): File does not exist", name, flags);
		return SCE_KERNEL_ERROR_NOFILE;
	}

	if (!size)
	{   
		ERROR_LOG(LOADER, "sceKernelLoadModule(%s, %08x): Module file is size 0", name, flags);
		return SCE_KERNEL_ERROR_ILLEGAL_OBJECT;
	}

	SceKernelLMOption *lmoption = 0;
	int position = 0;
	// TODO: Use position to decide whether to load high or low
	if (PARAM(2))
	{
		SceKernelLMOption *lmoption = (SceKernelLMOption *)Memory::GetPointer(PARAM(2));
	}

	std::string fn(name);
	int slashPos = fn.rfind('/');
	if (slashPos != -1)
	fn = fn.substr(slashPos + 1);
		
	// Check for module blacklist - we don't allow games to load these modules from disc
	// as we have HLE implementations and the originals won't run in the emu because they
	// directly access hardware or for other reasons.
	bool blacklisted = false;
	std::string guessedModuleName = fn.substr(0, fn.size() - 4);
	if (GetModuleIndex(guessedModuleName.c_str()) >= 0) {
		blacklisted = true;
	}
	for (int i = 0; i < ARRAY_SIZE(blacklistedModules); i++) {
		if (guessedModuleName == blacklistedModules[i]) {
			blacklisted = true;
			break;
		}
	}

	Module *module = 0;
	if (!blacklisted) {
		u8 *temp = new u8[(int)size];
		u32 handle = pspFileSystem.OpenFile(name, FILEACCESS_READ);
		pspFileSystem.ReadFile(handle, temp, (size_t)size);
		module = __KernelLoadELFFromPtr(temp, 0, &error_string);
		delete [] temp;                                                                                                                                                                                
		pspFileSystem.CloseFile(handle);
	}

	if (!module) {
		// Lie about successfully loading blacklisted modules.
		// Plus: Temporary hack until we have decryption: Lie that we succeeded. Helps compat.
		if (error_string == "Executable encrypted - not yet supported" || blacklisted)
		{
			NOTICE_LOG(LOADER, "Module %s is blacklisted or encrypted - we lie about success", name);
			return 1;
		}

		ERROR_LOG(LOADER, "Failed to load module %s", name);
		return SCE_KERNEL_ERROR_ILLEGAL_OBJECT;
	}

	if (lmoption) {
		INFO_LOG(HLE,"%i=sceKernelLoadModule(name=%s,flag=%08x,%08x,%08x,%08x,position = %08x)",
			module->GetUID(),name,flags,
			lmoption->size,lmoption->mpidtext,lmoption->mpiddata,lmoption->position);
	}
	else
	{
		INFO_LOG(HLE,"%i=sceKernelLoadModule(name=%s,flag=%08x,(...))", module->GetUID(), name, flags);
	}

	return module->GetUID();
}

void sceKernelStartModule()
{
	int id = PARAM(0);
	int argsize = PARAM(1);
	u32 argptr = PARAM(2);
	u32 ptrReturn = PARAM(3);
	if (PARAM(4)) {
		SceKernelSMOption *smoption = (SceKernelSMOption*)Memory::GetPointer(PARAM(4));
	}
	ERROR_LOG(HLE,"UNIMPL sceKernelStartModule(%d,asize=%08x,aptr=%08x,retptr=%08x,...)",
		id,argsize,argptr,ptrReturn);
	RETURN(0);
}

void sceKernelStopModule()
{
	ERROR_LOG(HLE,"UNIMPL sceKernelStopModule");
	RETURN(0);

}

void sceKernelUnloadModule()
{
	ERROR_LOG(HLE,"UNIMPL sceKernelUnloadModule");
	RETURN(0);

}

void sceKernelGetModuleIdByAddress()
{
	ERROR_LOG(HLE,"UNIMPL sceKernelGetModuleIdByAddress(%08x)", PARAM(0));
	if (PARAM(0) == 0x08800000)
		RETURN(mainModuleID);
	else
		RETURN(0);
}

void sceKernelGetModuleId()
{
	ERROR_LOG(HLE,"UNIMPL sceKernelGetModuleId");
	RETURN(0);
}

void sceKernelFindModuleByName()
{
	ERROR_LOG(HLE,"UNIMPL sceKernelFindModuleByName()");
	RETURN(1);
}



const HLEFunction ModuleMgrForUser[] = 
{
	{0x977DE386,&WrapU_CU<sceKernelLoadModule>,"sceKernelLoadModule"},
	{0xb7f46618,0,"sceKernelLoadModuleByID"},
	{0x50F0C1EC,&sceKernelStartModule,"sceKernelStartModule"},
	{0xD675EBB8,&sceKernelExitGame,"sceKernelSelfStopUnloadModule"}, //HACK
	{0xd1ff982a,&sceKernelStopModule,"sceKernelStopModule"},
	{0x2e0911aa,&sceKernelUnloadModule,"sceKernelUnloadModule"},
	{0x710F61B5,0,"sceKernelLoadModuleMs"},
	{0xF9275D98,0,"sceKernelLoadModuleBufferUsbWlan"}, ///???
	{0xCC1D3699,0,"sceKernelStopUnloadSelfModule"},
	{0x748CBED9,0,"sceKernelQueryModuleInfo"},
	{0xd8b73127,&sceKernelGetModuleIdByAddress, "sceKernelGetModuleIdByAddress"},
	{0xf0a26395,&sceKernelGetModuleId, "sceKernelGetModuleId"},
	{0x8f2df740,0,"sceKernel_ModuleMgr_8f2df740"},
};


void Register_ModuleMgrForUser()
{
	RegisterModule("ModuleMgrForUser", ARRAY_SIZE(ModuleMgrForUser), ModuleMgrForUser);
}
