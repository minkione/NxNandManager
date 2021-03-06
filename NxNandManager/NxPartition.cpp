/*
 * Copyright (c) 2019 eliboa
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "NxPartition.h"

// Constructor
NxPartition::NxPartition(NxStorage *p, const char* p_name, u32 lba_start, u32 lba_end, u64 attrs)
{
    dbg_printf("NxPartition::NxPartition(parent, %s, lba_start=%I32d, lba_end=%I32d)\n", p_name, lba_start, lba_end);
    parent = p;
    nxHandle = parent->nxHandle;
    
    // Get partition name length
    int name_len = strlen(p_name) + 1;

    // Init member variables
    for (int i = 0; i < 37 || i < strlen(p_name) - 1; i++)
        m_name[i] = p_name[i];
    m_lba_start = lba_start;
    m_lba_end = lba_end;
    m_attrs = attrs;
    m_isEncrypted = false;
    m_bad_crypto = false;
    m_isValidPartition = false;
    m_type = UNKNOWN;
    nxCrypto = nullptr;

    for( NxPart part : NxPartArr )
    {
        std::string sm_name(m_name);
        std::transform(sm_name.begin(), sm_name.end(), sm_name.begin(), ::toupper);
        std::string sp_name = part.name;
        std::transform(sp_name.begin(), sp_name.end(), sp_name.begin(), ::toupper);

        if(!sm_name.compare(sp_name))
        {
            m_isValidPartition = true;
            m_isEncrypted = part.isEncrypted;
            m_type = part.type;            
            nxPart_info = part;

            // Look for decrypted partition
            if (parent != nullptr && m_isEncrypted)
            {
                u8 buff[NX_BLOCKSIZE];
                u64 off = lba_start * NX_BLOCKSIZE + part.magic_off;                
                int remain = off % NX_BLOCKSIZE; // Block align
                if (parent->nxHandle->read(off - remain, buff, nullptr, NX_BLOCKSIZE))
                {
                    if (!memcmp(&buff[remain], part.magic, strlen(part.magic)))
                        m_isEncrypted = false;
                }
            }
        }
    }

    if (parent != nullptr)
    {
        parent->partitions.push_back(this);        
    }
}

NxPartition::~NxPartition()
{
    if (nullptr != nxCrypto)
        delete nxCrypto;
}

bool NxPartition::setCrypto(char* crypto, char* tweak)
{
    if (!nxPart_info.isEncrypted)
        return false;

    if (nullptr != nxCrypto)
        delete nxCrypto;

    //dbg_printf("NxPartition::setCrypto() for %s\n", partitionName().c_str());
    
    m_bad_crypto = false;
    nxCrypto = new NxCrypto(crypto, tweak);   
    nxHandle->initHandle(isEncryptedPartition() ? DECRYPT : NO_CRYPTO, this);

    // Validate first cluster
    unsigned char first_cluster[CLUSTER_SIZE];
    if (nxPart_info.magic != nullptr && nxHandle->read(first_cluster, nullptr, CLUSTER_SIZE))
    {
        // Do magic
        if (memcmp(&first_cluster[nxPart_info.magic_off], nxPart_info.magic, strlen(nxPart_info.magic)))
            m_bad_crypto = true;
        else if(is_in(m_type, {USER, SYSTEM}))
        {
            freeSpace = fat32_getFreeSpace();
        }

    }
    
    //dbg_printf("NxPartition::setCrypto() ends %s %s\n", partitionName().c_str(), m_bad_crypto ? "BAD CRYPTO" : "GOOD CRYPTO");
    return m_bad_crypto ? false : true;
}

std::string NxPartition::partitionName()
{
    return std::string(m_name);
}

u32 NxPartition::lbaStart()
{
    return m_lba_start;
}

u32 NxPartition::lbaEnd()
{
    return m_lba_end;
}

u64 NxPartition::size() 
{
    if(m_lba_end - m_lba_start > 0)
        return (u64)(m_lba_end - m_lba_start + 1) * NX_BLOCKSIZE;
    else 
        return 0;
}

bool NxPartition::isValidPartition()
{
    return m_isValidPartition;
}

bool NxPartition::isEncryptedPartition() 
{    
    return m_isEncrypted;
}

int NxPartition::dumpToFile(const char *file, int crypto_mode, void(*updateProgress)(ProgressInfo*))
{
    // Crypto check
    if (crypto_mode == DECRYPT && !m_isEncrypted)
        return ERR_CRYPTO_DECRYPTED_YET;
    if (crypto_mode == ENCRYPT && m_isEncrypted)
        return ERR_CRYPTO_ENCRYPTED_YET;

    // Test if file already exists
    std::ifstream infile(file);
    if (infile.good())
    {
        infile.close();
        return ERR_FILE_ALREADY_EXISTS;
    }

    // Open new stream for output file
    std::ofstream out_file = std::ofstream(file, std::ofstream::binary);

    // Lock volume (drive only)
    if (parent->isDrive())
        nxHandle->lockVolume();

    // Init input handle
    nxHandle->initHandle(crypto_mode, this);

    // Set new buffer
    int buff_size = nxHandle->getDefaultBuffSize();
    BYTE* buffer = new BYTE[buff_size];
    memset(buffer, 0, buff_size);
    DWORD bytesRead = 0;

    // Init progress info        
    ProgressInfo pi;
    pi.mode = COPY;
    pi.storage_name = partitionName();
    pi.begin_time = std::chrono::system_clock::now();
    pi.bytesCount = 0;
    pi.bytesTotal = size();
    if(nullptr != updateProgress) updateProgress(&pi);

    // Copy
    while (nxHandle->read(buffer, &bytesRead, buff_size))
    {
        if(stopWork) return userAbort();

        if (!out_file.write((char *)&buffer[0], bytesRead))
            break;

        pi.bytesCount += bytesRead;
        if(nullptr != updateProgress) updateProgress(&pi);
    }

    // Clean & unlock volume
    out_file.close();
    delete[] buffer;
    if (parent->isDrive())
        nxHandle->unlockVolume();

    // Check completeness
    if (pi.bytesCount != pi.bytesTotal)
        return ERR_WHILE_COPY;

    // Compute & compare md5 hashes
    if (crypto_mode == MD5_HASH)
    {
        // Get checksum for input
        HCRYPTHASH in_hash = nxHandle->md5Hash();
        std::string in_sum = BuildChecksum(in_hash);
        
        // Set new NxStorage for output
        NxStorage out_storage = NxStorage(file);

        // Init Progress Info
        pi.mode = MD5_HASH;
        pi.begin_time = std::chrono::system_clock::now();
        pi.bytesCount = 0;
        pi.bytesTotal = out_storage.size();
        pi.elapsed_seconds = 0;
        if(nullptr != updateProgress) updateProgress(&pi);

        // Hash output file
        while (!out_storage.nxHandle->hash(&pi.bytesCount))
        {
            if(stopWork) return userAbort();
            if(nullptr != updateProgress) updateProgress(&pi);
        }

        // Check completeness
        if (pi.bytesCount != pi.bytesTotal)
            return ERR_MD5_COMPARE;

        // Get checksum for output
        HCRYPTHASH out_hash = out_storage.nxHandle->md5Hash();
        std::string out_sum = BuildChecksum(out_hash);

        // Compare checksums
        if (in_sum.compare(out_sum))
            return ERR_MD5_COMPARE;
    }

    return SUCCESS;
}

int NxPartition::restoreFromStorage(NxStorage* input, int crypto_mode, void(*updateProgress)(ProgressInfo*))
{
    // Get handle to input NxPartition
    NxPartition *input_part = input->getNxPartition(m_type);

    // Controls
    if (nullptr == input_part)
        return ERR_IN_PART_NOT_FOUND;

    if (crypto_mode == DECRYPT && !input_part->isEncryptedPartition())
        return ERR_CRYPTO_DECRYPTED_YET;

    if (crypto_mode == ENCRYPT && input_part->isEncryptedPartition())
        return ERR_CRYPTO_ENCRYPTED_YET;

    if (not_in(crypto_mode, { ENCRYPT, DECRYPT }) && isEncryptedPartition() && !input_part->isEncryptedPartition())
        return ERR_RESTORE_CRYPTO_MISSING;

    if (not_in(crypto_mode, { ENCRYPT, DECRYPT }) && !isEncryptedPartition() && input_part->isEncryptedPartition())
        return ERR_RESTORE_CRYPTO_MISSIN2;

    if (input_part->size() > size())
        return ERR_IO_MISMATCH;

    // Lock output volume
    if (parent->isDrive())
        nxHandle->lockVolume();

    // Lock input volume
    if (input->isDrive())
        input->nxHandle->lockVolume();
    
    // Init handles for both input & output
    input->nxHandle->initHandle(crypto_mode, input_part);
    this->nxHandle->initHandle(NO_CRYPTO, this);

    // Set new buffer
    int buff_size = nxHandle->getDefaultBuffSize();
    BYTE* buffer = new BYTE[buff_size];
    memset(buffer, 0, buff_size);
    DWORD bytesRead = 0, bytesWrite = 0;

    // Init progress info    
    ProgressInfo pi;
    pi.mode = RESTORE;
    pi.storage_name = partitionName();
    pi.begin_time = std::chrono::system_clock::now();
    pi.bytesCount = 0;
    pi.bytesTotal = input_part->size();
    if(nullptr != updateProgress) updateProgress(&pi);

    while(input->nxHandle->read(buffer, &bytesRead, buff_size))
    {
        if(stopWork) return userAbort();

        if (!this->nxHandle->write(buffer, &bytesWrite, bytesRead))
            break;

        pi.bytesCount += bytesWrite;
        if(nullptr != updateProgress) updateProgress(&pi);
    }

    // Clean & unlock volume
    delete[] buffer;
    if (parent->isDrive())
        nxHandle->unlockVolume();
    if (input->isDrive())
        input->nxHandle->unlockVolume();

    // Check completeness
    if (pi.bytesCount != pi.bytesTotal)
        return ERR_WHILE_COPY;

    return SUCCESS;
}

// Get fat32 entries for given path
// If path is a file, only one entry is pushed back to entries vector
// Returns false when directory or file does not exist
bool NxPartition::fat32_dir(std::vector<fat32::dir_entry> *entries, const char *path)
{
    entries->clear();
    
    if (not_in(m_type, { SAFE, SYSTEM, USER }))
        return false;

    if (m_isEncrypted && (m_bad_crypto || nullptr == nxCrypto))
        return false;

    NxHandle *nxHandle = parent->nxHandle;
    nxHandle->initHandle(isEncryptedPartition() ? DECRYPT : NO_CRYPTO, this);
    BYTE buff[CLUSTER_SIZE];

    // Read first cluster
    if (!nxHandle->read(buff, nullptr, CLUSTER_SIZE))
        return false;
    
    // Get root address
    fat32::fs_attr fs;
    fat32::read_boot_sector(buff, &fs);
    u64 root_addr = (fs.num_fats * fs.fat_size * fs.bytes_per_sector) + (fs.reserved_sector_count * fs.bytes_per_sector);
    

    // TEST
    /*
    fat32::boot_sector *bs = (fat32::boot_sector*)(buff); 
    dbg_printf("fs = \n%s\n", hexStr((u8*)buff, 0x60).c_str());
    dbg_printf("bs = \n%s\n", hexStr((u8*)bs, 0x60).c_str());     
    dbg_printf("fs.bytes_per_sector = %d\n", fs.bytes_per_sector);
    dbg_printf("bs->bytes_per_sector = %d\n", *(u16*)&bs->bytes_per_sector);
    dbg_printf("bs->bytes_per_sector = %s, sizeof %I32d / u16 %I32d\n", hexStr((u8*)&bs->bytes_per_sector, 
        sizeof(bs->bytes_per_sector)).c_str(), sizeof(bs->bytes_per_sector), sizeof(unsigned short));
    dbg_printf("bs = \n%s\n", hexStr((u8*)&bs + 0xB, 0x10).c_str());
    dbg_printf("fs.sectors_per_cluster = %d\n", fs.sectors_per_cluster);
    dbg_printf("bs->sectors_per_cluster = %d\n", bs->sectors_per_cluster);
    dbg_printf("fs.reserved_sector_count = %d\n", fs.reserved_sector_count);    
    dbg_printf("bs->reserved_sector_count = %d\n", bs->reserved_sector_count);
    bs->reserved_sector_count = 3;
    fat32::read_boot_sector(buff, &fs);
    dbg_printf("fs.reserved_sector_count(2) = %d\n", fs.reserved_sector_count);
    dbg_printf("bs->reserved_sector_count(2) = %d\n", bs->reserved_sector_count);
    dbg_printf("fs.num_fats = %d\n", fs.num_fats);
    dbg_printf("bs->num_fats = %d\n", bs->num_fats);
    dbg_printf("fs.sectors_count = %I32d\n", fs.sectors_count);
    dbg_printf("bs->sectors_count = %I32d\n", bs->sectors_count);
    dbg_printf("fs.label = %s\n", fs.label);
    dbg_printf("bs->label = %s\n", bs->label);
    */

    // Read root cluster
    if (!nxHandle->read(root_addr, buff, nullptr, CLUSTER_SIZE))
        return false;
    
    // Get root entries    
    fat32::parse_dir_table(buff, entries);

    // path param is root dir
    if (entries->size() > 0 && (nullptr == path || (path[0] == '/' && strlen(path) == 1)))
        return true;

    // Explore path, one directory (*dir) after each other, from root
    char *cdir = strdup(path), *dir;
    while ((dir = strtok(cdir, "/")) != nullptr)
    {
        cdir = nullptr;
        bool found = false;
        for (fat32::dir_entry dir_entry : *entries)
        {
            // current dir found in entries
            if (!strcmp(dir, dir_entry.filename.c_str()))
            {
                // path is a file
                if (!dir_entry.is_directory)
                {
                    dir_entry.data_offset = fs.bytes_per_sector * ((dir_entry.entry.first_cluster - 2) * fs.sectors_per_cluster) + (fs.num_fats * fs.fat_size * fs.bytes_per_sector) + (fs.reserved_sector_count * fs.bytes_per_sector);                    
                    entries->clear();
                    entries->push_back(dir_entry);
                    return true;
                }

                // Read cluster for directory
                u64 next_cluster_off = fs.bytes_per_sector * ((dir_entry.entry.first_cluster - 2) * fs.sectors_per_cluster) + root_addr;
                if (!nxHandle->read(next_cluster_off, buff, nullptr, CLUSTER_SIZE))
                    return false;

                // Get next (or last) fat entries
                fat32::parse_dir_table(buff, entries);
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

// Get free space from free clusters count in FAT
u64 NxPartition::fat32_getFreeSpace()
{
    nxHandle->initHandle(isEncryptedPartition() ? DECRYPT : NO_CRYPTO, this);
    BYTE buff[CLUSTER_SIZE];

    // Read first cluster
    if (!nxHandle->read(buff, nullptr, CLUSTER_SIZE))
        return 0;
    
    // Get fs attributes from boot sector
    fat32::fs_attr fs;
    fat32::read_boot_sector(buff, &fs);

    u32 cluster_free_count = 0, cluster_count = 0, first_empty_cluster = 0;
    int cluster_num = fs.fat_size * fs.bytes_per_sector / CLUSTER_SIZE;
    unsigned char free_cluster[4] = { 0x00,0x00,0x00,0x00 };

    // Iterate cluster map
    for (int i(0); i < cluster_num; i++)
    {
        nxHandle->read(buff, nullptr, CLUSTER_SIZE);
        int count = 0;
        while (count < CLUSTER_SIZE)
        {
            cluster_count++;
            if (!memcmp(&buff[count], free_cluster, 4)) 
            {
                cluster_free_count++;
                if (!first_empty_cluster) first_empty_cluster = cluster_count;
            }
            else first_empty_cluster = 0;
            count += 4;
        }
    }

    
    dbg_printf("fs.bytes_per_sector = %I32d\n", fs.bytes_per_sector);
    dbg_printf("fs.sectors_per_cluster = %I32d\n", fs.sectors_per_cluster);
    dbg_printf("fs.sectors_count = %I32d (%s)\n", fs.sectors_count, GetReadableSize((u64)fs.sectors_count * NX_BLOCKSIZE).c_str());
    dbg_printf("fs.info_sector = %I32d\n", fs.info_sector);
    dbg_printf("fs.num_fats = %I32d\n", fs.num_fats);
    dbg_printf("fs.reserved_sector_count = %I32d\n", fs.reserved_sector_count);
    dbg_printf("fs.fat_size = %I32d (%s)\n", fs.fat_size, GetReadableSize((u64)fs.fat_size * 0x200).c_str());
    dbg_printf("clusters size for fs.fat_size = %I32d\n", fs.fat_size / 0x20);
    //u32 cl_add = fs.fat_size / 0x20 * 0x1000;
    u32 cl_add = fs.fat_size / 0x200 * 0x4000;
    u32 u_size = (m_lba_end - m_lba_start + 1);
    dbg_printf("clusters adresses for fs.fat_size = %I32d (%s)\n", n2hexstr(cl_add, 10).c_str(), GetReadableSize((u64)cl_add * 0x4000).c_str());
    dbg_printf("%s size is %I32d sectors (%s)\n", partitionName().c_str(), u_size, GetReadableSize((u64)u_size * 0x200).c_str());
    dbg_printf("clusters to address %I32d (%s)\n", u_size / 0x20, GetReadableSize((u64)u_size / 0x20 * 0x4000).c_str());
    dbg_printf("size of fat in clusters %I32d (%s)\n", u_size / 0x20 / 0x1000, GetReadableSize((u64)u_size / 0x20 / 0x1000 * 0x4000).c_str());
    dbg_printf("size of fat in sectors %I32d (%s)\n", u_size / 0x1000, GetReadableSize((u64)u_size / 0x1000 * 0x200).c_str());
    
    u32 free_cluster_count = cluster_count - first_empty_cluster;
    dbg_printf("%s first_empty_cluster is %I32d / %I32d (%s available)\n", m_name, first_empty_cluster, cluster_count, GetReadableSize((u64)free_cluster_count * CLUSTER_SIZE).c_str());

    return (u64)cluster_free_count * CLUSTER_SIZE;
}

void NxPartition::clearHandles()
{
    p_ofstream.close();
}
