#include <alloca.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs/hpfs/hpfs.h"

static int fd;
static uint32_t partition_base, partition_size, cseek;

static struct hpfs_superblock* superblock;
static struct hpfs_spareblock* spareblock;
static time_t NOW;
static uint8_t casetbl[256];

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
        perror("write");
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

static void print_dirent_name(struct hpfs_dirent* de)
{
    if (de->namelen == 2) {
        if (de->name_stuff[0] == 1 && de->name_stuff[1] == 1) {
            fprintf(stderr, "/<SPECIAL>/ ..\n");
            return;
        }
    } else if (de->namelen == 1) {
        if (de->name_stuff[0] == 0xFF) {
            fprintf(stderr, "/<SPECIAL>/ END\n");
            return;
        }
    }
    for (unsigned int i = 0; i < de->namelen; i++)
        fprintf(stderr, "%c", de->name_stuff[i]);
    fprintf(stderr, "\n");
}
static void print_fnode_name(struct hpfs_fnode* fn)
{
    unsigned int l = 15;
    if (fn->namelen < l)
        l = fn->namelen;
    for (unsigned int i = 0; i < fn->namelen; i++)
        fprintf(stderr, "%c", fn->name15[i]);
    fprintf(stderr, "\n");
}
static void print_dirblk(struct hpfs_dirblk* db)
{
    int offset = 0x14, id = 0;
    struct hpfs_dirent* de = (struct hpfs_dirent*)&db->data[0];
    while (1) {
        fprintf(stderr, "%d: [at=%d dl=%x] ", id++, offset, *(uint32_t*)(((void*)de) + de->size - 4));
        print_dirent_name(de);
        offset += de->size;
        if (de->size == 0) {
            abort();
        }
        if (de->flags & HPFS_DIRENT_FLAGS_DUMMY_END) {
            break;
        }
        de = ((void*)de) + de->size;
    }
    printf("Computed length: %d Actual length: %d\n", offset, db->first_free);
}

// Sort of like strcmp, but stricter
// Returns:
//  0 if the strings are equal
//  >0 if x comes before y
//  <0 if y comes before x
// DO NOT assume anything else about these values but their signs and whether they're zero or not.
static int fncompare(char* x, int xlen, char* y, int ylen, int casesens)
{
    while (1) {
        if ((xlen | ylen) == 0) // End of both strings, all equal so far
            return 0;
        if (!ylen) // end of y
            return 1;
        if (!xlen) // end of x
            return -1;
        int xc = *x, yc = *y, dc;
        if (casesens) {
            dc = xc - yc;
            if (dc == 0) {
                x++;
                xlen--;
                y++;
                ylen--;
            } else
                return dc;
        } else {
            dc = casetbl[xc] - casetbl[yc];
            if (dc == 0) {
                x++;
                xlen--;
                y++;
                ylen--;
            } else
                return dc;
        }
    }
}

