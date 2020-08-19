// Format a disk or a partition with HPFS.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> // for time(NULL)

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs/hpfs/hpfs.h"

static uint32_t partition_base, partition_size, cseek;
static int fd;
static struct hpfs_superblock* superblock;
static struct hpfs_spareblock* spareblock;
static uint32_t **blk_bitmaps, *bitmap_locations, *dirband_bitmap_data;
static uint32_t dirband_sectors_used;
static uint32_t NOW;
static uint8_t casetbl[256];

enum {
    TYPE_DIRECTORY,
    TYPE_FILE
};

struct hpfs_fnode_and_data {
    int type;
    struct hpfs_fnode* fnode;
    union {
        struct hpfs_dirblk* dirblk;
        uint8_t* data;
    };
};

#define BAND_SIZE (8 << 20)

static uint32_t chksum(void* vp, int size)
{
    uint32_t sum = 0;
    uint8_t* p = vp;

    while (size != 0) {
        sum += *p++;
        sum = (sum << 7) | (sum >> 25); /* Rotate left by 7 bits */
        --size;
    }
    return sum;
}

static void help(void)
{
    printf("mkhpfs -- Make High Performance File System\n"
           "Usage: mkhpfs [args] <raw disk>|<partition> [args]\n"
           "You can specify a partitioned disk (with a MBR) or a raw HPFS partition.\n"
           "args can be:\n"
           " -i  Format raw partition, not disk\n"
           " -p <id>  Partition number, if raw disk (default: 0)\n"
           " -b <file>  Set boot block image (max size: 8kb)*\n"
           " -h  Show this message\n"
           " -V <str>  Set volume label (default: \"MKHPFS\")\n"
           "\n"
           "ADVANCED OPTIONS:\n"
           " -H <n>  Set hotfix sector list size (default: 100, max: 255)\n"
           " -s <n>  Set number of spare dirblks (default: 20, max: 100)\n"
           " -O <str>  Set OEM name (default: \"OS2 20.0\")\n"
           "\n"
           "* Note that FAT fields in boot block image will be overwritten\n");
    exit(0);
}
static void _pread(int fd, void* data, int count, uint32_t offset)
{
    if (offset != cseek)
        lseek(fd, offset, SEEK_SET);
    if (read(fd, data, count) < 0) {
        perror("read");
        exit(-1);
    }
    cseek += count;
}
static void read_sector(int fd, void* data, int sec)
{
    _pread(fd, data, 512, (sec + partition_base) << 9);
}
static void read_sector2(int fd, void* data, int sec)
{
    _pread(fd, data, 512, sec << 9);
}
static void read_sectors(int fd, void* data, int secs, int sec)
{
    _pread(fd, data, 512 * secs, (sec + partition_base) << 9);
}
static void _pwrite(int fd, void* data, int count, uint32_t offset)
{
    if (offset != cseek)
        lseek(fd, offset, SEEK_SET);
    if (write(fd, data, count) < 0) {
        perror("read");
        exit(-1);
    }
    cseek += count;
}
static void write_sector(int fd, void* data, int sec)
{
    _pwrite(fd, data, 512, (sec + partition_base) << 9);
}
static void write_sectors(int fd, void* data, int secs, int sec)
{
    _pwrite(fd, data, 512 * secs, (sec + partition_base) << 9);
}

static void strcpy2(void* dest, void* src, int len)
{
    char *srcc = src, *destc = dest;
    for (int i = 0; i < len; i++) {
        char srcch = *srcc;
        if (srcch) {
            *destc++ = srcch;
            srcc++;
        } else
            *destc++ = ' ';
    }
}

