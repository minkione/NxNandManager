#include "NxStorage.h"


NxStorage::NxStorage(const char* storage)
{
	path = storage;
	pathLPWSTR = convertCharArrayToLPWSTR(storage);
	type = UNKNOWN;
	size = 0;
	isDrive = FALSE;
	pdg = { 0 };
	partCount = 0;
	firstPartion = NULL;

	this->InitStorage();
}

// Initialize and retrieve storage information
void NxStorage::InitStorage()
{
	HANDLE hDevice = INVALID_HANDLE_VALUE;
	BOOL bResult = FALSE;
	DWORD junk = 0;

	hDevice = CreateFileW(pathLPWSTR,
		0,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		0,
		NULL);

	if (hDevice != INVALID_HANDLE_VALUE)
	{
		// Get drive geometry
		if (DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &pdg, sizeof(pdg), &junk, (LPOVERLAPPED)NULL))
		{
			isDrive = TRUE;
			size = pdg.Cylinders.QuadPart * (ULONG)pdg.TracksPerCylinder * (ULONG)pdg.SectorsPerTrack * (ULONG)pdg.BytesPerSector;
		}
	}
	CloseHandle(hDevice);

	// Open new handle for read
	HANDLE hStorage;
	hStorage = CreateFileW(pathLPWSTR, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);

	if (hStorage == INVALID_HANDLE_VALUE)
	{
		CloseHandle(hStorage);
		return;
	}

	DWORD bytesRead = 0;
	BYTE buff[0x200];
	DWORD dwPtr = SetFilePointer(hStorage, 0x0400, NULL, FILE_BEGIN);
	if (dwPtr != INVALID_SET_FILE_POINTER)
	{
		BYTE sbuff[12];
		ReadFile(hStorage, buff, 0x200, &bytesRead, NULL);
		memcpy(sbuff, &buff[0x130], 12);
		// Look for boot_data_version + block_size_log2 + page_size_log2 at offset 0x0530
		if (0 != bytesRead && hexStr(sbuff, 12) == "010021000e00000009000000")
		{
			type = BOOT0;
		}
	}

	if (type == UNKNOWN)
	{
		dwPtr = SetFilePointer(hStorage, 0x1200, NULL, FILE_BEGIN);
		if (dwPtr != INVALID_SET_FILE_POINTER)
		{
			BYTE sbuff[4];
			ReadFile(hStorage, buff, 0x200, &bytesRead, NULL);
			memcpy(sbuff, &buff[0xD0], 4);
			// Look for "PK11" magic offset at offset 0x12D0
			if (0 != bytesRead && hexStr(sbuff, 4) == "504b3131")
			{
				type = BOOT1;
			}
		}
	}

	if (type == UNKNOWN)
	{
		dwPtr = SetFilePointer(hStorage, 0x200, NULL, FILE_BEGIN);
		if (dwPtr != INVALID_SET_FILE_POINTER)
		{
			BYTE buffGpt[0x4200];
			BYTE sbuff[15];
			ReadFile(hStorage, buffGpt, 0x4200, &bytesRead, NULL);
			memcpy(sbuff, &buffGpt[0x98], 15);
			// Look for "P R O D I N F O" string in GPT at offet 0x298
			if (0 != bytesRead && hexStr(sbuff, 15) == "500052004f00440049004e0046004f")
			{
				type = RAWNAND;
				this->ParseGpt(buffGpt);
			}
		}
	}

	// Get size
	LARGE_INTEGER Lsize;
	if (!isDrive)
	{
		if (!GetFileSizeEx(hStorage, &Lsize))
		{
			//printf("GetFileSizeEx failed. %s \n", GetLastErrorAsString().c_str());
			CloseHandle(hStorage);
		} else {
			size = Lsize.QuadPart;
		}
	}
	CloseHandle(hStorage);
}