// ============================================================================
// Sector allocation routines
// ============================================================================
static int lowest_sector_used = 0, dirband_sectors_used = 0;
static uint32_t *dirband_bitmap_data, **blk_bitmaps;
static int sector_unoccupied(uint32_t sec)
{
    uint32_t* bitmap = blk_bitmaps[sec >> 14];
    int offset = sec & 0x3FFF;
    return bitmap[offset >> 5] & (1 << (offset & 31));
}
static int dirband_sector_unoccupied(uint32_t sec)
{
    if (sec >= superblock->dir_band_sectors)
        return 0;
    return dirband_bitmap_data[sec >> 5] & (1 << (sec & 31));
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
static void mark_dirband_sectors_used(int sec, int count)
{
    if (count == 0)
        return;

    int offset = sec & 0x3FFF;
    for (int i = 0; i < count; i++) {
        // XXX -- there are probably faster ways to do this
        dirband_bitmap_data[offset >> 5] &= ~(1 << (offset & 31));
        offset++;
    }
}
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
// Determine the number of contiguous free sectors
static uint32_t find_extent(uint32_t secs)
{
    while (!sector_unoccupied(lowest_sector_used))
        lowest_sector_used++;
    int x = 0;
    for (unsigned int i = 0; i < secs; i++) {
        if (!sector_unoccupied(i + lowest_sector_used))
            break;
        x++;
    }
    return x;
}

// Try to allocate a bunch of sectors from the directory band, but if there's nothing left then allocate from the main band.
// Strictly speaking, using the dirband is optional, but HPFS would like us to use it.
static uint32_t alloc_dirband_sectors(int count)
{
    if ((dirband_sectors_used + count) >= superblock->dir_band_sectors)
        return alloc_sectors(count);

    int dirband_base = superblock->dir_band_start_sec;
    while (!dirband_sector_unoccupied(dirband_sectors_used))
        dirband_sectors_used++;
    int retv = dirband_sectors_used;
    for (int i = 1; i < count; i++)
        if (!dirband_sector_unoccupied(retv + i)) {
            retv += i;
            i = 0; // restart from the beginning
        }
    mark_dirband_sectors_used(retv, count);
    fprintf(stderr, " > alloc: %x\n", retv + dirband_base);
    return retv + dirband_base;
}

// We hold all our fnodes, data, etc. data in a hash table for easy lookup when the time comes to write them to disk
// We use multiple hashing and a linked list
#define HASHING_ATTEMPTS 2
enum {
    SECTOR_ENTRY_NONE,
    SECTOR_ENTRY_DIRBLK,
    SECTOR_ENTRY_ALSEC,
    SECTOR_ENTRY_FNODE,
    SECTOR_ENTRY_DATA
};
static const char* names[] = {
    "none",
    "dirblk",
    "alsec",
    "fnode",
    "data"
};

struct ht_entry {
    uint32_t sector;
    int type;
    void* data;

    void* next;
};

#define HASHTABLE_ENTRIES (1024)
static inline uint32_t hash_value(uint32_t key)
{
    return (key << 3 + key >> 2 + key) & (HASHTABLE_ENTRIES - 1); // random hash idea idk
}
static struct ht_entry* ht;
static void ht_init(void)
{
    ht = malloc(HASHTABLE_ENTRIES * sizeof(struct ht_entry));
    for (int i = 0; i < HASHTABLE_ENTRIES; i++) {
        ht[i].sector = ht[i].type = SECTOR_ENTRY_NONE;
        ht[i].next = NULL;
    }
}

static void ht_add(uint32_t sector, int type, void* data)
{
    int hash = sector;
    for (int i = 0; i < HASHING_ATTEMPTS; i++) {
        hash = hash_value(hash);
        if (ht[hash].sector == 0) {
            // we have found an open spot
            ht[hash].sector = sector;
            ht[hash].type = type;
            ht[hash].data = data;
            ht[hash].next = NULL;
            return;
        }
    }

    // linked list time
    struct ht_entry *hte = &ht[hash], *next;
    while (hte->next)
        hte = hte->next;
    next = malloc(sizeof(struct ht_entry));
    next->data = data;
    next->next = NULL;
    next->sector = sector;
    next->type = type;
    hte->next = next;
    return;
}

static void* ht_get(uint32_t sector, int type)
{
    int hash = sector;
    for (int i = 0; i < HASHING_ATTEMPTS; i++) {
        hash = hash_value(hash);
        if (ht[hash].sector == sector) {
            // we have found an open spot
            if (ht[hash].type != type) {
                fprintf(stderr, "Incorrect sector type at sector 0x%x! (in ht=%s wanted=%s)\n", sector, names[ht[hash].type], names[type]);
                abort();
            }
            return ht[hash].data;
        }
    }
    // linked list time
    struct ht_entry *hte = &ht[hash], *next;
    while (1) {
        if (hte->sector == sector) {
            // we have found an open spot
            if (hte->type != type) {
                fprintf(stderr, "Incorrect sector type at sector 0x%x! (in ht=%s wanted=%s)\n", sector, names[ht[hash].type], names[type]);
                exit(-1);
            }
            return hte->data;
        }
        if (hte->next)
            hte = hte->next;
        else {
            fprintf(stderr, "Invalid sector reference at 0x%x\n", sector);
            exit(-1);
        }
    }
}

static void ht_writeback(struct ht_entry* hte)
{
    switch (hte->type) {
    case SECTOR_ENTRY_FNODE:
#if PRINT_TYPES
        fprintf(stderr, "FNODE at %x (nam15: ", hte->sector);
        print_fnode_name(hte->data);
#endif
        if (hte->sector == 0) {
            fprintf(stderr, "ERROR: sector should not be zero (likely a bug)\n");
            exit(-1);
        }
        write_sector(fd, hte->data, hte->sector);
        free(hte->data);
        break;
    case SECTOR_ENTRY_DIRBLK:
#if PRINT_TYPES
        printf("DIRBLK at %x\n", hte->sector);
#endif
        if (hte->sector == 0) {
            fprintf(stderr, "ERROR: sector should not be zero (likely a bug)\n");
            exit(-1);
        }
        if (((struct hpfs_dirblk*)hte->data)->signature != HPFS_DIRBLK_SIG) {
            fprintf(stderr, "Dirblk has wrong sig\n");
            abort();
        }
        write_sectors(fd, hte->data, 4, hte->sector);
        free(hte->data);
        break;
    case SECTOR_ENTRY_ALSEC:
#if PRINT_TYPES
        printf("ALSEC at %x\n", hte->sector);
#endif
        if (hte->sector == 0) {
            fprintf(stderr, "ERROR: sector should not be zero (likely a bug)\n");
            exit(-1);
        }
        if (((struct hpfs_alsec*)hte->data)->signature != HPFS_ALSEC_SIG) {
            fprintf(stderr, "Alsec has wrong sig\n");
            abort();
        }
        write_sector(fd, hte->data, hte->sector);
        free(hte->data);
        break;
    }
}

static struct hpfs_dirblk* hpfs_new_dirblk(uint32_t parent_lba)
{
    struct hpfs_dirblk* db = calloc(4, 512);
    db->parent_lba = parent_lba;
    db->signature = HPFS_DIRBLK_SIG;
    db->this_lba = alloc_dirband_sectors(4);

    ht_add(db->this_lba, SECTOR_ENTRY_DIRBLK, db);
    return db;
}

static struct hpfs_fnode* hpfs_new_fnode(uint32_t parent_lba, uint32_t* this_lba)
{
    struct hpfs_fnode* fn = calloc(1, 512);
    int lba = alloc_sectors(1);
    *this_lba = lba;
    fn->signature = HPFS_FNODE_SIG;
    fn->container_dir_lba = parent_lba;
    ht_add(lba, SECTOR_ENTRY_FNODE, fn);
    return fn;
}

// ===================================================
// Everything to do with DIRENTs and DIRBLKS (B-Trees)
// ===================================================
// The general routine for adding goes as such:
//  if (possible to add more entries to dirblk)
//  then
//      append entry to dirblk
//  else
//      split dirblk into half
//      send median to parent
//  endif

// TODO: for POSIX subsystem
#define CASESENS(de) 0

// Whether dirblk is at the top of the chain
#define DIRBLK_IS_TOP(blk) blk->change & 1
// Maximum possible dirent size, including overflow
#define MAX_DIRENT_SIZE (2048 + 0x124)
// Determien whether dirent 'de' is an ending entry
#define DIRENT_IS_END(de) (de->flags & HPFS_DIRENT_FLAGS_DUMMY_END)
// Gets LBA for dirblk from FNODE
#define FNODE_TO_DIRBLK_LBA(fn) (fn->btree_info_flag & 0x80) ? fn->alnodes[0].physical_lba : fn->alleafs[0].physical_lba
// sets downlink
#define SET_DOWNLINK(de, n) *(uint32_t*)(((void*)de) + de->size - 4) = n
// gets downlink
#define GET_DOWNLINK(de) *(uint32_t*)(((void*)de) + de->size - 4)
// Get dirent data
#define DE_DATA(de) ((struct hpfs_dirent*)(&de->data[0]))

static void hpfs_set_dirblk_len(struct hpfs_dirblk* blk, struct hpfs_dirent* de)
{
    blk->first_free = (uintptr_t)de - (uintptr_t)blk;
}
// Add an end entry to the dirent. Returns a pointer to the next entry.
static struct hpfs_dirent* hpfs_add_end(struct hpfs_dirent* de, uint32_t downlink)
{
    de->size = 32 + (downlink != 0) * 4;
    de->namelen = 1;
    de->name_stuff[0] = 0xFF;
    if (downlink)
        SET_DOWNLINK(de, downlink);
    if (downlink)
        de->flags = HPFS_DIRENT_FLAGS_DUMMY_END | HPFS_DIRENT_FLAGS_BTREE;
    else
        de->flags = HPFS_DIRENT_FLAGS_DUMMY_END;
    return ((void*)de) + de->size;
}
// Add '..' entry to file. Returns a pointer to the next entry.
static struct hpfs_dirent* hpfs_add_dotdot(struct hpfs_dirent* de, uint32_t parent)
{
    de->size = 36; // ((31 + 2) + 3) & ~3
    de->atime = de->ctime = de->mtime = NOW;
    de->attributes = HPFS_DIRENT_ATTR_DIRECTORY;
    de->code_page_index = 0;
    de->ea_size = 0;
    de->filelen = 0;
    de->flags = HPFS_DIRENT_FLAGS_SPECIAL;
    de->flex = 0;
    de->fnode_lba = parent;
    de->namelen = 2;
    de->name_stuff[0] = de->name_stuff[1] = 1;
    return ((void*)de) + de->size;
}

// Count number of dirents in dirblk. Doesn't include end block.
static int hpfs_count_dirents(struct hpfs_dirblk* dirblk)
{
    int offsz = 0, count = 0;
    while (1) {
        struct hpfs_dirent* dirent = (void*)(&dirblk->data[offsz]);
        if (DIRENT_IS_END(dirent))
            return count;
        else {
            count++;
            offsz += dirent->size;
            if (offsz > MAX_DIRENT_SIZE) {
                // We should have encountered an END dirent by now; otherwise, this entry is malformed.
                fprintf(stderr, "hpfs_count_dirents: Dirblk doesn't contain end dirent\n"
                                "dirblk lba: 0x%x parent lba: 0x%x\n",
                    dirblk->this_lba, dirblk->parent_lba);
                abort();
            }
        }
    }
}

#define DOFFS(dirblk, offs) (((void*)dirblk) + offs)
#define DIRBLK_HDR_SIZE 0x14
// Iterate through a dirblk
#define DIRBLK_ITER(varn, dirblk)                                  \
    struct hpfs_dirent* __end = DOFFS(dirblk, dirblk->first_free); \
    for (struct hpfs_dirent* varn = (void*)(dirblk->data); varn < __end; varn = hpfs_next_de(varn))

static inline struct hpfs_dirent* hpfs_next_de(struct hpfs_dirent* dirent)
{
    if (dirent->size == 0) {
        fprintf(stderr, "dirent with size 0\n");
        abort();
    }
    return ((void*)dirent) + dirent->size;
}

// We know that there is enough room within dirblk to insert de.
static void hpfs_insert_dirblk_nosplit(struct hpfs_dirblk* dirblk, struct hpfs_dirent* de)
{
    //fprintf(stderr, "ITERLIST : ");
    //print_dirent_name(de);
    DIRBLK_ITER(cur, dirblk)
    {
#if 0
        if (!DIRENT_IS_END(cur)) {
            fprintf(stderr, "  NAME: ");
            print_dirent_name(cur);
        }
#endif
        if (
            DIRENT_IS_END(cur) // we've reached the end of the chain and found nothing
            || fncompare(cur->name_stuff, cur->namelen, de->name_stuff, de->namelen, CASESENS(de)) > 0 // We have to insert it before the next one, to preserve case-order
            ) {
            // Insert it here
            // Move the other ents back (note that these are most likely overlapping)
            int offset = (uintptr_t)cur - (uintptr_t)dirblk;
            memmove(((void*)cur) + de->size, cur, dirblk->first_free - offset);
            // ... and insert ours here
            memcpy((void*)cur, de, de->size);
            // Update the size
            dirblk->first_free += de->size;
            return;
        }
    }
}

// Look up a dirblk from the hash table
static struct hpfs_dirblk* hpfs_get_dirblk(uint32_t lba)
{
    struct hpfs_dirblk* elt = ht_get(lba, SECTOR_ENTRY_DIRBLK);
    if (!elt) {
        fprintf(stderr, "Lookup of block 0x%x failed\n", lba);
        exit(-1);
    }
    return elt;
}

// Determine the offset in dirblk->data of dirent number #offset
// Also could be used to determine the size of #(offset - 1) dirents
static int hpfs_offsetof_dirblk(struct hpfs_dirblk* dirblk, int offset)
{
    int offs = 0;
    for (int i = 0; i < offset; i++) {
        struct hpfs_dirent* dirent = (void*)(&dirblk->data[offs]);
        if (dirent->flags & HPFS_DIRENT_FLAGS_DUMMY_END) {
            if (i != (offset - 1)) {
                // there were 2 entries in the dirblk, but we asked for the index of the third
                fprintf(stderr, "hpfs_offsetof_dirblk: Out of range (dirblk=0x%x, offset=%d)\n", dirblk->this_lba, offset);
                return -1;
            }
        }
        offs += dirent->size;
    }
    return offs;
}

// Clear a dirblk and set "de" as the only directory entry.
static void hpfs_dirblk_setonly(struct hpfs_dirblk* dirblk, struct hpfs_dirent* de, uint32_t downlink_de, uint32_t downlink_end)
{
    struct hpfs_dirent* d = (void*)&dirblk->data[0];
    memcpy(d, de, de->size);
    if (downlink_de) {
        if (!(d->flags & HPFS_DIRENT_FLAGS_BTREE)) { // If we haven't already, make room
            d->size += 4;
            d->flags |= HPFS_DIRENT_FLAGS_BTREE;
        }
        SET_DOWNLINK(d, downlink_de);
    }
    d = hpfs_next_de(d);
    d = hpfs_add_end(d, downlink_end);
    hpfs_set_dirblk_len(dirblk, d);
}

static struct hpfs_dirblk* temp_dirblk;
static struct hpfs_dirent* temp_dirent;

// Adds a complete dirent entry, de, to dirblk. Creates subdirblks if needed
// Note that "dirblk" should be the lowest leaf in the B-tree.
static void hpfs_add_dirent_internal(struct hpfs_dirblk* dirblk, struct hpfs_dirent* de)
{
    while (1) {
        // Check if we have enough room to insert this into the dirblk
        if ((dirblk->first_free + de->size) < sizeof(struct hpfs_dirblk)) {
            // Yes we do. Insert it in the right location
            hpfs_insert_dirblk_nosplit(dirblk, de);
            return;
        }

        // we need to split
        // TODO: eliminate the use of malloc
        if (!temp_dirblk)
            temp_dirblk = malloc(2048 + 0x124 + 16);

        // Copy the dirblk into our temporary space.
        memcpy(temp_dirblk, dirblk, dirblk->first_free);
        // Insert our new entry into the sorted list
        hpfs_insert_dirblk_nosplit(temp_dirblk, de);
        //print_dirblk(temp_dirblk);

        int is_top = DIRBLK_IS_TOP(dirblk);
        uint32_t new_parent_lba = is_top
            ? dirblk->this_lba // We reuse this dirblk for the top entry
            : dirblk->parent_lba; // No parents are being modified

        // Determine the positions and addresses of our new dirblks
        struct hpfs_dirblk *left = hpfs_new_dirblk(new_parent_lba),
                           *right = is_top ? hpfs_new_dirblk(new_parent_lba) : dirblk,
                           *top = is_top ? dirblk : NULL;

        //printf("Splitting dirblk! (%d - lsn: %x) top? %d\n", dirblk->first_free, dirblk->this_lba, is_top);
        struct hpfs_dirent* median;
        // Determine the median
        {
            int median_index = (temp_dirblk->first_free - 0x14) / 2;
            struct hpfs_dirent* de_mid = DOFFS(temp_dirblk, median_index);
            median = (struct hpfs_dirent*)&temp_dirblk->data[0];
            // Find the first block before the median.
            int n = 0, k = 0;
            while (median < de_mid) {
                k += median->size;
                median = hpfs_next_de(median);
            }

            // Copy the first blocks before the median into the left dirblk.
            // We preserve the header because we worked hard to make it!
            int leftlen = (uintptr_t)median - (uintptr_t)(&temp_dirblk->data[0]);
            struct hpfs_dirent *leftent = (struct hpfs_dirent *)&left->data[0],
                               *lefttemp;
            memcpy(leftent, &temp_dirblk->data[0], leftlen);
            leftent = DOFFS(leftent, leftlen);

            // Duplicate the median's downlink in our end entry, if it has one
            if (median->flags & HPFS_DIRENT_FLAGS_BTREE)
                leftent = hpfs_add_end(leftent, GET_DOWNLINK(median));
            else
                leftent = hpfs_add_end(leftent, 0);
            // Set the length of the dirent
            hpfs_set_dirblk_len(left, leftent);
            //fprintf(stderr, " == LEFT (%x) ==\n", left->this_lba);
            //print_dirblk(left);

            // =======================
            // Handle the right dirblk
            // =======================

            struct hpfs_dirent* after_med = hpfs_next_de(median);
            int after_med_offset = (uintptr_t)after_med - (uintptr_t)&temp_dirblk->data[0],
                copy_len = temp_dirblk->first_free - after_med_offset; // first_free includes DIRBLK_HDR_SIZE
            memcpy(&right->data[0], after_med, copy_len - DIRBLK_HDR_SIZE); // TODO: right calculation is wrong
            // Adjust the length of the first free entry
            right->first_free = copy_len /* + DIRBLK_HDR_SIZE*/;
            //fprintf(stderr, " == RIGHT (%x) ==\n", right->this_lba);
            //print_dirblk(right);

            // ======
            // Fixups
            // ======
            // If we created a new dirblk for left and/or right and we're not dealing with a leaf node, then fix up the block addresses
            if (median->flags & HPFS_DIRENT_FLAGS_BTREE) {
                // Fix up all of our left uplinks to match this parent
                DIRBLK_ITER(cur, left)
                {
                    // For each entry in "left," get the dirblk pointed to by its downlink pointer and modify its parent_lba field.
                    ((struct hpfs_dirblk*)ht_get(GET_DOWNLINK(cur), SECTOR_ENTRY_DIRBLK))->parent_lba = left->this_lba;
                }
                // Fix the right ones too, if needed
                if (is_top) {
                    DIRBLK_ITER(cur, right)
                    {
                        ((struct hpfs_dirblk*)ht_get(GET_DOWNLINK(cur), SECTOR_ENTRY_DIRBLK))->parent_lba = right->this_lba;
                    }
                }
            }
        }

        // sanity check
        int right_ents = right->first_free - DIRBLK_HDR_SIZE;
        int left_ents = left->first_free - DIRBLK_HDR_SIZE;
        int temp_ents = temp_dirblk->first_free - DIRBLK_HDR_SIZE + 36 - de->size; // add an extra end entry
        //printf("l=%d r=%d l+r=%d expected=%d\n", left_ents, right_ents, left_ents + right_ents, temp_ents);

        //printf("left=%d [%x] right=%d [%x] dirblk=%d [%x]\n", left->first_free, left->this_lba, right->first_free, right->this_lba, dirblk->first_free, dirblk->this_lba);

        // We have to be careful about changing links while promoting blocks.
        // The simplest case is a simple promotion from the lowest level to the next highest one. Since the median being promoted doesn't contain any downlinks, we're free to modify it as we see fit.
        // But what if we're promoting a mid-level block? Then it gets messy.
        // Suppose we had a tree that looked like this:
        //
        //         [A   B   C   E   *]
        //         /   /   /   /   /
        //       [1] [2] [3] [4] [5]
        // Block 1 contains entries like "Add" and "Apple"
        // Block 2 contains entries like "Baby" and "Bath"
        // Block 3 contains entries like "Cat" and "C++"
        // So on and so forth
        //
        // The * denotes an ending dirblk
        //
        // Suppose that we promoted D, with its own dirblk chain.
        //
        //         [A   B   C  <D>  E   *]:0
        //         /   /   /   /   /   /
        //       [1] [2] [3] [6] [4] [5]
        //
        // Note that the '6' now simply points to the dirblk number containing entries like "Dirent" and "Directory." It says nothing about its alphabetical position
        // Let's say that it forced us to do another round of promotions:
        //                 8:[C           *]
        //                   /           /
        //       7:[A   B   *]  [D   E  *]:0
        //         /   /   /    /   /   /
        //       [1] [2] [3]  [6] [4] [5]
        //
        // Here's how the dirblks are shunted around:
        //  - Dirent C now contains a link to the left dirblk, and a newly created END entry contains a link to the original.
        //  - The entire right dirblk keeps its downlinks exactly as they were.
        //  - C's downlink now migrates to the ending dirent of the left block.

        //
        // Add the left downlink we created and mark it having one
        //median->flags |= HPFS_DIRENT_FLAGS_BTREE;
        //SET_DOWNLINK(median, left->this_lba); // this line is done later

        if (is_top) { // If the dirblk we just split was the top
            // Promote median -- make it the only thing in the new root
            hpfs_dirblk_setonly(dirblk, median, left->this_lba, right->this_lba);
            return;
        } else {
            // There's no need to add the right dirent -- it's supposed to be attached to the dirent after this one
            if (!temp_dirent)
                temp_dirent = malloc(0x124);

            // In some cases, we might promote the same median over and over again.
            // If so, then temp_dirent == median, and memcpy doesn't like that
            if (temp_dirent != median) // Promote the median into the next one
                memcpy(temp_dirent, median, median->size);

            // Adjust the temporary dirent
            if (!(temp_dirent->flags & HPFS_DIRENT_FLAGS_BTREE)) {
                temp_dirent->flags |= HPFS_DIRENT_FLAGS_BTREE;
                temp_dirent->size += 4;
                SET_DOWNLINK(temp_dirent, left->this_lba);
            }

            // Get the parent
            dirblk = hpfs_get_dirblk(dirblk->parent_lba);
            de = temp_dirent;
        }
    }
}

// Call this routine to add dirents to a dirblk correctly. It traverses the dirblk b-tree and passes the appropriate parameters to hpfs_add_dirent_internal
static void hpfs_add_dirent(struct hpfs_dirblk* dirblk, struct hpfs_dirent* de)
{
top : {
    DIRBLK_ITER(cur, dirblk)
    {
        if (DIRENT_IS_END(cur) || fncompare(cur->name_stuff, cur->namelen, de->name_stuff, de->namelen, CASESENS(cur)) > 0) {
            // The entry goes in between us and the next
            if (cur->flags & HPFS_DIRENT_FLAGS_BTREE) {
                // Go down the tree
                dirblk = ht_get(GET_DOWNLINK(cur), SECTOR_ENTRY_DIRBLK);
                goto top;
            } else { // We've arrived on the lowest entry
                hpfs_add_dirent_internal(dirblk, de);
                return;
            }
        }
    }
}
}
// Get directory fnode that's already on disk
static struct hpfs_fnode* hpfs_get_ondisk_fnode(uint32_t lba)
{
    struct hpfs_fnode* fnode = malloc(512);
    read_sector(fd, fnode, lba);

    if (fnode->signature != HPFS_FNODE_SIG)
        goto fail; // Not a fnode

    if (!(fnode->dir_flag & 1))
        goto fail; // Not a directory

    return fnode;
fail:
    free(fnode);
    return NULL;
}

static struct hpfs_dirblk* hpfs_get_ondisk_dirblk(uint32_t lba)
{
    struct hpfs_dirblk* dirblk = malloc(2048);
    read_sectors(fd, dirblk, 4, lba);

    if (dirblk->signature != HPFS_DIRBLK_SIG)
        goto fail; // Not a fnode
    return dirblk;
fail:
    free(dirblk);
    return NULL;
}

static struct hpfs_dirent* temp_addfiles_de;
static char temp_path[4096];

static void concatpath(char* dest, char* p1, int p1l, char* p2)
{
    // Copy dest name characters
    memcpy(dest, p1, p1l);
    // Add terminating slash to very end
    if (p1[p1l - 1] != '/')
        dest[p1l++] = '/';
    // Copy string and include final pointer.
    strcpy(dest + p1l, p2);
}

// Determine whether a name would be considered "long" by HPFS.
// This routine handles names of the form 'NAME.EXT,' not the 'NAME    EXT' of FAT
// Why does HPFS care about long names? It's not like they're handled differently.
static inline int is_longname(char* name)
{
    int len = strlen(name);
    if (len > 12)
        return 1; // longer than the maximum possible 8.3 filename

    char* dot = strchr(name, '.');
    // COMMAND.COM --> "COMMAND".length === 8
    int pre_len = (uintptr_t)dot - (uintptr_t)name;
    if (pre_len > 8)
        return 1;

    // COMMAND.COM --> "COM".length === 3
    int extlen = (uintptr_t)(name + len) - (uintptr_t)(dot + 1);
    if (pre_len > 3)
        return 1;
    return 0;
}

static inline uint8_t stat2attr(struct stat* statbuf)
{
    uint8_t attr = 0;
    if (S_ISDIR(statbuf->st_mode))
        attr |= HPFS_DIRENT_ATTR_DIRECTORY;
    // that's all i can think of for now
    return attr;
}

// ===============================================
// Everything to do with FNODEs and AL*s (B+Trees)
// ===============================================

static struct hpfs_alsec* hpfs_new_alsec(int flags, uint32_t parent)
{
    int sec = alloc_sectors(1);
    struct hpfs_alsec* al = calloc(1, 512);
    al->signature = HPFS_ALSEC_SIG;
    al->btree_flag = flags;
    al->free_entries = flags & HPFS_BTREE_ALNODES ? HPFS_ALNODES_PER_ALSEC : HPFS_ALLEAFS_PER_ALSEC;
    al->used_entries = 0;
    al->free_entry_offset = sizeof(struct hpfs_btree_header);
    al->parent_lba = parent;
    al->this_lba = sec;
    ht_add(sec, SECTOR_ENTRY_ALSEC, al);
    return al;
}

// A fully general-purpose routine to insert an ALNODE/ALSEC into an array.
static void hpfs_insert_al(void* area, struct hpfs_btree_header* hdr, void* elt)
{
    int size = hdr->flag & HPFS_BTREE_ALNODES ? sizeof(struct hpfs_alnode) : sizeof(struct hpfs_alleaf);
    for (int i = 0; i < hdr->used; i++) {
        // Compares alnode->end_sector_count or alleaf->logical_lba, both of which are at offset 0
        if (*((uint32_t*)area) > *((uint32_t*)elt)) {
            int bytes_to_move = (hdr->used - i) * size;
            memmove(area + size, area, bytes_to_move);
            // ... and insert ours here
            memcpy(area, elt, size);
            hdr->used++;
            hdr->free--;
            hdr->free_offset += size;
            return;
        }
        area += size;
    }
    // Insert it at the very end
    memcpy(area, elt, size);
    hdr->used++;
    hdr->free--;
    hdr->free_offset += size;
    //if(size==12&&hdr->used>40) __asm__("int3");
}

static void hpfs_hdr_compute_free(struct hpfs_btree_header* hdr)
{
    if (hdr->flag & HPFS_BTREE_ALNODES)
        hdr->free_offset = sizeof(struct hpfs_btree_header) + hdr->used * sizeof(struct hpfs_alnode);
    else // alleaf
        hdr->free_offset = sizeof(struct hpfs_btree_header) + hdr->used * sizeof(struct hpfs_alleaf);
}

// Add an ALNODE/ALLEAF to a FNODE, splitting if needed
// We pass fnode_lba explicitly because fnodes, for some reason, don't have this_lba pointers
static void hpfs_insert_into_fnode(struct hpfs_fnode* fnode, uint32_t fnode_lba, void* elt)
{
    struct hpfs_btree_header* hdr = &fnode->btree_hdr;
    if (hdr->free != 0) {
        // There's room in the header to insert it
        hpfs_insert_al(fnode->alleafs, hdr, elt);
    } else {
        // Make a new ALSEC containing a copy of our array, maintaining the HPFS_BTREE_ALNODES flag, if present
        struct hpfs_alsec* alsec = hpfs_new_alsec((hdr->flag & HPFS_BTREE_ALNODES) | HPFS_BTREE_PARENT_IS_FNODE, fnode_lba);

        // Copy the array elements itself
        memcpy(alsec->alleafs, fnode->alleafs, HPFS_ALLEAFS_PER_FNODE * sizeof(struct hpfs_alleaf));

        // Copy B+tree header (we already set flags when we called hpfs_new_alsec), but size it up
        alsec->btree.used = hdr->used;
        int alloc_type = hdr->flag & HPFS_BTREE_ALNODES ? HPFS_ALNODES_PER_ALSEC : HPFS_ALLEAFS_PER_ALSEC;
        alsec->btree.free = alloc_type - hdr->used;
        alsec->btree.free_offset = hdr->free_offset;

        // Insert our new element
        hpfs_insert_al(alsec->alleafs, &alsec->btree, elt);

        // Resize the parent FNODE
        hdr->flag = HPFS_BTREE_ALNODES;
        hdr->free = HPFS_ALNODES_PER_FNODE - 1;
        hdr->used = 1;
        hpfs_hdr_compute_free(hdr);

        // Insert our new element
        fnode->alnodes[0].end_sector_count = -1;
        fnode->alnodes[0].physical_lba = alsec->this_lba;

        // Fix up flags pointed to by child alsec, if needed. The elements in the array no longer point to the parent FNODE
        // Also fix up parent pointers
        if (alsec->btree.flag & HPFS_BTREE_ALNODES)
            for (unsigned int i = 0; i < alsec->btree.used; i++) {
                struct hpfs_alsec* child = ht_get(alsec->alnodes[i].physical_lba, SECTOR_ENTRY_ALSEC);
                // Clear "parent is fnode" flag
                child->btree.flag &= ~HPFS_BTREE_PARENT_IS_FNODE;
                // Set proper parent pointer
                child->parent_lba = alsec->this_lba;
            }
    }
}

static struct hpfs_alleaf* alleaf_temp; // used to temporarily hold alleafs and alnodes before splitting

// Insert an ALNODE/ALLEAF into an ALSEC. If a new entry is created, puts it in aln.
static int hpfs_insert_into_alsec(struct hpfs_alsec* alsec, void* elt, struct hpfs_alnode* aln)
{
    struct hpfs_btree_header* hdr = &alsec->btree;
    if (hdr->free != 0) {
        hpfs_insert_al(alsec->alleafs, hdr, elt);
        return 0;
    }

    int size = hdr->flag & HPFS_BTREE_ALNODES ? sizeof(struct hpfs_alnode) : sizeof(struct hpfs_alleaf);

    // Create temporary space.
    // TODO: We can do this without the malloc.
    if (!alleaf_temp)
        alleaf_temp = malloc(sizeof(struct hpfs_alleaf) * 61);

    // This size should be equivalent to 480 (HPFS_ALLEAFS_PER_ALSEC * sizeof(struct hpfs_alleaf))
    if ((size * hdr->used) != (HPFS_ALLEAFS_PER_ALSEC * sizeof(struct hpfs_alleaf))) {
        fprintf(stderr, "INTERNAL INCONSISTENCY: Size of all used elements: %d (sz/ea=%d) / Expected: %d\n", size * hdr->used, size, HPFS_ALLEAFS_PER_ALSEC * (int)sizeof(struct hpfs_alleaf));
        abort();
    }

    // Allocate a temporary copy here so that we don't clobber the actual ALSEC header
    struct hpfs_btree_header temp_hdr;
    temp_hdr.flag = hdr->flag;
    temp_hdr.free = hdr->free;
    temp_hdr.used = hdr->used;
    // No need to set free_offset -- we don't use it

    // Copy our elements to the new area, and insert our new element
    memcpy(alleaf_temp, alsec->alleafs, size * hdr->used);
    hpfs_insert_al(alleaf_temp, &temp_hdr, elt);

    // Since left is a "twin" of the right ALSEC, both the flags and the parent LBA should be identical
    struct hpfs_alsec *left = hpfs_new_alsec(alsec->btree.flag, alsec->parent_lba), *right = alsec;

    // Fill in fields for the left DIRBLK
    int half = temp_hdr.used >> 1,
        max = temp_hdr.free + temp_hdr.used;
    left->btree.free = max - half;
    left->btree.used = half;
    hpfs_hdr_compute_free(&left->btree);
    memcpy(left->alleafs, alleaf_temp, half * size);
#if 0 // idea on how to do this without malloc
    if(left->LAST_ENTRY > aln) hpfs_insert_al(left->alleafs, &left->btree, aln);
#endif

    // Do the same for the right DIRBLK
    // note that if temp_hdr.used is odd, than half != other_half
    int other_half = temp_hdr.used - half;
    right->btree.free = max - other_half;
    right->btree.used = other_half;
    hpfs_hdr_compute_free(&right->btree);
    // Starting at element `half`, copy `other_half` entries
    memcpy(right->alleafs, ((void*)alleaf_temp) + half * size, other_half * size);
#if 0 // idea on how to do this without malloc
    if(right->LAST_ENTRY > aln) hpfs_insert_al(right->alleafs, &right->btree, aln);
#endif

    if (hdr->flag & HPFS_BTREE_ALNODES) {
        struct hpfs_alnode* alnode_temp = (void*)alleaf_temp;
        aln->end_sector_count = alnode_temp[half - 1].end_sector_count;
        aln->physical_lba = left->this_lba;

        // Adjust left entries' parent LBA
        for (int i = 0; i < half; i++)
            ((struct hpfs_alsec*)ht_get(left->alnodes[i].physical_lba, SECTOR_ENTRY_ALSEC))->parent_lba = left->this_lba;
        
        // Adjust final left entry's ID to -1
        left->alnodes[half - 1].end_sector_count = -1;
    } else // Simply compute the ending entry
        aln->end_sector_count = alleaf_temp[half - 1].logical_lba + alleaf_temp[half - 1].run_size;
    aln->physical_lba = left->this_lba;
    return 1;
}

// I hate how HPFS holds extents in B+trees. I had to rewrite this code multiple times. 
// This is the most elegant way that I could think of doing this.
static void hpfs_add_extent(struct hpfs_fnode* fnode, uint32_t fnode_lba, struct hpfs_alleaf* extent)
{
    fprintf(stderr, "Added extent %d\n", extent->logical_lba);
    struct hpfs_btree_header* hdr = &fnode->btree_hdr;
    // Simplest case: fnode contains just ALLEAFs
    if (!(hdr->flag & HPFS_BTREE_ALNODES)) {
        hpfs_insert_into_fnode(fnode, fnode_lba, extent);
        return;
    }

    // We know now that there is more than one layer.

    uint32_t extent_end = extent->logical_lba + extent->run_size;
    int depth = 0;
    struct hpfs_alnode* alnodes = fnode->alnodes;
    struct hpfs_alsec* alsec;
// This is basically while (hdr->flag & HPFS_BTREE_ALNODES), but C doesn't let out break out of nested loops without goto.
top:
    if (hdr->flag & HPFS_BTREE_ALNODES) {
        // Determine correct location
        for (unsigned int i = 0; i < hdr->used; i++) {
            if (alnodes[i].end_sector_count > extent_end) {
                depth++;
                alsec = ht_get(alnodes[i].physical_lba, SECTOR_ENTRY_ALSEC);
                alnodes = alsec->alnodes; // in the case that we have alsecs, (hdr->flag & HPFS_BTREE_ALNODES) will evaluate to 0
                hdr = &alsec->btree;

                // If we have an ALLEAF array, we'll go back to top: and (hdr->flag & HPFS_BTREE_ALNODES) will evaluate to 0
                goto top;
            }
        }
        // We've looked through the whole tree but we still can't find it.
        fprintf(stderr, "Can't find alsec for extent! (file offset=%08x disk lba=%08x len=%d)\n", extent->logical_lba, extent->physical_lba, extent->run_size);
        exit(1);
    }

    // Add ALLEAF into ALSEC
    struct hpfs_alnode aln;
    int result = hpfs_insert_into_alsec(alsec, extent, &aln);
    if (!result)
        return; // our job is done
    // We've already handled the bottom layer
    depth--;
    while (depth >= 0) {
        if (depth == 0) {
            hpfs_insert_into_fnode(fnode, fnode_lba, &aln);
            return;
        } else {
            // Get parent ALSEC
            alsec = ht_get(alsec->parent_lba, SECTOR_ENTRY_ALSEC);
            // We pass the same entry two times, but that's fine since they're not read/written at the same time
            result = hpfs_insert_into_alsec(alsec, &aln, &aln);
            // If no promotion is necessary, then return
            if (!result)
                return;
            depth--;
        }
    }
}
static int add_host_files(struct hpfs_dirblk* dirblk, char* hostdir);

static uint8_t temp_data[512];
static int add_host_dirent(struct hpfs_dirblk* dirblk, char* hostdir, struct dirent* host_de)
{
    struct stat statbuf;
    int p1l, p2l;
    char* npath = alloca((p1l = strlen(hostdir)) + 1 + (p2l = strlen(host_de->d_name)) + 1);
    concatpath(npath, hostdir, p1l, host_de->d_name);

    if (p2l > 254) {
        fprintf(stderr, "Name '%s' is too long to fit in a HPFS volume. Truncating to first 254 characters.\n", host_de->d_name);
        p2l = 254;
    }

    if (stat(npath, &statbuf) < 0)
        return -1;
    uint8_t attr = stat2attr(&statbuf);
    if (is_longname(host_de->d_name))
        attr |= HPFS_DIRENT_ATTR_LONGNAME;

    // Create fnode and populate it.
    uint32_t lba;
    struct hpfs_fnode* fn = hpfs_new_fnode(dirblk->parent_lba, &lba);
    fn->dir_flag = (attr & HPFS_DIRENT_ATTR_DIRECTORY) != 0;
    fn->filelen = statbuf.st_size;
    fn->namelen = p2l;
    fn->btree_info_flag = 0;
    fn->free_entries = HPFS_ALLEAFS_PER_FNODE;
    fn->free_entry_offset = sizeof(struct hpfs_btree_header); // 0x40 is the beginning of the internal AL* table
    int offset = p2l - 15;
    if (offset < 0)
        offset = 0;
    memcpy(fn->name15, &host_de->d_name[offset], p2l >= 15 ? 15 : p2l);

#if 0
    fprintf(stderr, "%s\n", host_de->d_name);
    if (strcmp("softfloat.c", host_de->d_name) == 0)
        __asm__("int3");
#endif

    // Create dirent and fnode
    // The Linux HPFS driver doesn't like it when the downlink space is reserved ahead of time.
    temp_addfiles_de->size = (0x1F + /*4 + */ p2l + 3) & ~3; // (sizeof dirent_header + /*sizeof downlink */+ sizeof name + rounding_fudge) & ~3
    temp_addfiles_de->atime
        = temp_addfiles_de->ctime = temp_addfiles_de->mtime = NOW;
    temp_addfiles_de->attributes = attr;
    temp_addfiles_de->code_page_index = 0;
    temp_addfiles_de->ea_size = 0;
    temp_addfiles_de->filelen = statbuf.st_size;
    temp_addfiles_de->flags = 0; // will be filled in later
    temp_addfiles_de->flex = 0;
    temp_addfiles_de->fnode_lba = lba;
    temp_addfiles_de->namelen = p2l;
    memcpy(temp_addfiles_de->name_stuff, host_de->d_name, p2l);
    //SET_DOWNLINK(temp_addfiles_de, 0);
    hpfs_add_dirent(dirblk, temp_addfiles_de);

    if (attr & HPFS_DIRENT_ATTR_DIRECTORY) {
        struct hpfs_dirblk* newdir = hpfs_new_dirblk(lba);
        newdir->change = 1;
        struct hpfs_dirent* entn = hpfs_add_dotdot(DE_DATA(newdir), dirblk->parent_lba);
        entn = hpfs_add_end(entn, 0); // no downlinks yet
        hpfs_set_dirblk_len(newdir, entn);

        // Attach this dirblk to the fnode
        fn->btree_info_flag = 0; // ALLEAFs
        fn->alleafs[0].logical_lba = 0;
        fn->alleafs[0].physical_lba = newdir->this_lba;
        fn->alleafs[0].run_size = 0;
        fn->alleafs[1].logical_lba = -1;

        add_host_files(newdir, npath);
    } else {
        // Add file data, if necessary.
        //if (strcmp(host_de->d_name, "a.zip") == 0)
        if (statbuf.st_size != 0) { // If we have zero-length files, then we just keep them as they are
            uint32_t secs = (statbuf.st_size + 511) >> 9, offset = 0;
            int fd2 = open(npath, O_RDONLY);
            while (secs > 0) {
                uint32_t x = find_extent(secs);
#if 0 // set this to 1 if you want to try creating files with lots and lots of extents
                x = 1;
#endif
                uint32_t secloc = alloc_sectors(x);

                struct hpfs_alleaf alleaf;
                alleaf.logical_lba = offset;
                alleaf.physical_lba = secloc;
                alleaf.run_size = x;
                hpfs_add_extent(fn, lba, &alleaf);

                // Copy data from file
                for (unsigned int i = 0; i < x; i++) {
                    if (read(fd2, temp_data, 512) < 0) {
                        perror("read file");
                        exit(-1);
                    }
                    write_sector(fd, temp_data, secloc + i);
                }

                secs -= x;
                offset += x;
            }
            //abort();
        }
    }
}

// Add references to files in host directory 'hostdir' into in-image 'dirblk'
static int add_host_files(struct hpfs_dirblk* dirblk, char* hostdir)
{
    // We need to run the following steps:
    // For each element in the directory:
    //  Add dirent (splitting as needed)
    //  Attach fnode to dirent
    DIR* dir = opendir(hostdir);
    struct dirent* entry;
    if (!dir) {
        fprintf(stderr, "Error opening directory '%s'\n", hostdir);
        exit(-1);
    }

    if (!temp_addfiles_de)
        temp_addfiles_de = calloc(1, 0x124);

    while ((entry = readdir(dir)) != NULL) {
        // skip . and .. entries
        int len = strlen(entry->d_name);
        if (len <= 2) {
            // '.' can lead to '.\0' or '..'
            if (entry->d_name[0] == '.' && (entry->d_name[1] == 0 || entry->d_name[1] == '.'))
                continue;
        }

        add_host_dirent(dirblk, hostdir, entry);
    }
    closedir(dir);
}

static void parse_partition(int partid)
{
    uint8_t mbr[512];
    read_sector(fd, mbr, 0);
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        fprintf(stderr, "No 55AA signature\n");
        exit(-1);
    }
    int pt = 0x1BE;
    if (partid == -1) {
        for (int i = 0; i < 4; i++) {
            if (mbr[pt + 4] == 7) // Use this partition since it's likely HPFS
                goto done;
            pt += 0x10;
        }
        fprintf(stderr, "Unable to find partition with type HPFS. Perhaps manually specify a partition or reformat?\n");
        exit(1);
    } else {
        if (partid >= 4 || partid < 0) {
            fprintf(stderr, "Partition ID out of bounds\n");
            exit(-1);
        }
        pt += partid << 4;
    }
done:
#define READ32(n) (mbr[n]) | (mbr[n + 1]) << 8 | (mbr[n + 2]) << 16 | (mbr[n + 3]) << 24
    partition_base = READ32(pt + 8);
    partition_size = READ32(pt + 12);
#undef READ32
}