static void install_boot_blk(char* img, char* oem, char* vollab)
{
    int fd2 = -1;
    struct hpfs_bpb bpb = {0};
    if (img) {
        fd2 = open(img, O_RDONLY);
        if (fd2 < 0) {
            perror("open bootblk");
            exit(1);
        }
        // Read first 512 bytes
        read_sector2(fd2, &bpb, 0);
    }// else // avoid weird gcc warning
      //  memset(&img, 0, 512);

    // Get image size to compute CHS. We only set this for BPB purposes -- all other accesses are done using LBA
    uint32_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, cseek, SEEK_SET);

    // XXX bad CHS algorithm
    int heads = 16, spt = 63, cyls = (size >> 9) / (heads * spt);

    // These FAT fields aren't actually used for anything -- the Linux driver doesn't read them.
    // But these values were produced by the OS/2 format command, so here we go.
    strcpy2(bpb.oem, oem, 8);

    bpb.bytes_per_sector = 512;
    bpb.sectors_per_cluster = 8;
    bpb.reserved_sectors = 1;
    bpb.number_of_fats = 0;
    bpb.root_dir_entries = 512;
    bpb.total_sectors16 = 0;
    bpb.media_desc = 0xF8; // hard disk
    bpb.sectors_per_fat = 0x86;

    bpb.spt = spt;
    bpb.heads = heads;
    bpb.hidden_sectors = partition_base;
    bpb.total_sectors32 = partition_size;

    bpb.drive_number = 0x80;
    bpb.flags = 0;
    bpb.boot_sig = 0x28;
    bpb.serial = 0x12345678; // XXX
    strcpy2(bpb.volume_label, vollab, 11);
    strcpy2(bpb.fstype, "HPFS", 8);

    bpb.boot_magic[0] = 0x55;
    bpb.boot_magic[1] = 0xAA;
    // Write boot parameter block
    write_sector(fd, &bpb, 0);

    if (fd2 >= 0) {
        // Now add in the rest of our boot block
        uint8_t sec[512];
        for (int i = 1; i < 16; i++) {
            int retval;
            // read next sector of boot block image
            if ((retval = read(fd2, sec, 512)) < 0) {
                perror("read bootblk image");
                exit(-1);
            }
            // Pad it with zeros, if needed
            if (retval < 512)
                memset(sec + retval, 0, 512 - retval);
            // Write it to disk
            write_sector(fd, sec, i);

            // If we've reached the end, then just quit
            if (retval < 512)
                break;
        }
        close(fd2);
    }
}

// Fill in our superblock image with the right values, but don't write it to disk yet -- we haven't decided where our bands are going to be yet
static void populate_superblock(struct hpfs_superblock* superblock)
{
    superblock->signature[0] = HPFS_SUPER_SIG0;
    superblock->signature[1] = HPFS_SUPER_SIG1;
    superblock->version = 2;
    superblock->functional_ver = 2; // disk is <= 4G
    superblock->rootdir_fnode = 0;
    superblock->sectors_in_partition = partition_size;
    superblock->bad_sector_count = 0;
    superblock->list_bitmap_secs = 0;
    superblock->bitmap_secs_spare = 0;
    superblock->list_bad_secs = 0;
    superblock->bad_secs_spare = 0;
    superblock->chkdsk_last_run = 0;
    superblock->last_optimized = 0;

    superblock->dir_band_bitmap = 0;
    superblock->dir_band_end_sec = 0;
    superblock->dir_band_sectors = 0;
    superblock->dir_band_start_sec = 0;
}

static void populate_spareblock(struct hpfs_spareblock* spareblock, int hotfix2, int spare)
{
    spareblock->signature[0] = HPFS_SPARE_SIG0;
    spareblock->signature[1] = HPFS_SPARE_SIG1;
    spareblock->partition_status = HPFS_STATUS_FASTFORMAT;
    spareblock->hotfix_list = 0;
    spareblock->hotfix_entries_used = 0;
    spareblock->total_hotfix_entries = hotfix2;
    spareblock->spare_dirblks_count = spare;
    spareblock->free_spare_dirblks = spare;
    spareblock->code_page_dir_sec = 0;
    spareblock->total_code_pages = 0;
}

static int sector_unoccupied(int sec)
{
    uint32_t* bitmap = blk_bitmaps[sec >> 14];
    int offset = sec & 0x3FFF;
    return bitmap[offset >> 5] & (1 << (offset & 31));
}

