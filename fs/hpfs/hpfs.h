#ifndef __HPFS_H
#define __HPFS_H

// Note that OS/2 uses LSN (linear sector number), we use LBA (linear block number, because that's what the ATA spec uses). They're identical.
// All LSNs/LBAs are relative to the partition start, with one exception -- hidden_sectors in the FAT16 BPB.

#include <stdint.h>

// https://en.wikipedia.org/wiki/BIOS_parameter_block#FAT12_/_FAT16_/_HPFS
struct hpfs_bpb {
    uint8_t jmpboot[3];
    uint8_t oem[8];

    // FAT12 BPB
    uint16_t bytes_per_sector; // 0x0B
    uint8_t sectors_per_cluster; // 0x0D
    uint16_t reserved_sectors; // 0x0E
    uint8_t number_of_fats; // 0x10
    uint16_t root_dir_entries; // 0x11
    uint16_t total_sectors16; // 0x13
    uint8_t media_desc; // 0x15
    uint16_t sectors_per_fat; // 0x16

    // FAT16 BPB
    uint16_t spt; // 0x18
    uint16_t heads; // 0x1A
    uint32_t hidden_sectors; // 0x1C
    uint32_t total_sectors32; // 0x20

    // HPFS BPB
    uint8_t drive_number; // 0x24
    uint8_t flags; // 0x25
    uint8_t boot_sig; // 0x26
    uint32_t serial; // 0x27 unaligned >:(
    uint8_t volume_label[11]; // 0x2B
    uint8_t fstype[8]; // 0x36 "HPFS    "

    // Some code
    uint8_t code[448];

    // Boot signature: 0x55 0xAA
    uint8_t boot_magic[2];
} __attribute__((packed));

// http://www.edm2.com/0501/hpfs3.html
struct hpfs_superblock {
#define HPFS_SUPER_SIG0 0xF995E849
#define HPFS_SUPER_SIG1 0xFA53E9C5
    uint32_t signature[2];

    uint8_t version; // 0x08
    uint8_t functional_ver; // 0x09
    uint16_t __dummy; // 0x0A

    uint32_t rootdir_fnode; // 0x0C, LBA
    uint32_t sectors_in_partition; // 0x10
    uint32_t bad_sector_count; // 0x14
    uint32_t list_bitmap_secs; // 0x18, LBA
    uint32_t bitmap_secs_spare; // 0x1C
    uint32_t list_bad_secs; // 0x20, LBA
    uint32_t bad_secs_spare; // 0x24
    uint32_t chkdsk_last_run; // 0x28
    uint32_t last_optimized; // 0x2C
    uint32_t dir_band_sectors; // 0x30, LBA
    uint32_t dir_band_start_sec; // 0x34, LBA
    uint32_t dir_band_end_sec; // 0x38
    uint32_t dir_band_bitmap; // 0x3C
    uint32_t __dummy2[8]; // 0x40
    uint32_t first_uid_sec; // 0x60, HPFS386 only
} __attribute__((packed));

struct hpfs_spareblock {
#define HPFS_SPARE_SIG0 0xF9911849
#define HPFS_SPARE_SIG1 0xFA5229C5
    uint32_t signature[2];

#define HPFS_STATUS_OLDFS (1 << 7) // Written by old ifs
#define HPFS_STATUS_FASTFORMAT (1 << 5) // Fast format flag
#define HPFS_STATUS_BAD_BITMAP (1 << 4)
#define HPFS_STATUS_BAD_SECTOR (1 << 3)
#define HPFS_STATUS_HOTFIX_SECS_USED (1 << 2)
#define HPFS_STATUS_SPARE_DIRBLKS_USED (1 << 1)
#define HPFS_STATUS_DIRTY (1 << 0) // "Dirty flag" -- this is what causes chkdsk to be run if you pull the plug on the machine
    uint8_t partition_status; // 0x08

    uint8_t __dummy[3]; // 0x09

    uint32_t hotfix_list; // 0x0C, LBA
    uint32_t hotfix_entries_used; // 0x10, LBA
    uint32_t total_hotfix_entries; // 0x14
    uint32_t spare_dirblks_count; // 0x18
    uint32_t free_spare_dirblks; // 0x1C
    uint32_t code_page_dir_sec; // 0x20
    uint32_t total_code_pages; // 0x24
    uint32_t superblock_crc32; // 0x28, unused except for HPFS386
    uint32_t spareblock_crc32; // 0x3C
    uint32_t extra[15]; // 0x40
    uint32_t spare_dirblks[0]; // 0x6C, length is determined by spare_dirblks_count
} __attribute__((packed));

