// Inspects and dumps everything about a HPFS partition
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs/hpfs/hpfs.h"

static uint32_t partition_offset = 0;
static void _pread(int fd, void* data, int count, int offset)
{
    lseek(fd, offset + partition_offset, SEEK_SET);
    if (read(fd, data, count) < 0) {
        perror("read");
        exit(-1);
    }
}
static void read_sector(int fd, void* data, int sec)
{
    _pread(fd, data, 512, sec << 9);
}
static void read_sectors(int fd, void* data, int secs, int sec)
{
    _pread(fd, data, 512 * secs, sec << 9);
}

static void printstr(void* data, int maxchrs)
{
    char* datac = data;
    putchar('\"');
    for (int i = 0; i < maxchrs; i++) {
        if (datac[i] == 0)
            putchar(' ');
        else
            putchar(datac[i]);
    }
    putchar('\"');
}

static void printtime(uint32_t t)
{
    if (!t) {
        puts("(zero)");
        return;
    }
    // sizeof(uint32_t) != sizeof(time_t)
    time_t tim = t;
    printf("%s", asctime(localtime(&tim)));
}

static void pagewait(int paged)
{
    if (paged) {
        printf("Press enter to continue: ");
        getchar();
    }
}

static void validate_fnode(struct hpfs_fnode* fnode)
{
    if (fnode->signature != HPFS_FNODE_SIG) {
        fprintf(stderr, "Invalid fnode signature!\n");
        exit(1);
    }
}
static void print_fnode(struct hpfs_fnode* fnode)
{
    printf("  Name15: ");
    printstr(fnode->name15, 15);
    printf("\n  Parent directory FNODE LBA: 0x%x\n"
           "  ACL:\n"
           "    Sectors: %d\n"
           "    LBA: 0x%x (to %s)\n"
           "    Internal size: %d\n"
           "  EA:\n"
           "    Sectors: %d\n"
           "    LBA: 0x%x (to %s)\n"
           "    Internal size: %d\n"
           "  Type: %s\n"
           "  B+Tree flags:\n"
           "    Parent is %s\n"
           "    This FNODE contains an array of %ss\n"
           "  AL* Array:\n"
           "    Free entries: %d\n"
           "    Used entries: %d\n"
           "    Next free entry offset: 0x%x\n",
        fnode->container_dir_lba,
        fnode->acl_ext_run_size,
        fnode->acl_lba,
        fnode->acl_alsec_flag ? "ALSEC" : "raw data",
        fnode->acl_internal_size,

        fnode->ea_ext_run_size,
        fnode->ea_lba,
        fnode->ea_alsec_flag ? "ALSEC" : "raw data",
        fnode->ea_internal_size,

        fnode->dir_flag & HPFS_FNODE_ISDIR ? "Directory" : "File",

        fnode->btree_info_flag & HPFS_BTREE_PARENT_IS_FNODE ? "FNODE" : "ALSEC",
        fnode->btree_info_flag & HPFS_BTREE_ALNODES ? "ANODE" : "ALLEAFs",

        fnode->free_entries,
        fnode->used_entries,
        fnode->free_entry_offset);
    if (!(fnode->btree_info_flag & HPFS_BTREE_ALNODES))
        for (int i = 0; i < 8; i++)
            printf("    ALLEAF Extent #%d\n"
                   "      Offset in file: 0x%x (sector=0x%x)\n"
                   "      Run size: 0x%x\n"
                   "      Physical LBA: 0x%x\n",
                i,
                fnode->alleafs[i].logical_lba << 9, fnode->alleafs[i].logical_lba,
                fnode->alleafs[i].run_size,
                fnode->alleafs[i].physical_lba);
    else
        for (int i = 0; i < 12; i++)
            printf("      ALNODE Extent #%d\n"
                   "      End sector count: 0x%x\n"
                   "      Physical LBA: 0x%x\n",
                i,
                fnode->alnodes[i].end_sector_count,
                fnode->alnodes[i].physical_lba);
    printf("  File length: %d (0x%x)\n"
           "  EAs necessary: %d\n"
           "  ACL/EA offset in FNODE: 0x%x\n",
        fnode->filelen, fnode->filelen,
        fnode->needed_ea_counts,
        fnode->acl_ea_offset);
}

