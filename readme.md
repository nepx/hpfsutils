# HPFS Utilities

HPFS is a relatively obscure filesystem originally created for OS/2. There seem to be a dearth of open-source HPFS utilities, so I wrote some. 

Although these tools have been used and shown to work, they have not been rigorously tested. I'd recommend using them on blank disk images, perhaps for an operating system project. Both partitioned disk images (created with `fdisk` and the like) and raw partition images can be used. 

Even if you're not interested in the utilities themselves, hopefully the B-tree and B+tree algorithms will be of some use to developers of other HPFS-related tools. 

None of these tools require dependencies, other than the C standard library. Each C source file in the root directory is a standalone utility and should be compiled individually. Because they're so simple. 

This repository also contains Eberhard Mattes's fst (File System Tool), which was crucial for verifying generated HPFS utilities. 

## `mkhpfs`

Creates a fresh HPFS filesystem on a partition. It creates all the necessary structures on-disk, populates them, and adds a root directory. Due to the lack of documentation on the filesystem and a lack of tools to experiment with, I've been unable to determine what happens if you have a disk that's an unusual size. For best results, ensure that your partition is a multiple of 8 MB, or at least `(disk_size_in_mb % 16) < 8`. 

## `hpfsimg`

Copies a directory and its contents on the host to a HPFS-partitioned disk image. This is done recursively, so all subdirectories have their contents copied too. Originally, this tool was merged with `mkhpfs`, but I decided to split it into two tools after the source files grew too long. 

All file metadata is stored in memory before being written to disk (but, to save memory, file data is written directly to disk), meaning that if something goes wrong with creating on-disk structures, it's not going to corrupt the volume (file data only goes into unallocated sectors, so future reads will treat it like harmless garbage data). 

Directories of up to 5,000 entries have been tested, and files with up to 23,000 extents have been successfully created. That's not to say that those are limits -- you can probably go a lot higher, but those were the tested limits. In practice, most files should have few extents, less than 10. 

## `inspect`

Dumps information about HPFS volume. I wrote this early into the creation of `hpfsutils`, so it uses a slightly different set of options. It's useful for determining raw values of various fields. 