struct hpfs_codepage_info_entry {
    uint16_t country; // 0x00
    uint16_t codepage; // 0x02
    uint32_t checksum; // 0x04
    uint32_t data_lba; // 0x08
    uint16_t index; // 0x0C
    uint16_t dbcs_count; // 0x0E, double byte caracter set
} __attribute__((packed));

// Size should be 512 bytes
struct hpfs_codepage_info {
#define HPFS_CODEPAGE_INFO_SIG 0x494521F7
    uint32_t signature; // 0x00
    uint32_t cp_count; // 0x04
    uint32_t cp_sec_index; // 0x08, starting index
    uint32_t next_cp_sec; // 0x0C, may not be used
    struct hpfs_codepage_info_entry entries[31];
} __attribute__((packed));

struct hpfs_codepage_data_entry {
    uint16_t country_code;
    uint16_t codepage;
    uint16_t dbcs_range;
    uint8_t mapping_table[128];
    uint8_t dbcs_range_start;
    uint8_t dbcs_range_end;
} __attribute__((packed));

struct hpfs_codepage_data {
#define HPFS_CODEPAGE_DATA_SIG 0x894521F7
    uint32_t signature; // 0x00
    uint16_t count; // 0x04
    uint16_t index; // 0x06
    uint32_t crc32[3]; // 0x08, 0x0C, 0x10
    uint16_t offset[3];
    struct hpfs_codepage_data_entry entries[3];

    uint8_t __padding[78];
} __attribute__((packed));

// This struct contains the actual file extent information, a leaf of the HPFS FNODE B+trees
struct hpfs_alleaf {
    // For a file of size 350, suppose we have three extents:
    // Extent 0:
    //  logical_lba: 0
    //  run_size: 100
    //  physical_lba: 400
    // Extent 1:
    //  logical_lba: 100
    //  run_size: 50
    //  physical_lba: 600
    // Extent 2:
    //  logical_lba: 150
    //  run_size: 200
    //  physical_lba: 1000
    // The first 100 sectors are located at LBA 400 (400-499)
    // The next 50 sectors are located at LBA 600 (600-649)
    // The final 200 sectors are located at LBA 1000 (1000-1199)
    uint32_t logical_lba; // 0x00 -- sector offset of this extent within file
    uint32_t run_size; // 0x04 -- number of sectors in extent
    uint32_t physical_lba; // 0x08 -- file: LBA of start, dir: b-tree's DIRBLK
} __attribute__((packed));

// This struct contains pointerrs to the ALSEC sectors, an intermediate node of the HPFS FNODE B+trees
struct hpfs_alnode {
    uint32_t end_sector_count; // 0x00, Number of sectors mapped by this alnode
    uint32_t physical_lba; // 0x04, file: LBA of ALSEC, dir: b-tree's DIRBLK
} __attribute__((packed));

// Information about the B+tree that follows. Used in FNODEs and ALSECs
struct hpfs_btree_header {
    uint8_t flag;
    uint8_t __padding[3];
    uint8_t free;
    uint8_t used;
    uint16_t free_offset;
} __attribute__((packed));

// FNODE structure, somewhat involved.
// Terminology used here:
//  EA: Extended attributes, up to 64K of data of our choosing.
//  ACL: Access Control Lists, has something to do with LAN manager.
//  ALSEC: A sector countaining a large number of ALNODE/ALLEAF structures
struct hpfs_fnode {
#define HPFS_FNODE_SIG 0xF7E40AAE
    uint32_t signature;
    uint32_t seq_read_history; // 0x04, not implemented
    uint32_t fast_read_history; // 0x08, not implemented
    uint8_t namelen; // 0x0C
    uint8_t name15[15]; // 0x0D, last 15 characters
    uint32_t container_dir_lba; // 0x1C
    uint32_t acl_ext_run_size; // 0x20
    uint32_t acl_lba; // 0x24, location of ACL
    uint16_t acl_internal_size; // 0x28
    uint8_t acl_alsec_flag; // 0x2A
    uint8_t history_bits; // 0x2B, not implemented
    uint32_t ea_ext_run_size; // 0x2C
    uint32_t ea_lba; // 0x30
    uint16_t ea_internal_size; // 0x34
    uint8_t ea_alsec_flag; // 0x36, != 0 if EA LSN points to an ALSEC
#define HPFS_FNODE_ISDIR 1
    uint8_t dir_flag; // 0x37
#define HPFS_BTREE_PARENT_IS_FNODE 0x20 // otherwise ALSEC
#define HPFS_BTREE_ALNODES 0x80 // otherwise ALLEAFs