static void mark_sectors_used(int sec, int count)
{
    if (count == 0)
        return;

    uint32_t* bitmap = blk_bitmaps[sec >> 14];
    int offset = sec & 0x3FFF;
    for (int i = 0; i < count; i++) {
        // XXX -- there are probably faster ways to do this
        bitmap[offset >> 5] &= ~(1 << (offset & 31));
        offset++;

        // If we've reached the end, then move onto the next one
        if (offset == 0x4000) {
            bitmap = blk_bitmaps[(sec + offset) >> 14];
            offset = 0;
        }
    }
}
static int lowest_sector_used = 0;
static uint32_t alloc_sectors(int count)
{
    // Make sure that our base isn't occupied
    while (!sector_unoccupied(lowest_sector_used))
        lowest_sector_used++;
    int retv = lowest_sector_used;
    for (int i = 1; i < count; i++)
        if (!sector_unoccupied(retv + i)) {
            retv += i;
            i = 0; // restart from the beginning
        }
    mark_sectors_used(retv, count);
    return retv;
}

// Try to allocate a bunch of sectors from the directory band, but if there's nothing left then allocate from the main band.
// Strictly speaking, using the dirband is optional, but HPFS would like us to use it.
static uint32_t alloc_dirband_sectors(int count)
{
    if ((dirband_sectors_used + count) >= superblock->dir_band_sectors)
        return alloc_sectors(count);

    int retv = superblock->dir_band_start_sec + dirband_sectors_used;
    // Mark dirband sectors as used
    for (int i = 0; i < count; i++) {
        int ndx = retv + i;
        dirband_bitmap_data[ndx >> 5] &= ~(1 << (ndx & 31));
    }
    dirband_sectors_used += count;
    return retv;
}

static int create_codepage(void)
{
    // Code pages are probably very broken, hopefully nobody uses them.
    struct hpfs_codepage_info info;
    struct hpfs_codepage_data data;
    memset(&info, 0, 512); // sizeof(info) == 512
    memset(&data, 0, 512); // sizeof(data) == 512
    info.cp_count = 1; // total: one code page
    info.cp_sec_index = 0; // first codepage, starts from 0
    info.next_cp_sec = 0;
    info.signature = HPFS_CODEPAGE_INFO_SIG;

    info.entries[0].index = 0;
// Change the below values to fit your needs
// Some combinations I've seen:
//  061 (Australia)/850 (Basic Multilingual Latin-1)
//  421 (Slovakia)/852 (Central European Languages)
// Country Code seems to be the same thing as the national calling code
// http://www.edm2.com/index.php/COUNTRYCODE
#define CODE_COUNTRY 1 // USA
#define CODE_PAGE 437 // Code Page 437, same as standard IBM PC VGA font
    info.entries[0].country = CODE_COUNTRY;
    info.entries[0].codepage = CODE_PAGE;

    int info_sector = alloc_sectors(1);
    int data_sector = info.entries[0].data_lba = alloc_sectors(1);
    info.entries[0].dbcs_count = 0; // Needed for the CJK languages

    data.signature = HPFS_CODEPAGE_DATA_SIG;
    data.count = 1; // One table in data page
    data.index = 0; // First code page
    data.offset[0] = 26; // offsetof(data, entries[0])
    data.entries[0].codepage = CODE_PAGE;
    data.entries[0].country_code = CODE_COUNTRY;
    data.entries[0].dbcs_range = 0; // XXX check for CJK languages
    data.entries[0].dbcs_range_end = 0;
    data.entries[0].dbcs_range_start = 0;
    // XXX - better mapping
    for (int i = 0; i < 128; i++)
        data.entries[0].mapping_table[i] = i | 0x80;

    // Code pages require a check sum
    data.crc32[0] = info.entries[0].checksum = chksum(&data.entries[0], sizeof(data.entries[0]));
    write_sector(fd, &data, data_sector);
    write_sector(fd, &info, info_sector);
    return info_sector;
}

#define FNODE_USE_ALNODE(fnode) fnode->btree_info_flag |= HPFS_BTREE_ALNODES
#define FNODE_USE_ALLEAF(fnode) fnode->btree_info_flag &= ~HPFS_BTREE_ALNODES

static struct hpfs_fnode* mk_fnode(char* name, uint32_t container_dir_lba, int isdir, uint32_t length)
{
    struct hpfs_fnode* fnode = calloc(1, 512);
    fnode->signature = HPFS_FNODE_SIG;
    // Gather last 15 bytes of string name or whole name, whichever one is shorter.
    int namelen = strlen(name), ndxstart = namelen - 15;
    if (ndxstart < 0)
        ndxstart = 0;
    fnode->namelen = namelen;
    strcpy(fnode->name15, &name[ndxstart]);
    fnode->container_dir_lba = container_dir_lba;