// Parse fixed-position blocks
static void parse_fixed_blocks(void)
{
    struct hpfs_bpb bpb;
    read_sector(fd, &bpb, 0);
    if (bpb.jmpboot[0] != 0xEB || bpb.boot_magic[0] != 0x55 || bpb.boot_magic[1] != 0xAA || bpb.bytes_per_sector != 512) {
        fprintf(stderr, "Invalid BPB fields\n");
        exit(-1);
    }

    superblock = calloc(1, 512);
    read_sector(fd, superblock, 16);
    if (superblock->signature[0] != HPFS_SUPER_SIG0 || superblock->signature[1] != HPFS_SUPER_SIG1 || superblock->version != 2) {
        fprintf(stderr, "Invalid superblock signature\n");
        exit(-1);
    }

    spareblock = calloc(1, 512);
    read_sector(fd, spareblock, 17);
    if (spareblock->signature[0] != HPFS_SPARE_SIG0 || spareblock->signature[1] != HPFS_SPARE_SIG1) {
        fprintf(stderr, "Invalid spareblock signature\n");
        exit(-1);
    }
}

int main(int argc, char** argv)
{
    int raw_part = 0, partid = -1;
    char *dir = NULL, *img = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && strlen(argv[i]) == 2) {
            char* arg;
            switch (argv[i][1]) {
#define ARG()                                                   \
    i++;                                                        \
    if (i == argc)                                              \
        fprintf(stderr, "Expected argument to: %s\n", argv[i]); \
    arg = argv[i];
            case 'd':
                ARG();
                dir = arg;
                break;
            case 'p':
                ARG();
                partid = atoi(arg);
                break;
            case 'i':
                raw_part = 1;
                break;
            case 'h':
                fprintf(stderr,
                    "hpfsimg - Install files onto a HPFS image\n"
                    "Usage: hpfsimg [-d rootdir] [-p partid] [-E] [-i] image\n"
                    "Options:\n"
                    " -d <dir>  Makes a copy of this directory in the HPFS image\n"
                    " -p <n>    Select partition number to install on (default: first with type of 7)\n"
                    " -E        Enable EA-based extensions (i.e. case-sensitivity, requires OS support)\n"
                    " -i        Specifies a raw HPFS partition instead of an entire disk\n");
                exit(1);
                break;
            default:
                fprintf(stderr, "Unknown option: %s. Try '-h'\n", argv[i]);
                exit(1);
            }
        } else
            img = argv[i];
    }

    // Make sure we have all our variables set as they should
    if (!dir || !img) {
        fprintf(stderr, "No directory or image specified!\n");
        exit(-1);
    }

    fd = open(img, O_RDWR);
    if (fd < 0) {
        perror("open");
        exit(-1);
    }

    for (int i = 0; i < 256; i++) {
        if (i >= 'a' && i <= 'z')
            casetbl[i] = i - 'a' + 'A';
        else
            casetbl[i] = i;
    }

    NOW = time(NULL);
    ht_init();

    // What we do here
    //  - Validate HPFS
    //  - Load HPFS structures
    //  - Create dirblk structures
    //  - Populate image with data
    //  - Write back all structures

    if (!raw_part)
        parse_partition(partid);
    parse_fixed_blocks();

    uint32_t *band_bitmaps,
        bands = (superblock->sectors_in_partition + 0x3FFF) >> 14;
    {
        // Count how many sectors the band bitmap is going to cover
        int band_bitmaps_count = (bands + 127) >> 7;
        // Allocate bitmap blocks list
        band_bitmaps = malloc(band_bitmaps_count * 512);
        // Allocate our block bitmaps master table, which we use in "alloc_sectors" and friends
        blk_bitmaps = malloc(sizeof(uint32_t*) * bands);
        // Read sectors
        read_sectors(fd, band_bitmaps, band_bitmaps_count, superblock->list_bitmap_secs);
        // Read each bitmap
        for (unsigned int i = 0; i < bands; i++) {
            blk_bitmaps[i] = malloc(2048);
            read_sectors(fd, blk_bitmaps[i], 4, band_bitmaps[i]);
        }
    }

    // Read dirband bitmap
    dirband_bitmap_data = malloc(2048);
    read_sectors(fd, dirband_bitmap_data, 4, superblock->dir_band_bitmap);

    // Read dirblk fnode
    struct hpfs_fnode* rootdir_fblock = hpfs_get_ondisk_fnode(superblock->rootdir_fnode);
    if (!rootdir_fblock) {
        fprintf(stderr, "Unable to open root directory\n");
        exit(-1);
    }

    struct hpfs_dirblk* rootdir = hpfs_get_ondisk_dirblk(FNODE_TO_DIRBLK_LBA(rootdir_fblock));
    if (!rootdir) {
        fprintf(stderr, "Unable to open root directory\n");
        exit(-1);
    }

    add_host_files(rootdir, dir);

    // Write back rootdir
    write_sectors(fd, rootdir, 4, FNODE_TO_DIRBLK_LBA(rootdir_fblock));

    // Write back hash table
    for (int i = 0; i < HASHTABLE_ENTRIES; i++) {
        struct ht_entry *hte = &ht[i], *old;
        int init = 1;
        do {
            ht_writeback(hte);
            old = hte;
            hte = hte->next;
            if (!init)
                free(old);
            init = 0;
        } while (hte);
    }
    free(ht);

    // write back dirband bitmap
    write_sectors(fd, dirband_bitmap_data, 4, superblock->dir_band_bitmap);
    free(dirband_bitmap_data);

    // write back block bitmaps
    for (unsigned int i = 0; i < bands; i++) {
        write_sectors(fd, blk_bitmaps[i], 4, band_bitmaps[i]);
        free(blk_bitmaps[i]);
    }
    free(blk_bitmaps);
    free(band_bitmaps);

    free(rootdir_fblock);
    free(rootdir);
}