    union {
        struct {
            uint8_t btree_info_flag; // 0x38
            uint8_t __padding[3];

            uint8_t free_entries; // 0x3C
            uint8_t used_entries; // 0x3D
            uint16_t free_entry_offset; // 0x3E, offset to next free entry
        };
        struct hpfs_btree_header btree_hdr;
    };
    union {
#define HPFS_ALLEAFS_PER_FNODE 8
        struct hpfs_alleaf alleafs[8];
#define HPFS_ALNODES_PER_FNODE 12
        struct hpfs_alnode alnodes[12];
    };

    uint32_t filelen; // 0xA0
    uint32_t needed_ea_counts; // 0xA4
    uint8_t uid[16]; // 0xA8, not used by OS/2, but can be used by for our own purposes
    uint16_t acl_ea_offset; // 0xB8, should be set to 0xC4, regardless of whether the file has EA/ACL data or not.
    uint8_t spare[10]; // 0xBA, unused
    // These is for internal ACL/EA data storage -- for when we don't want to read a bunch of sectors to get file metadata
    uint8_t acl_ea_storage[316]; // 0xC4
} __attribute__((packed));

struct hpfs_alsec {
#define HPFS_ALSEC_SIG 0x37E40AAE
    uint32_t signature; // 0x00
    uint32_t this_lba; // 0x04, our LBA
    uint32_t parent_lba; // 0x08
    union {
        struct hpfs_btree_header btree;
        struct {
            uint8_t btree_flag; // 0x0C, valid flags are HPFS_BTREE_*
            uint8_t __padding[3]; // 0x0D
            uint8_t free_entries; // 0x10
            uint8_t used_entries; // 0x11
            uint16_t free_entry_offset; // 0x12
        };
    };

    union {
#define HPFS_ALLEAFS_PER_ALSEC 40
        struct hpfs_alleaf alleafs[40];
#define HPFS_ALNODES_PER_ALSEC 60
        struct hpfs_alnode alnodes[60];
    };
    uint32_t __padding2[3]; // 0x1F4, unused
} __attribute__((packed));

struct hpfs_dirent {
    uint16_t size; // 0x00
#define HPFS_DIRENT_FLAGS_SPECIAL 0x01
#define HPFS_DIRENT_FLAGS_ACL 0x02
#define HPFS_DIRENT_FLAGS_BTREE 0x04
#define HPFS_DIRENT_FLAGS_DUMMY_END 0x08
#define HPFS_DIRENT_FLAGS_EA 0x10
#define HPFS_DIRENT_FLAGS_EXTENDED_PERMISSIONS 0x20
#define HPFS_DIRENT_FLAGS_EXPLICIT_ACL 0x40
#define HPFS_DIRENT_FLAGS_NEEDED_EA 0x80
    uint8_t flags; // 0x02

// Mirrors FAT flags
#define HPFS_DIRENT_ATTR_READONLY 0x01
#define HPFS_DIRENT_ATTR_HIDDEN 0x02
#define HPFS_DIRENT_ATTR_SYSTEM 0x04
#define HPFS_DIRENT_ATTR_DIRECTORY 0x10
#define HPFS_DIRENT_ATTR_ARCHIVE 0x20
#define HPFS_DIRENT_ATTR_LONGNAME 0x40
    uint8_t attributes; // 0x03

    uint32_t fnode_lba; // 0x04
    uint32_t mtime; // 0x08
    uint32_t filelen; // 0x0C
    uint32_t atime; // 0x10
    uint32_t ctime; // 0x14
    uint32_t ea_size; // 0x18
#define HPFS_FLEX_MASK 7
    uint8_t flex; // 0x1C, # acls
#define HPFS_CP_MASK 127
#define HPFS_CP_DCBS_PRESENT 128
    uint8_t code_page_index; // 0x1D
    uint8_t namelen; // 0x1E
    uint8_t name_stuff[0];

    // After the name and ACLs, we need a down pointer to another dirent
};

struct hpfs_dirblk {
#define HPFS_DIRBLK_SIG 0x77E40AAE
    uint32_t signature; // 0x00
    uint32_t first_free; // 0x04
    uint32_t change; // 0x08, low bit indicates whether this is the topmost DIRBLK in the B-tree
    uint32_t parent_lba; // 0x0C
    uint32_t this_lba; // 0x10

    uint8_t data[2028];
};

#endif