    // Ignore ACLs and EAs right now. We can add them later, if we want.

    fnode->dir_flag = isdir != 0;
    // We don't know what kind of allocations this file contains, so we ignore free_entries, used_entries, and free_entry_offset for now.

    fnode->filelen = isdir ? 0 : length; // Directories have lengths of zero bytes.
    fnode->acl_ea_offset = 0xC4;
}

static int override_dirband = 0;
static void hpfs_mkdir(struct hpfs_fnode_and_data* fnd, char* name, uint32_t container_dir_lba)
{
    struct hpfs_fnode* fnode = mk_fnode(name, container_dir_lba, 1, 0);
    // Make the fnode use ALLEAFs
    FNODE_USE_ALLEAF(fnode);

    // Set the first ALLEAF to our prospective dirblk location
    int dirblk_location;
    if (override_dirband) { // For something like the root dirblk, which has a defined place in the image file
        dirblk_location = override_dirband;
        override_dirband = 0;
    } else
        dirblk_location = alloc_dirband_sectors(4);

    // Inform our fnode where our dirblk is
    fnode->alleafs[0].logical_lba = 0;
    fnode->alleafs[0].physical_lba = dirblk_location;
    fnode->alleafs[0].run_size = 0;
    fnode->alleafs[1].logical_lba = -1; // This appears on my hpfs formatted disk

    fnode->used_entries = 1;
    fnode->free_entries = HPFS_ALLEAFS_PER_FNODE - 1;
    fnode->free_entry_offset = 0x14; // &fnode->alleafs[2] - &fnode->alleafs[0]

    // Make a dirblk too
    struct hpfs_dirblk* dirblk = calloc(1, 2048);
    dirblk->signature = HPFS_DIRBLK_SIG;
    dirblk->change = 1;

    // Create a ".." entry
    struct hpfs_dirent* de = (struct hpfs_dirent*)&dirblk->data[0];
    de->attributes = HPFS_DIRENT_ATTR_DIRECTORY;
    de->atime = de->mtime = de->ctime = NOW;
    de->code_page_index = 0;
    de->ea_size = 0;
    de->filelen = 0;
    de->flags = HPFS_DIRENT_FLAGS_SPECIAL;
    de->fnode_lba = 0;
    de->flex = 0;
    de->namelen = 2;
    de->name_stuff[0] = de->name_stuff[1] = 1; // '..', for some reason stored as 01 01
    *(uint32_t*)(&de->name_stuff[4]) = 0;
    de->size = 30 + 2 + 4; // sizeof dirent + 2 padding + down ptr

    de = (struct hpfs_dirent*)&dirblk->data[30 + 2 + 4];
    de->flags = HPFS_DIRENT_FLAGS_DUMMY_END;
    // The "name" of this entry has to be a single FF byte
    de->namelen = 1;
    de->name_stuff[0] = 0xFF;
    // Clear downlink pointer
    //*(uint32_t*)(&de->name_stuff[1]) = 0;
    //de->size = 36;
    de->size = 32;

    dirblk->first_free = 32 + (30 + 2 + 4) + 20;

    // Technically, we've written 2, but ignore it.

    fnd->type = TYPE_DIRECTORY;
    fnd->dirblk = dirblk;
    fnd->fnode = fnode;
}

static void write_fnode(struct hpfs_fnode_and_data* fnd, int sector)
{
    write_sector(fd, fnd->fnode, sector);
    uint32_t dest_sector;
    switch (fnd->type) {
    case TYPE_DIRECTORY:
        dest_sector = fnd->fnode->alleafs[0].physical_lba;

        // Fill in LBA of parent FNODE, current LBA
        fnd->dirblk->parent_lba = sector;
        fnd->dirblk->this_lba = dest_sector;
        // Fill in dirblk
        ((struct hpfs_dirent*)(&fnd->dirblk->data[0]))->fnode_lba = sector;

        write_sectors(fd, fnd->dirblk, 4, dest_sector);
        break;
    }
}