static void handle_dirblk(struct hpfs_dirblk* dirblk, uint32_t blk)
{
    if (dirblk->signature != HPFS_DIRBLK_SIG) {
        fprintf(stderr, "Invalid dirblk signature\n");
        exit(1);
    }
    printf("Dirblk:\n"
           "  Offset of first free entry: 0x%x\n"
           "  Top-most block? %c\n"
           "  Self-pointer valid? %c\n",
        dirblk->first_free,
        dirblk->change & 1 ? 'Y' : 'N',
        dirblk->this_lba == blk ? 'Y' : 'N');
    int offset = 0, id = 0;
    while (1) {
        struct hpfs_dirent* de = (void*)&dirblk->data[offset];
        if (de->size & 3)
            fprintf(stderr, "ERROR: dirent size is NOT a multiple of 4!\n");
        if (de->size == 0) {
            fprintf(stderr, "DIRENT SIZE IS ZERO\n");
            return;
        }
        int end = de->flags & HPFS_DIRENT_FLAGS_DUMMY_END;
        printf("  DIRENT #%d\n"
               "    Entry size: 0x%x\n"
               "    Flags:\n"
               "      Special '..' entry? %c\n"
               "      Has an ACL? %c\n"
               "      Has a B-tree down-pointer? %c\n"
               "      Is a dummy end record? %c\n"
               "      Has an EA list? %c\n"
               "      Has an extended permission list? %c\n"
               "      Has an explicit ACL? %c\n"
               "      Has a needed EA? %c\n"
               "    Attributes:\n"
               "      Read-Only? %c\n"
               "      Hidden? %c\n"
               "      System? %c\n"
               "      Directory? %c\n"
               "      Archive? %c\n"
               "      Long Name? %c\n"
               "    FNODE LSN: 0x%x\n"
               "    Last modified: ",
            id,
            de->size,
            de->flags & HPFS_DIRENT_FLAGS_SPECIAL ? 'Y' : 'N',
            de->flags & HPFS_DIRENT_FLAGS_ACL ? 'Y' : 'N',
            de->flags & HPFS_DIRENT_FLAGS_BTREE ? 'Y' : 'N',
            de->flags & HPFS_DIRENT_FLAGS_DUMMY_END ? 'Y' : 'N',
            de->flags & HPFS_DIRENT_FLAGS_EA ? 'Y' : 'N',
            de->flags & HPFS_DIRENT_FLAGS_EXTENDED_PERMISSIONS ? 'Y' : 'N',
            de->flags & HPFS_DIRENT_FLAGS_EXPLICIT_ACL ? 'Y' : 'N',
            de->flags & HPFS_DIRENT_FLAGS_NEEDED_EA ? 'Y' : 'N',
            de->attributes & HPFS_DIRENT_ATTR_READONLY ? 'Y' : 'N',
            de->attributes & HPFS_DIRENT_ATTR_HIDDEN ? 'Y' : 'N',
            de->attributes & HPFS_DIRENT_ATTR_SYSTEM ? 'Y' : 'N',
            de->attributes & HPFS_DIRENT_ATTR_DIRECTORY ? 'Y' : 'N',
            de->attributes & HPFS_DIRENT_ATTR_ARCHIVE ? 'Y' : 'N',
            de->attributes & HPFS_DIRENT_ATTR_LONGNAME ? 'Y' : 'N',
            de->fnode_lba);
        printtime(de->mtime);
        printf("    File size: %d (0x%x)\n"
               "    Last accessed: ",
            de->filelen, de->filelen);
        printtime(de->atime);
        printf("    Created: ");
        printtime(de->ctime);
        printf("    EA size: %d\n"
               "    # ACLs: %d\n"
               "    Code page index: %d, DBCS present? %c\n"
               "    Name: ",
            de->ea_size,
            de->flex & HPFS_FLEX_MASK,
            de->code_page_index & HPFS_CP_MASK, de->code_page_index & HPFS_CP_DCBS_PRESENT ? 'Y' : 'N');
        if (!end) {
            if (de->flags & HPFS_DIRENT_FLAGS_SPECIAL)
                printf(".. (special directory)");
            else
                printstr(de->name_stuff, de->namelen);
        } else
            printf("(n/a -- last dirent)");
        printf("\n\n");
        if (end)
            break;
        id++;
        offset += de->size;
    }
}

static int printdir(int fd, struct hpfs_fnode* fnode)
{
    if (!(fnode->dir_flag & HPFS_FNODE_ISDIR)) {
        fprintf(stderr, "Not a directory\n");
        return -1;
    }
    struct hpfs_dirblk dirblk;
    if (fnode->btree_info_flag & HPFS_BTREE_ALNODES)
        for (unsigned int i = 0; i < fnode->used_entries; i++) {
            read_sectors(fd, &dirblk, 4, fnode->alnodes[i].physical_lba);
            handle_dirblk(&dirblk, fnode->alnodes[i].physical_lba);
        }
    else
        for (unsigned int i = 0; i < fnode->used_entries; i++) {
            read_sectors(fd, &dirblk, 4, fnode->alleafs[i].physical_lba);
            handle_dirblk(&dirblk, fnode->alleafs[i].physical_lba);
        }
    return 0;
}