// Parse GUID Partition Table
BOOL NxStorage::ParseGpt(unsigned char* gptHeader)
{
	GptHeader *hdr = (GptHeader *)gptHeader;

	// Iterate partitions backwards (from GPT header) 
	for (int i = hdr->num_part_ents - 1; i >= 0; --i)
	{
		// Get GPT entry
		GptEntry *ent = (GptEntry *)(gptHeader + (hdr->part_ent_lba - 1) * NX_EMMC_BLOCKSIZE + i * sizeof(GptEntry));

		// Set new partition
		partCount++;
		GptPartition *part = (GptPartition *)malloc(sizeof(GptPartition));
		part->lba_start = ent->lba_start;
		part->lba_end = ent->lba_end;
		part->attrs = ent->attrs;
		char name[37];
		for (u32 i = 0; i < 36; i++)
		{
			part->name[i] = ent->name[i];
		}
		part->name[36] = '0';

		// Add partition to linked list
		part->next = firstPartion;
		firstPartion = part;
	}

	return hdr->num_part_ents > 0 ? TRUE : FALSE;
}

// Get handle to drive/file for read/write operation
// & set pointers to a specific partition if specified
int NxStorage::GetIOHandle(HANDLE* hHandle, DWORD dwDesiredAccess, const char* partition, u64 *bytesToRead)
{
	if (dwDesiredAccess == GENERIC_READ)
	{
		// Get handle for reading
		*hHandle = CreateFileW(pathLPWSTR, GENERIC_READ, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	} else {
		// Get handle for writing
		*hHandle = CreateFileW(pathLPWSTR, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			isDrive ? OPEN_EXISTING : CREATE_ALWAYS, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	}

	if (*hHandle == INVALID_HANDLE_VALUE)
	{
		return -3;
	}

	if (NULL != partition && NULL != bytesToRead && type == RAWNAND)
	{
		// Iterate GPT entry
		GptPartition *cur = firstPartion;
		while (NULL != cur)
		{
			// If partition exists in i/o stream
			if (strncmp(cur->name, partition, strlen(partition)) == 0)
			{
				// Try to set pointers
				if (SetFilePointer(*hHandle, cur->lba_start * NX_EMMC_BLOCKSIZE, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
				{
					*bytesToRead = (cur->lba_end - cur->lba_start + 1) * NX_EMMC_BLOCKSIZE;
					return 0;
				} else {
					return -2;
				}
				break;
			}
			cur = cur->next;
		}
		return -1;
	}
	return 0;
}

// Dump raw data from hHandleIn to hHandleOut.  This function must be called recursively until it returns FALSE;
BOOL NxStorage::dumpStorage(HANDLE* hHandleIn, HANDLE* hHandleOut, u64* readAmount, u64* writeAmount, u64 bytesToWrite, HCRYPTHASH* hHash)
{
	BYTE buffer[DEFAULT_BUFF_SIZE], wbuffer[DEFAULT_BUFF_SIZE];
	u64 buffSize = DEFAULT_BUFF_SIZE;
	DWORD bytesRead = 0, bytesWritten = 0, bytesWrite = 0;

	if (NULL != bytesToWrite && *writeAmount >= bytesToWrite)
	{
		return FALSE;
	}

	// Read buffer
	if (!ReadFile(*hHandleIn, buffer, buffSize, &bytesRead, NULL))
	{
		return FALSE;
	}
	if (0 == bytesRead)
	{
		return FALSE;
	}
	*readAmount += (DWORD) bytesRead;

	if (NULL != bytesToWrite && *readAmount > bytesToWrite)
	{
		// Adjust write buffer
		memcpy(wbuffer, &buffer[0], buffSize - (*readAmount - bytesToWrite));
		bytesWrite = buffSize - (*readAmount - bytesToWrite);
		if (bytesWrite == 0)
		{
			return FALSE;
		}
	} else {
		// Copy read to write buffer
		memcpy(wbuffer, &buffer[0], buffSize);
		bytesWrite = buffSize;
	}

	if (NULL != hHash)
	{
		CryptHashData(*hHash, wbuffer, bytesWrite, 0);
	}

	if(!WriteFile(*hHandleOut, wbuffer, bytesWrite, &bytesWritten, NULL))
	{
		printf("Error during write operation : %s \n", GetLastErrorAsString().c_str());
		return FALSE;
	} else {
		*writeAmount += (DWORD) bytesWritten;
	}
	return TRUE;
}

const char* NxStorage::GetNxStorageTypeAsString()
{
	switch (type)
	{
	case BOOT0:
		return "BOOT0";
		break;
	case BOOT1:
		return "BOOT1";
		break;
	case RAWNAND:
		return "RAWNAND";
		break;
	case INVALID:
		return "INVALID";
		break;
	default:
		return "UNKNOWN";
		break;
	}
}