int main(int argc, char** argv)
{
    int raw = 1, partn = -1;
    char *bootblk = NULL, *system_root = NULL, *img = NULL;
    char *oem = "OS2 20.0", *vollab = "MKHPFS";
    int number_of_hotfix_sectors = 100, number_of_spare_dirblks = 20;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && strlen(argv[i]) == 2) {
            char* arg;
            switch (argv[i][1]) {
            case 'i':
                raw = 0;
                break;
#define ARG()                                                   \
    i++;                                                        \
    if (i == argc)                                              \
        fprintf(stderr, "Expected argument to: %s\n", argv[i]); \
    arg = argv[i];
            case 'p':
                ARG();
                partn = atoi(arg);
                break;
            case 'b':
                ARG();
                bootblk = arg;
                break;
            case 'd':
                ARG();
                system_root = arg;
                break;
            case 'H':
                ARG();
                number_of_hotfix_sectors = atoi(arg);
                if (number_of_hotfix_sectors < 0 || number_of_hotfix_sectors >= 256) {
                    fprintf(stderr, "Hotfix sector count out of range (%d)\n", number_of_hotfix_sectors);
                    break;
                }
                break;
            case 's':
                ARG();
                number_of_spare_dirblks = atoi(arg);
                if (number_of_spare_dirblks < 0 || number_of_spare_dirblks >= 100) {
                    fprintf(stderr, "Spare dirblk count out of range (%d)\n", number_of_hotfix_sectors);
                    break;
                }
                break;
            case 'O':
                ARG();
                oem = arg;
                break;
            case 'V':
                ARG();
                vollab = arg;
                break;
            case 'h':
                help();
                break;
            default:
                fprintf(stderr, "Unknown argument: %s\n", argv[i]);
                break;
            }
        } else
            img = argv[i];
    }
    if (!img) {
        fprintf(stderr, "Missing image\n");
        exit(-1);
    }

    fd = open(img, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("open");
        exit(-1);
    }

    NOW = time(NULL);

    uint8_t mbr[512];
    if (raw) {
        // We can call read_sector to get the MBR since partition_base is still 0.
        read_sector(fd, mbr, 0);
        if (partn == -1) {
            // Find a partition large enough for our purposes
            int base = 0x1BE;
            for (int i = 0; i < 4; i++) {
                fprintf(stderr, "size: %08x\n", *(uint32_t*)(&mbr[base + 12]));
                if (*(uint32_t*)(&mbr[base + 12]) > (BAND_SIZE >> 9)) {
                    partn = i;
                    break;
                }
            }
            if (partn == -1) {
                fprintf(stderr, "Could not find suitable partition (must be >8M, >16M preferred)\n");
                exit(1);
            }
        } else if (partn > 3) {
            fprintf(stderr, "Invalid partition number\n");
            exit(1);
        }
        // Set our partion type in the partition table
        if (mbr[0x1BE + (partn * 16) + 4] != 7) {
            mbr[0x1BE + (partn * 16) + 4] = 0x07;
            write_sector(fd, mbr, 0);
        }
        partition_base = *(uint32_t*)(&mbr[0x1BE + (partn * 16) + 8]);
        partition_size = *(uint32_t*)(&mbr[0x1BE + (partn * 16) + 12]);
    } else {
        partition_base = 0;
        partition_size = lseek(fd, 0, SEEK_END);
        lseek(fd, cseek, SEEK_SET);
    }

    install_boot_blk(bootblk, oem, vollab);

    // Create superblock/spareblock
    superblock = calloc(1, 512);
    spareblock = calloc(1, 512);

    populate_superblock(superblock);
    populate_spareblock(spareblock, number_of_hotfix_sectors, number_of_spare_dirblks);

    // Determine how many bands we have.
    int bands = (partition_size + 0x3FFF) >> 14;
    blk_bitmaps = malloc(bands * sizeof(uint32_t*));

    for (int i = 0; i < bands; i++) {
        blk_bitmaps[i] = malloc(4 * 512);
        memset(blk_bitmaps[i], 0xFF, 4 * 512);
    }
    // Mark the first 20 sectors as used:
    //  0-15: Bootblock (16)
    //  16: Superblock (1)
    //  17: Spareblock (1)
    //  18-19: Unused, but allocated (2)
    alloc_sectors(20);

    // How many sectors will our bitmap list take up?
    int bitmap_sectors = (bands * 4 + 511) / 512;
    bitmap_sectors = (bitmap_sectors + 3) & ~3;
    // Create a list for our band sector usage bitmaps, round up to the first multiple of four
    int bitmap_list = superblock->list_bitmap_secs = alloc_sectors(bitmap_sectors);
    bitmap_locations = calloc(bitmap_sectors, 512);

    // If Band #0 was a normal band, its bitmap would be located at sector 0.
    // Unfortunately, that's where the boot block is, so we place it as close as possible to the beginning.
    bitmap_locations[0] = alloc_sectors(4);

    // This is four sectors, according to my knowledge
    superblock->list_bad_secs = alloc_sectors(4);

    // Hotfix entries
    spareblock->hotfix_list = alloc_sectors(4);
    int hotfix_sectors = alloc_sectors(number_of_hotfix_sectors);

    // We put the hotfix sectors right after hotfix entry table, but first we need to create it
    uint32_t* hotfix_table = calloc(4, 512);
    // When a write error occurs, HPFS allows the sector to be temporarily remapped to a number of "hotfix sectors."
    // If sector 0x1234 goes bad while the computer is on, for instance, all writes that would formerly go to sector 0x1234 might go to sector 0x26.
    // It's CHKDSK's job to mark down these hotfixes in the bad sectors array and clear the list.
    //
    // The hotfix table looks like this:
    // struct hotfix {
    //  uint32_t bad_sectors[hotfix];
    //  uint32_t good_sectors[hotfix];
    // };
    // new_sector_address = good_sectors[bad_sectors.indexOf(old_sector_address)];
    for (int i = 0; i < number_of_hotfix_sectors; i++)
        hotfix_table[i + number_of_hotfix_sectors] = hotfix_sectors + i;

    for (int i = 1; i < bands; i++) {
        // Mark the band bitmaps as being used
        // Even bitmaps will have them at the beginning, odd bitmaps will have them at the end
        uint32_t bandid = i * (8 << 20) / 512;
        const uint32_t BAND_END_M4 = (8 << 20) / 512 - 4;
        const uint32_t BAND_END = (8 << 20) / 512;
        if (i & 1) {
            // end
            bandid = bandid + BAND_END_M4;
            int total_sectors = 4;
            if (bandid >= partition_size) {
                // Put band right before partition end? I don't know about this behavior
                bandid = partition_size - 4;
            }
#if 0
            if (bandid >= partition_size) { // We've hit the end of the disk
                // We have to mark all the sectors after this as unreachable.
                // For instance, say we have a partition of 125 sectors, and we want to write sector 196. Band 2 ends at sector 200.
                // That means that (196 + 4) - 125 = 200 - 125 = 75 sectors need to be marked as reserved and used.
                total_sectors = bandid + 4 - partition_size;
                // Put the free space bitmap at the end of the disk
                bandid = partition_size - 4;
            }
#endif
            mark_sectors_used(bandid, total_sectors);
        } else {
            int total_sectors = 4;
#if 0
            if ((bandid + BAND_END) > partition_size)
                // Like above: if our band ends at sector 300 but our disk cuts off at 225 (and we're writing sector 200)
                //  (100 + 200) - 225 = 75
                total_sectors = bandid + BAND_END_M4 - partition_size;
#endif
            mark_sectors_used(bandid, total_sectors);
        }
        bitmap_locations[i] = bandid;
    }

    // We handle spare dirblks later; they go in the middle of the disk, if possible.

    // Handle code pages. I don't really know how they're supposed to be handled, so I'll only provide the bare minimum implementation here.
    spareblock->code_page_dir_sec = create_codepage();
    spareblock->total_code_pages = 1;

    // We have to determine where the the directory structures are going to go.
    // Normally, they go in the middle, but if the partition is small enough, they'll be in the beginning

    // Choosing the mid-most band is always a good place to start
    // Note that having 12 or 13 bands is good enough
    int mid_block = bands >> 1;
    if (mid_block) {
        // If we're jumping to the middle of the disk, find out where to st
        lowest_sector_used = mid_block * (8 << 20) / 512 - 12;
    } else {
        // If we're using block 0, then we start from where we left off.
        // Nothing to do here
    }
    // The layout of the previous block:
    //  +3FF4: directory band bitmap
    //  +3FF8: root directory dirblk (NOT the fnode)
    //  +3FFC: Actual band bitmap
    // The layout of the block we choose:
    //  +0000: Actual band bitmap
    //  +0004: Directory band (your size may vary, size in example is 0xA64)
    //  +0A68: Spare Dirblks (your size may vary, default is 0x50)
    //  +0AB8: Root directory FNODE
    //  +0AB9: Band data
    int dirband_bitmap_lba = alloc_sectors(4);
    dirband_bitmap_data = malloc(4 * 512);
    memset(dirband_bitmap_data, 0xFF, 4 * 512);

    int rootdir_dirblk_lba = alloc_sectors(4);
    if (mid_block) // We don't have to do this if we're starting from band 0
        lowest_sector_used += 8; // Skip band bitmaps (3FFC to 0003)

    // Determine size of directory band. I couldn't figure out how OS/2 determined this count, but a good approximation is 1% of all sectors
    uint32_t dirband_size = partition_size / 100;
    dirband_size = (dirband_size + 3) & ~3; // round up to nice even number

    // Cap our dirband size -- don't let it be larger than a band
    // The dirband bitmap is only four sectors long, indicating that the max size of this structure should be at most 8M.
    if (dirband_size > 0x3FFC) // 0x4000 - 4 (account for the band bitmap!)
        dirband_size = 0x3FFC;
    int dirband = alloc_sectors(dirband_size);

    // Let the superblock know where our dirband is
    superblock->dir_band_bitmap = dirband_bitmap_lba;
    superblock->dir_band_end_sec = dirband + dirband_size - 1; // end is, apparently, inclusive
    superblock->dir_band_sectors = dirband_size;
    superblock->dir_band_start_sec = dirband;
    dirband_sectors_used = 0;

    // Allocate our spare dirblks. Note that each dirblk is 4 sectors, regardless of whether it's a spare or not.
    for (int i = 0; i < number_of_spare_dirblks; i++)
        spareblock->spare_dirblks[i] = alloc_sectors(4);

    // Allocate the User ID (ACL) table. Unused
    superblock->first_uid_sec = alloc_sectors(8);

    // Allocate fnode
    superblock->rootdir_fnode = alloc_sectors(1);
    // We're done writing the disk itself -- now it's time for our filesystem.
    // To speed things up, we hold the structure (not the actual contents) in memory, and only write it out when we're completely done with it
    struct hpfs_fnode_and_data rootdir;
    override_dirband = rootdir_dirblk_lba;
    hpfs_mkdir(&rootdir, "", superblock->rootdir_fnode);

    write_fnode(&rootdir, superblock->rootdir_fnode);

    // Write our housekeeping blocks
    write_sector(fd, superblock, 16);
    write_sector(fd, spareblock, 17);

    // Write our hotfix block
    write_sectors(fd, hotfix_table, 4, spareblock->hotfix_list);

    // Write our indirect blocks list
    write_sectors(fd, bitmap_locations, bitmap_sectors, bitmap_list);

    // Write dirband bitmap
    write_sectors(fd, dirband_bitmap_data, 4, dirband_bitmap_lba);

    uint8_t number_of_unfree_sects[256];
    for (int i = 0; i < 256; i++) {
        int v = 0;
        if (!(i & 1))
            v++;
        if (!(i & 2))
            v++;
        if (!(i & 4))
            v++;
        if (!(i & 8))
            v++;
        if (!(i & 16))
            v++;
        if (!(i & 32))
            v++;
        if (!(i & 64))
            v++;
        if (!(i & 128))
            v++;
        number_of_unfree_sects[i] = v;
    }
    // Write our sectors
    for (int i = 0; i < bands; i++) {
        write_sectors(fd, blk_bitmaps[i], 4, bitmap_locations[i]);
        int unfree = 0;
        for (int j = 0; j < 2048; j++)
            unfree += number_of_unfree_sects[((uint8_t*)(blk_bitmaps[i]))[j]];
        fprintf(stderr, "Band #%d: \n"
                        " Total sectors: %d\n"
                        " Sectors used: %d\n"
                        " Sectors free: %d\n",
            i, 16 << 10, unfree, (16 << 10) - unfree);
    }
}