static uint32_t partition_base = 0;

int main(int argc, char** argv)
{
    int is_part_image = 0, paged = 0;
    char* img = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i")) {
            is_part_image = 1;
        } else if (!strcmp(argv[i], "-p")) {
            paged = 1;
        } else if (!strcmp(argv[i], "-o")) {
            partition_offset = atoi(argv[++i]) * 512;
        } else {
            if (argv[i][0] == '-') {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                exit(1);
            } else
                img = argv[i];
        }
    }
    if (!img) {
        fprintf(stderr, "No image specified!\n");
        exit(1);
    }

    int fd = open(img, O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(-1);
    }

    struct hpfs_bpb bpb;
    _pread(fd, &bpb, 512, partition_base);

    // Print out stuff about our BPB
    printf(" == BIOS Parameter Block == \n"
           "OEM label: ");
    printstr(bpb.oem, 8);
    printf("\nFAT12:\n"
           "  Bytes per sector: %d (0x%x)\n"
           "  Sectors per cluster: %d\n"
           "  Number of reserved sectors: %d (0x%x)\n"
           "  Number of FATs: %d\n"
           "  Number of root directory entries: %d\n"
           "  Total sectors 16-bit is zero? %c\n"
           "  Media descriptor: 0x%02x\n"
           "  Sectors per FAT: 0x%04x\n",
        bpb.bytes_per_sector, bpb.bytes_per_sector, // %d, 0x%x
        bpb.sectors_per_cluster, // %d
        bpb.reserved_sectors, bpb.reserved_sectors, // %d, 0x%x
        bpb.number_of_fats, // %d
        bpb.root_dir_entries, // %d
        bpb.total_sectors16 == 0 ? 'Y' : 'N', // %c
        bpb.media_desc, // 0x%02x
        bpb.sectors_per_fat // 0x%04x
        );
    pagewait(paged);

    printf("\nFAT16:\n"
           "  Sectors per track: %d\n"
           "  Heads: %d\n"
           "  Hidden sectors (number of sectors before partition): %d (0x%x)\n"
           "  Total sectors in partition: %d (0x%x)\n",
        bpb.spt, // %d
        bpb.heads, // %d
        bpb.hidden_sectors, bpb.hidden_sectors, // %d, 0x%x
        bpb.total_sectors32, bpb.total_sectors32 // %d, 0x%x
        );
    pagewait(paged);

    printf("\nHPFS:\n"
           "  BIOS drive number: 0x%02x\n"
           "  Flags: 0x%02x\n"
           "  Boot signature: 0x%02x\n"
           "  Serial number: 0x%08x\n"
           "  Volume label: ",
        bpb.drive_number, // 0x%02x
        bpb.flags, // 0x%02x
        bpb.boot_sig, // 0x%02x
        bpb.serial // 0x%08x, note unaligned
        );
    printstr(bpb.volume_label, 11);
    printf("\n  Filesystem type: ");
    printstr(bpb.fstype, 8);
    printf("\n");
    printf("Boot signature? %c\n", bpb.boot_magic[0] == 0x55 && bpb.boot_magic[1] == 0xAA ? 'Y' : 'N');

    if (memcmp(bpb.fstype, "HPFS", 4)) {
        fprintf(stderr, "Not a HPFS volume!\n  Reason: Wrong filesystem type in BPB (should be \"HPFS    \")\n");
        exit(-1);
    }
    pagewait(paged);

    uint32_t secsize = bpb.bytes_per_sector;

    // Read LBA16
    struct hpfs_superblock superblock;
    _pread(fd, &superblock, sizeof(struct hpfs_superblock), 16 * secsize);

    printf("\n\n == Superblock ==\n");
    printf(
        "  Signature? %c\n"
        "  Version: %d\n"
        "  Functional version: %d (disk is %s 4G)\n"
        "  LBA of root directory fnode: 0x%x\n"
        "  Sectors in partition: 0x%x\n"
        "  Bad sectors count: 0x%x\n"
        "  LBA of sector bitmap: 0x%x (spare: 0x%x)\n"
        "  LBA of bad sector list: 0x%x (spare: 0x%x)\n"
        "  Chkdsk /f last run: ",
        superblock.signature[0] != HPFS_SUPER_SIG0 || superblock.signature[1] != HPFS_SUPER_SIG1 ? 'N' : 'Y', // %c
        superblock.version, // %d
        superblock.functional_ver, superblock.functional_ver == 2 ? "<=" : ">", // %d, %s
        superblock.rootdir_fnode, // 0x%x
        superblock.sectors_in_partition, // 0x%x
        superblock.bad_sector_count, // 0x%x
        superblock.list_bitmap_secs, superblock.bitmap_secs_spare, // 0x%x, 0x%x
        superblock.list_bad_secs, superblock.bad_secs_spare // 0x%x, 0x%x
        );

    printtime(superblock.chkdsk_last_run);
    printf("  Last optimized: ");
    printtime(superblock.last_optimized);
    printf("  Directory band:\n"
           "    Sectors: 0x%x\n"
           "    Start sectors: 0x%x\n"
           "    End sectors: 0x%x\n"
           "    Bitmap: 0x%x\n",
        superblock.dir_band_sectors,
        superblock.dir_band_start_sec,
        superblock.dir_band_end_sec,
        superblock.dir_band_bitmap);
    pagewait(paged);

    struct hpfs_spareblock spareblock;
    _pread(fd, &spareblock, sizeof(struct hpfs_spareblock), 17 * secsize);
    printf("\n\n == Spareblock ==\n");
    printf(
        "  Signature? %c\n"
        "  Status: 0x%02x\n"
        "    Written by old IFS? %c\n"
        "    Fast formatted? %c\n"
        "    Bad bitmap? %c\n"
        "    Bad sector? %c\n"
        "    Hotfix sectors used? %c\n"
        "    Spare DirBlks used? %c\n"
        "    Dirty? %c\n"
        "  Hotfix list start: 0x%x\n"
        "  Hotfix entries used: 0x%x\n"
        "  Total hotfix entries: 0x%x\n"
        "  Spare dirblks count: 0x%x\n"
        "  Free spare dirblks: 0x%x\n"
        "  Code page directory: 0x%x\n"
        "  Number of code pages: 0x%x\n"
        "  Superblock CRC (unused if not HPFS386): 0x%x\n"
        "  Spareblock CRC (unused if not HPFS386): 0x%x\n\n\n",
        spareblock.signature[0] != HPFS_SPARE_SIG0 || spareblock.signature[1] != HPFS_SPARE_SIG1 ? 'N' : 'Y', // %c
        spareblock.partition_status, // 0x%02x
        spareblock.partition_status & HPFS_STATUS_OLDFS ? 'Y' : 'N', // %c
        spareblock.partition_status & HPFS_STATUS_FASTFORMAT ? 'Y' : 'N', // %c
        spareblock.partition_status & HPFS_STATUS_BAD_BITMAP ? 'Y' : 'N', // %c
        spareblock.partition_status & HPFS_STATUS_BAD_SECTOR ? 'Y' : 'N', // %c
        spareblock.partition_status & HPFS_STATUS_HOTFIX_SECS_USED ? 'Y' : 'N', // %c
        spareblock.partition_status & HPFS_STATUS_SPARE_DIRBLKS_USED ? 'Y' : 'N', // %c
        spareblock.partition_status & HPFS_STATUS_DIRTY ? 'Y' : 'N', // %c
        spareblock.hotfix_list, // 0x%x
        spareblock.hotfix_entries_used, // 0x%x
        spareblock.total_hotfix_entries, // 0x%x
        spareblock.spare_dirblks_count, // 0x%x
        spareblock.free_spare_dirblks, // 0x%x
        spareblock.code_page_dir_sec, // 0x%x
        spareblock.total_code_pages, // 0x%x
        spareblock.superblock_crc32, // 0x%x
        spareblock.spareblock_crc32 // 0x%x
        );
    pagewait(paged);

    // Explore the Hotfix entries. It's four sectors of the format
    //  uint32_t from[spareblock.total_hotfix_entries];
    //  uint32_t to[spareblock.total_hotfix_entries];
    uint32_t hotfix[512];
    if (spareblock.total_hotfix_entries >= 256) // This isn't a fatal error -- we simply don't need to print out spareblocks
        fprintf(stderr, "Too many hotfix entries (max total_hotfix_entries: 256)\n");
    else {
        printf("Hotfix list: \n");
        _pread(fd, hotfix, secsize * 4, secsize * spareblock.hotfix_list);
        if (!spareblock.hotfix_entries_used)
            printf("  (none in use)\n");
        else {
            printf("  Format: (from, bad sector) -> (to, good sector)");
            for (unsigned int i = 0; i < spareblock.hotfix_entries_used; i++)
                printf("  0x%x -> 0x%x", hotfix[i], hotfix[i + spareblock.total_hotfix_entries]);
        }
        printf("\n\n");
    }
    pagewait(paged);

    // Explore spare dirblk entries, if there are any
    if (spareblock.spare_dirblks_count) {
        // check if we have too many
        if (((secsize - 0x6C) >> 2) <= spareblock.spare_dirblks_count)
            fprintf(stderr, "Too many spare dirblks (max spare_dirblks_count: %d)\n", (secsize - 0x6C) >> 2);
        else {
            int bytes = spareblock.spare_dirblks_count << 2;
            uint32_t* dirblks = alloca(bytes);
            _pread(fd, dirblks, bytes, 17 * secsize + 0x6C);
            printf("Spare dirblk list:");
            unsigned int i = 0;
            while (i < spareblock.spare_dirblks_count) {
                if ((i & 7) == 0) // Create newline every eighth element, and first one too
                    printf("\n  0x%08x", dirblks[i]);
                else
                    printf(", 0x%08x", dirblks[i]);
                i++;
            }
            printf("\n\n\n");
            pagewait(paged);
        }
    }

    // Explore code pages
    if (spareblock.code_page_dir_sec) {
        struct hpfs_codepage_info cpinfo;
        struct hpfs_codepage_data cpdata;
        read_sector(fd, &cpinfo, spareblock.code_page_dir_sec);
        if (cpinfo.signature != HPFS_CODEPAGE_INFO_SIG)
            fprintf(stderr, "Invalid codepage info signature.\n");
        else {
            uint32_t current_sector = spareblock.code_page_dir_sec;
            for (unsigned int i = 0; i < spareblock.total_code_pages;) {
                read_sector(fd, &cpinfo, current_sector);
                if (cpinfo.signature != HPFS_CODEPAGE_INFO_SIG)
                    fprintf(stderr, "Invalid codepage info signature.\n");
                printf("Codepages:\n"
                       "  Count: %d\n",
                    cpinfo.cp_count);

                for (unsigned int j = 0; j < cpinfo.cp_count; j++) {
                    printf("  Ent#%d:\n"
                           "    Country: %d | Codepage: %d | CRC32: 0x%08x | Sector: 0x%x | DBCS ranges: %d\n"
                           "    Data:\n",
                        j + cpinfo.cp_sec_index, // or just use 'i?' j + cpinfo.cp_sec_index should equal i
                        cpinfo.entries[j].country,
                        cpinfo.entries[j].codepage,
                        cpinfo.entries[j].checksum,
                        cpinfo.entries[j].data_lba,
                        cpinfo.entries[j].dbcs_count);

                    read_sector(fd, &cpdata, cpinfo.entries[j].data_lba);
                    printf("      Signature? %c\n"
                           "      Number of tables: %d\n"
                           "      Data index: %d\n",
                        cpdata.signature == HPFS_CODEPAGE_DATA_SIG ? 'Y' : 'N',
                        cpdata.count,
                        cpdata.index);
                    for (int k = 0; k < 2; k++) {
                        if (cpdata.offset[k]) {
                            printf("      CP%d (offs: %d):\n"
                                   "        CRC32: 0x%08x\n"
                                   "        Entry: %d\n",
                                cpinfo.entries[j].codepage,
                                cpdata.offset[k],
                                cpdata.crc32[k],
                                cpdata.offset[k]);
#if 0 // Set this to '1' if you want to print out the codepage data
                        // Now print character set
                        for (int l = 0; l < 128; l++) {
                            if (!(l & 7))
                                printf("\n");
                            printf(" '%d' > '%d'", l | 0x80, cpdata.entries[k].mapping_table[l]);
                    }
#endif
                        }
                    }
                    i++;
                    printf("%d %d\n", i, spareblock.total_code_pages);
                }
                current_sector = cpinfo.next_cp_sec;
            }
        }
        pagewait(paged);
    }

    // The housekeeping stuff is now complete. Now we can read the root directory
    printf(" == Root Directory ==\n");
    struct hpfs_fnode fnode;
    read_sector(fd, &fnode, superblock.rootdir_fnode);
    validate_fnode(&fnode);
    print_fnode(&fnode);
    printdir(fd, &fnode);
}