/*
 * EXT2 Filesystem.
 *
 * (C) 2018.07.13 BiscuitOS <buddy.zhang@aliyun.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/malloc.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/ext2_fs.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/stat.h>
#include <linux/unistd.h>
#include <linux/fcntl.h>

#include <asm/bitops.h>

#include <demo/debug.h>

/* Superblock on Disk */
#define SUPER_BLOCK      0x1

/*
 * Disk Organization
 *  An Ext2 file system start with a superblock located at offset 1024
 *  from the start of the volume. This is block 1 for a 1KiB block 
 *  formatted volume or within block 0 for larger block size. Note that
 *  the size of the superblock is constant regardless of the block size.
 *
 *
 *  BlockSize = 1KiB
 *
 *  | <- block0 -> | <---- block1 ----> |
 *  +--------------+--------------------+---------------------------+
 *  |              |                    |                           |
 *  |     Boot     |     Superblock     | ....                      |
 *  |              |                    |                           |
 *  +--------------+--------------------+---------------------------+
 *  
 *  BlockSize = 2KiB
 *
 *  | <----------- block 0 -----------> |
 *  +--------------+--------------------+---------------------------+
 *  |              |                    |                           |
 *  |     Boot     |     Superblock     | ....                      |
 *  |              |                    |                           |
 *  +--------------+--------------------+---------------------------+
 * 
 *  BlockSize = 4KiB
 *
 *  | <----------------------- block0 -----------------------> |
 *  +--------------+--------------------+---------------------------+
 *  |              |                    |                           |
 *  |     Boot     |     Superblock     | ....                      | 
 *  |              |                    |                           |
 *  +--------------+--------------------+---------------------------+
 * 
 *  On the next block(s) following the superblock, is the Block Group
 *  Descriptor Table. Which provides an overview of how the volume is 
 *  split into block groups and where to find the inode bitmap, the
 *  block bitmap, and the inode table for each block group.
 *
 *  In revision 0 of ext2, each block group consists of a copy superblock,
 *  a copy of the block group descriptor table, a block bitmap, and inode
 *  bitmap, an inode table, and data blocks.
 *
 *  With the introduction of revision 1 and the sparse superblock feature
 *  in Ext2, only specific block groups contain copies of the superblock
 *  and block group descriptor table. All block groups still contain the
 *  block bitmap, inode bitmap, inode table, and data blocks. The shadow
 *  copies of the superblock can be located in block groups 0, 1 and powers
 *  of 3, 5 and 7.
 *
 *  The block bitmap and inode bitmap are limited to 1 block each per 
 *  block group, so the total blocks per block group is therefore limited.
 *  (More inofrmation in the Block Size Impact table).
 *
 *  Each data block may also be future divided into "fragments". As of
 *  Linux 2.6.28, support for fragment was still not implemented in the
 *  kernel. It is therefore suggested to ensure the fragment size is 
 *  equal to the block size so as to maintain compatiblity.
 *
 *  Figure: Disk layout, 1KiB blocks
 *
 *  |block0|<- block1 ->|<- block02 ->| block3 | block4 |block05|block28|
 *  +------+------------+-------------+--------+--------+-------+-------+
 *  |      |            |             |        |        |       |       |
 *  |      |            | block group | block  | inode  | inode | data  |
 *  | Boot | Superblock | descriptor  | bitmap | bitmap | table | block |
 *  |      |            |             |        |        |       |       |
 *  +------+------------+-------------+--------+--------+-------+-------+
 * 
 *  Table: Disk Layout, 1KiB blocks
 *
 *  -----------------------------------------------------------------------
 *  Block Offset   Length         Description
 *  -----------------------------------------------------------------------
 *  byte 0         512 Bytes      Boot record (if present)
 *  byte 512       512 Bytes      additional boot record data (if present)
 *
 *  -- block group 0, blocks 1 to 8192 --
 *  
 *  byte 1024      1024 bytes     superblock
 *  block 2        1 block        block group descriptor table
 *  block 3        1 block        block bitmap
 *  block 4        1 block        inode bitmap
 *  block 5        23 blocks      inode table
 *  block 28       1412 blocks    data blocks
 *  -----------------------------------------------------------------------
 *
 *  For the curious, block 0 always points to the first sector of the disk
 *  or partition and will always contain the boot record if one is present.
 *  
 *  The superbloc is always located at byte offset 1024 from the start of
 *  the disk or partition. In a 1KiB block-size formatted file system,
 *  this is block 1, but it will always be block0 (at 1024 bytes within 
 *  block 0) in larger block size file systems.
 *
 *  And here's the organisation of 20MB ext2 filesystem, using 1KiB blocks:
 *
 *  Figure: 20MB ext2 Partition Layout
 *
 *  +----------------+---------------+---------------+---------------+
 *  |                |               |               |               |
 *  |      Boot      | block group 0 | block group 1 | block group 2 |
 *  |                |               |               |               |
 *  +----------------+---------------+---------------+---------------+
 *                       
 *  block group 0
 *
 *  +------------+-------------+--------+--------+-------+-----------+
 *  |            |             |        |        |       |           |
 *  |            | block group | block  | inode  | inode | data      |
 *  | superblock | descriptor  | bitmap | bitmap | table | blocks    |
 *  |            | table       |        |        |       |           |
 *  +------------+-------------+--------+--------+-------+-----------+
 * 
 *  block group 1
 *
 *  +------------+-------------+--------+--------+-------+-----------+
 *  |            |             |        |        |       |           |
 *  |            | block group | block  | inode  | inode | data      |
 *  | superblock | descriptor  | bitmap | bitmap | table | blocks    |
 *  |            | table       |        |        |       |           |
 *  +------------+-------------+--------+--------+-------+-----------+
 * 
 *  bloc group 2
 *
 *  +--------------+--------------+-------------+--------------------+  
 *  |              |              |             |                    |
 *  | block bitmap | inode bitmap | inode table | data blocks        |
 *  |              |              |             |                    |
 *  +--------------+--------------+-------------+--------------------+  
 *  
 *  Table: 20MB ext2 Partition Layout
 *
 *  -----------------------------------------------------------------------
 *  Block Offset   Length         Description
 *  -----------------------------------------------------------------------
 *  byte 0         512 Bytes      Boot record (if present)
 *  byte 512       512 Bytes      additional boot record data (if present)
 *
 *  -- block group 0, blocks 1 to 8192 --
 *
 *  byte 1024      1024 bytes     superblock
 *  block 2        1 block        block group descriptor table
 *  block 3        1 block        block bitmap
 *  block 4        1 block        inode bitmap
 *  block 5        214 blocks     inode table
 *  block 219      7974 blocks    data blocks
 *
 *  -- block group 1, block 8193 to 16384 --
 *
 *  block 8193     1 block        superblock backup
 *  block 8194     1 block        block group descriptor table backup
 *  block 8195     1 block        block bitmap
 *  block 8196     1 block        inode bitmap
 *  block 8197     214 blocks     inode table
 *  block 8408     7974 blocks    data blocks
 *
 *  -- block group 2, block 16385 to 24576 --
 *
 *  block 16385    1 block        block bitmap
 *  block 16386    1 block        inode bitmap
 *  block 16387    214 block      inode table
 *  block 16601    3879 blocks    data blocks
 *  -----------------------------------------------------------------------
 *
 *  The layout on disk is very predictable as long as you know a few basic
 *  information. block size, blocks per group, inodes per group. This
 *  information is all located in, or can be computed from, the superblock
 *  structure.
 *
 *  Nevertheless, unless the image was crafted with controlled parameters,
 *  the position of the various structures on disk (except the superblock)
 *  should never be assumed. Always load the superblock first.
 *
 *  Notice how block 0 is not part of the block group 0 in 1KiB block size
 *  file systems. The reason for this is block group 0 always starts with 
 *  the block containing the superblock. Hence, on 1 KiB block systems,
 *  block group 0 starts at block 1, but on larger block sizes it starts
 *  on block 0. For more information, see the s_first_data_block superblock
 *  entry.
 */

/*
 * Ext2 superblock on disk
 *  An Ext2 file system start with a superblock located at offset 1024
 *  from the start of the volume. This is block 1 for a 1KiB block 
 *  formatted volume or within block 0 for larger block size. Note that
 *  the size of the superblock is constant regardless of the block size.
 *
 *  Superblock layout:
 *
 *  +--------+---------------------+----------------------------------+
 *  | Offset | Constant name       | Description                      |
 *  +--------+---------------------+----------------------------------+
 *  | 0x00   | s_inodes_count      | Inodes count                     |
 *  +--------+---------------------+----------------------------------+
 *  | 0x04   | s_blocks_count      | Blocks count                     |
 *  +--------+---------------------+----------------------------------+
 *  | 0x08   | s_r_blocks_count    | Reserved blocks count            |
 *  +--------+---------------------+----------------------------------+
 *  | 0x0C   | s_free_blocks_count | Free blocks count                |
 *  +--------+---------------------+----------------------------------+
 *  | 0x10   | s_free_inodes_count | Free inodes count                |
 *  +--------+---------------------+----------------------------------+
 *  | 0x14   | s_first_data_block  | First Data Block                 |
 *  +--------+---------------------+----------------------------------+
 *  | 0x18   | s_log_block_size    | Block size                       |
 *  +--------+---------------------+----------------------------------+
 *  | 0x1F   | s_log_frag_size     | Fragment size                    |
 *  +--------+---------------------+----------------------------------+
 *  | 0x20   | s_blocks_per_group  | Blocks per group                 |
 *  +--------+---------------------+----------------------------------+
 *  | 0x24   | s_frags_per_group   | Fragment per group               |
 *  +--------+---------------------+----------------------------------+
 *  | 0x28   | s_inodes_per_group  | Inodes per group                 |
 *  +--------+---------------------+----------------------------------+
 *  | 0x2C   | s_mtime             | Mount time                       |
 *  +--------+---------------------+----------------------------------+
 *  | 0x30   | s_wtime             | Write time                       |
 *  +--------+---------------------+----------------------------------+
 *  | 0x32   | s_mnt_count         | Mount count                      |
 *  +--------+---------------------+----------------------------------+
 *  | 0x34   | s_max_mnt_count     | Maximal mount count              |
 *  +--------+---------------------+----------------------------------+
 *  | 0x36   | s_magic             | Magic signature                  |
 *  +--------+---------------------+----------------------------------+
 *  | 0x38   | s_state             | File system state                |
 *  +--------+---------------------+----------------------------------+
 *  | 0x3A   | s_errors            | Behaviour when detecting errors  |
 *  +--------+---------------------+----------------------------------+
 *  | 0x3C   | s_pad               |                                  |
 *  +--------+---------------------+----------------------------------+
 *  | 0x3E   | s_lastcheck         | time of last check               |
 *  +--------+---------------------+----------------------------------+
 *  | 0x42   | s_checkinterval     | max. time between checks         |
 *  +--------+---------------------+----------------------------------+
 *
 *   The superblock contains all the information about the configuration
 *   of the filesystem. The information in the superblock contains fields
 *   such as the total number of inodes and blocks in the filesystem and
 *   how many are free, how many inodes and blocks are in each block group,
 *   when the filesystem was mounted (and if it was cleanly unmounted), 
 *   when it was modified, what version of the filesystem it is and which
 *   OS created it.
 *
 *   The primary copy of the superblock is stored at an offset of 1024 
 *   bytes from the start of the device, and it is essential to mounting
 *   the filesystem. Since it is so important, backup copies of the 
 *   superblock are stored in block groups throughout the filesytem.
 *
 *   The first version of ext2 (revision 0) stores a copy at the start of
 *   every block group, along with backups of the group descriptor block(s).
 *   Because this can consume a considerable amount of space for filesystems,
 *   later revisions can optionally reduce the number of backup copies by
 *   only putting backups in special groups (this is the sparse superblock
 *   feature). The groups chosen are 0, 1 and powers of 3, 5 and 7.
 *
 *   Revision 1 and higher of the filesystem also store extra fields, 
 *   such as a volume name, a unique identification number, the inode size,
 *   and space for optional filesystem features to store configuration info.
 *
 *   All fields in the superblock (as in all other ext2 structure) are
 *   stored on the Disc in little endian format, so a filesytem is portable
 *   between machines without having to know what machine it was created
 *   on.
 *
 */
static __unused int ext2_superblock(struct super_block *sb)
{
    struct ext2_super_block *es = NULL;
    struct buffer_head *bh;

    /* Obtain ext2 superblock from vfs superblock */
    es = sb->u.ext2_sb.s_es;
    /* If ext2 superblock doesn't exist, we reload superblock from
     * Disk 
     */
    if (!es) {
        if (!(bh = bread(sb->s_dev, SUPER_BLOCK, BLOCK_SIZE))) {
            printk(KERN_ERR "Unable read superblock from Disk\n");
            return -EINVAL;
        }
        es = (struct ext2_super_block *)bh->b_data;
    }
    /*
     * s_inodes_count
     *  32bit value indicating the total number of inodes, both used
     *  and free, in the file system. This value must be lower or 
     *  equal to (s_inode_per_group * number of block groups). It must
     *  be equal to the sum of the inodes defined in each block group.
     */
    printk("Total inodes: %#x\n", (unsigned int)es->s_inodes_count);
    /*
     * s_blocks_count
     *   32bit value indicating the total number of blocks in the
     *   system including all used, free and reserved. This value
     *   must be lower or equal (s_blocks_per_group * number of block
     *   groups). It must be equal to the sum of the blocks defined
     *   in each block group. 
     */
    printk("Total blocks: %#x\n", (unsigned int)es->s_blocks_count);
    /*
     * s_r_blocks_count
     *   32bit value indicating the total number of blocks reserved for
     *   the super user. This is most useful if for some reason a user,
     *   maliciously or not, fill the file system to capacity. The 
     *   super user will have this specified amount of free blocks at
     *   his disposal so he can edit and save configureation file.
     */
    printk("Total reserved blocks: %#x\n", 
                             (unsigned int)es->s_r_blocks_count);
    /*
     * s_free_blocks_count
     *   32bit value indicating the total number of tree blocks, 
     *   including the number of reserved blocks (see s_r_blocks_count).
     *   This is a sum of all free blocks of all the block groups.
     */
    printk("Total free blocks: %#x\n",
                             (unsigned int)es->s_free_blocks_count);
    /*
     * s_free_inodes_count
     *   32bit value indicating the total number of free inodes. This is
     *   a sum of all free inodes of all the block groups.
     */
    printk("Total free inodes: %#x\n",
                             (unsigned int)es->s_free_inodes_count);
    /*
     * s_first_data_block
     *   32bit value identifying the first data block, in other word the
     *   id of the block containing the superblock structure.
     *
     *   Note that this value is always 0 for file system with a block
     *   size larger than 1KB, and always 1 for file systems with a block
     *   size of 1KB. The superblock is always starting at the 1024th
     *   byte of the disk, which normally happens to be the first byte
     *   of the 3rd sector.
     */
    printk("first data block: %#x\n",
                             (unsigned int)es->s_first_data_block);

    /*
     * s_log_block_size
     *   The block size is computed using this 32bit values as the number
     *   of bits to shift left the value 1024. This value may only be
     *   positive.
     *
     *   block size = 1024 << s_log_block_size
     *
     *   Common block size include 1KiB, 2KiB, 4KiB and 8KiB. For 
     *   information about the impact of selecting a block size, see
     *   Impact of Block Sizes.
     *
     */
    printk("The block size: %#x\n",
                           (unsigned int)(1024 << es->s_log_block_size));


    /*
     * s_log_frag_size
     *   The fragment size is computed using this 32bit value as the number
     *   of bits to shift left the value 1024. Note that a negative value
     *   would shift the bit right rather than left.
     *
     *   if (positive)
     *       fragment size = 1024 << s_log_frag_size
     *   else
     *       fragment size = 1024 >> -s_log_frag_size
     */
    if (es->s_log_frag_size >= 0)
        printk("fragment size: %#x\n", 
                   (unsigned int)(1024 << es->s_log_frag_size));
    else
        printk("fragment size: %#x\n",
                   (unsigned int)(1024 >> es->s_log_frag_size));

    /*
     * s_blocks_per_group
     *   32bit value indicating the total number of blocks per group.
     *   This value in combination with s_first_data_block can be used
     *   to determine the block groups boundaries.
     */
    printk("Perblock: %#x\n", (unsigned int)es->s_blocks_per_group);

    /*
     * s_frags_per_group
     *   32bit value indicating the total number of fragments per group.
     *   It is also used to determine the size of the block bitmap of
     *   each block group.
     */
    printk("PerFragment: %#x\n", (unsigned int)es->s_frags_per_group);

    /*
     * s_inodes_per_group
     *   32bit value indicating the total number of inodes per group. 
     *   This is also used to determine the size of the inode bitmap
     *   of each block group. Note that you cannot have more than (block
     *   size in bytes * 8) inodes per group as the inode bitmap must fit
     *   within a single block. This value must be a perfect multiple of
     *   the number of inodes that can fit in a block ((1024 << 
     *   s_log_block_size)/s_inode_size).
     */
    printk("PerInodes: %#x\n", (unsigned int)es->s_inodes_per_group);

    /*
     * s_mtime
     *   Unix time, as defined by POSIX, of the last time the file system
     *   was mounted.
     */
    printk("Mounttime: %#x\n", (unsigned int)es->s_mtime);

    /*
     * s_wtime
     *   Unix time, as defined by POSIX, of the last write access to the
     *   file syste,.
     */
    printk("Last write time: %#x\n", (unsigned int)es->s_wtime);

    /*
     * s_mnt_count
     *   32bit value indicating how many time the file system was mounted
     *   since the last time it was fully verified.
     */
    printk("Mount times: %#x\n", (unsigned int)es->s_mnt_count);

    /*
     * s_max_mnt_count
     *   32bit value indicating the maximum number of times that the file
     *   system may by mounted before a full check is performed.
     */
    printk("Maxmount: %#x\n", (unsigned int)es->s_max_mnt_count);

    /*
     * s_magic
     *   16bit value identifying the file system as Ext2. The value is
     *   currently fixed to EXT2_SUPER_MAGIC of value 0xEF53.
     */
    printk("Magic: %#x\n", (unsigned int)es->s_magic);

    /*
     * s_state
     *   16bit value indicating the file system state. When the file system
     *   is mounted, this state is set to EXT2_ERROR_FS. After the file
     *   system was cleanly unmounted, this value is set to EXT2_VALID_FS.
     *
     *   When mounting the file system, if a valid of EXT2_ERROR_FS is 
     *   encountered it means the file system was not cleanly unmounted 
     *   and most likely contain errors that will need to be fixed.
     *   Typically under Linux this means running fsck.
     *
     *   Table: Defined s_state value
     *
     *    Constant Name              Value       Description
     *   --------------------------------------------------------------
     *    EXT2_VALID_FS              1           Unmounted cleanly
     *    EXT2_ERROR_FS              2           Errors detected
     *   --------------------------------------------------------------
     */
    printk("FState: %#x\n", (unsigned int)es->s_state);

    /*
     * s_errors
     *   16bit value indicating what the file system driver should do
     *   when an error is detected. The follwing value has been defined.
     *
     *   Table: Defined s_errors values
     *
     *    Constant Name          Value   Description
     *   --------------------------------------------------------------
     *    EXT2_ERRORS_CONTINUE   1       continue as if nothing happend
     *    EXT2_ERRORS_RO         2       remount read-only
     *    EXT2_ERRORS_PANIC      3       cause a kernel panic
     *   --------------------------------------------------------------
     */
    printk("s_erros: %#x\n", (unsigned int)es->s_errors);

    /*
     * s_lastcheck
     *   Unix time, as defined by POSIX, of the last file system check.
     */
    printk("LastChecktime: %#x\n", (unsigned int)es->s_lastcheck);

    /*
     * s_checkinterval
     *   Maximum Unix time interval, as defined by POSIX, allowed between
     *   file system checks.
     */
    printk("Maxcheck: %#x\n", (unsigned int)es->s_checkinterval);

    return 0;
}

/*
 * ext2 superblock on vfs
 *   The superblock contains all the information about the configuration
 *   of the filesystem. The information in the superblock contains fields
 *   such as the total number of inodes and blocks in the filesystem and
 *   how many are free, how many inodes and blocks are in each block group,
 *   when the filesystem was mounted (and if it was cleanly unmounted), 
 *   when it was modified, what version of the filesystem it is and which
 *   OS created it.
 *
 *   The primary copy of the superblock is stored at an offset of 1024 
 *   bytes from the start of the device, and it is essential to mounting
 *   the filesystem. Since it is so important, backup copies of the 
 *   superblock are stored in block groups throughout the filesytem.
 *
 *   The first version of ext2 (revision 0) stores a copy at the start of
 *   every block group, along with backups of the group descriptor block(s).
 *   Because this can consume a considerable amount of space for filesystems,
 *   later revisions can optionally reduce the number of backup copies by
 *   only putting backups in special groups (this is the sparse superblock
 *   feature). The groups chosen are 0, 1 and powers of 3, 5 and 7.
 *
 *   Revision 1 and higher of the filesystem also store extra fields, 
 *   such as a volume name, a unique identification number, the inode size,
 *   and space for optional filesystem features to store configuration info.
 *
 *   All fields in the superblock (as in all other ext2 structure) are
 *   stored on the Disc in little endian format, so a filesytem is portable
 *   between machines without having to know what machine it was created
 *   on.
 *
 */
static __unused int ext2_superblock_vfs(struct super_block *sb)
{
    struct ext2_super_block *es;
    struct buffer_head *bh = NULL;
    int i, j, bh_count;

    es = sb->u.ext2_sb.s_es;
    if (!es) {
        if (!(bh = bread(sb->s_dev, SUPER_BLOCK, BLOCK_SIZE))) {
            printk(KERN_ERR "Unable to read superblock from disk.\n");
            return -EINVAL;
        }
        es = (struct ext2_super_block *)bh->b_data;
    }

    /*
     * s_frag_size
     *   32bit value indicating the size of a fragment in bytes. 
     *   The 'EXT2_MIN_FRAG_SIZE' in combination with 's_log_frag_size'
     *   can be used to determine the fragment size.
     */
    sb->u.ext2_sb.s_frag_size = EXT2_MIN_FRAG_SIZE <<
                                       es->s_log_frag_size;

    /*
     * s_frags_per_block
     *   32bit value indicating the total number of fragments per block.
     */
    sb->s_blocksize = EXT2_MIN_BLOCK_SIZE << es->s_log_block_size;
    if (sb->u.ext2_sb.s_frag_size)
        sb->u.ext2_sb.s_frags_per_block = sb->s_blocksize /
               sb->u.ext2_sb.s_frag_size;

    /*
     * s_inodes_per_block
     *   32bit value indicating the total number of inodes per block.
     */
    sb->u.ext2_sb.s_inodes_per_block = sb->s_blocksize /
                     sizeof(struct ext2_inode);

    /*
     * s_frags_per_group
     *   32bit value indicating the total number of fragments per group.
     *   It is also used to determine the size of the block bitmap of
     *   each block group.
     */
    sb->u.ext2_sb.s_frags_per_group = es->s_frags_per_group;
    
    /*
     * s_blocks_per_group
     *   32bit value indicating the total number of blocks per group.
     *   This value in combination with s_first_data_block can be
     *   used to determine the block groups boundaries.
     */
    sb->u.ext2_sb.s_blocks_per_group = es->s_blocks_per_group;

    /*
     * s_inodes_per_group
     *   32bit value indicating the total number of inodes per group.
     *   This is also used to determine the size of the inode bitmap of
     *   each block group. Note that you cannot have more than (block size
     *   in bytes * 8) inodes per group as the inode bitmap must fit within
     *   a signal block. This value must be a perfect multiple of the 
     *   number of inodes that can fit in a block ((1024 < 
     *   s_log_block_size) / s_inode_size).
     */
    sb->u.ext2_sb.s_inodes_per_group = es->s_inodes_per_group;

    /*
     * s_itb_per_group
     *   32bit value indicating the total number of inode table blocks
     *   per group.
     */
    sb->u.ext2_sb.s_itb_per_group = sb->u.ext2_sb.s_inodes_per_group /
                       sb->u.ext2_sb.s_inodes_per_block;

    /*
     * s_desc_per_block
     *   32bit value indicating the total number of group descriptor
     *   per block.
     */
    sb->u.ext2_sb.s_desc_per_block = sb->s_blocksize /
                       sizeof(struct ext2_group_desc);

    /*
     * s_groups_count
     *   32bit value indicating the total number of groups on ext2.
     *   The value 's_blocks_count' indicating the total number of blocks,
     *   and the value 'EXT2_BLOCKS_PER_GROUP' indicating the total number
     *   of block per group, these value in combination with 
     *   's_first_data_block' can be used to determine the total number
     *   of groups on ext2.
     *   
     */
    sb->u.ext2_sb.s_groups_count = (es->s_blocks_count - 
                        es->s_first_data_block + 
                        EXT2_BLOCKS_PER_GROUP(sb) - 1) / 
                        EXT2_BLOCKS_PER_GROUP(sb);
 
    /*
     * s_sbh
     *   The pointer point the buffer containing the ext2 super block.
     */
    sb->u.ext2_sb.s_sbh = bh;

    /*
     * s_es
     *   The pointer point the buffer containing the ext2 super block.
     */
    sb->u.ext2_sb.s_es = es;

    /*
     * s_group_desc
     *   The pointer array points the buffer containing the group
     *   descriptor. On ext2-fs layout, the group descriptor is behind
     *   superblock, as figure:
     *
     *   +------+------------+---------------+---------------+--------+
     *   |      |            |               |               |        |
     *   | Boot | superblock | group_desc[0] | group_desc[1] | ...    |
     *   |      |            |               |               |        |
     *   +------+------------+---------------+---------------+--------+
     *
     *   This value 's_groups_per_block' in combination with 's_groups_count' 
     *   can be used to determine the total number of block for reading.
     * 
     *     count = (s->groups_count + s_groups_per_block - 1) /
     *                          s_groups_per_block
     *
     */
    for (i = 0; i < EXT2_MAX_GROUP_DESC; i++)
        sb->u.ext2_sb.s_group_desc[i] = NULL;
    bh_count = (sb->u.ext2_sb.s_groups_count +
                     EXT2_DESC_PER_BLOCK(sb) - 1) /
                     EXT2_DESC_PER_BLOCK(sb);
    for (i = 0; i < bh_count; i++) {
        sb->u.ext2_sb.s_group_desc[i] = 
                bread(sb->s_dev, SUPER_BLOCK + i + 1, sb->s_blocksize);
        if (!sb->u.ext2_sb.s_group_desc[i]) {
            sb->s_dev = 0;
            for (j = 0; j < i; j++)
                brelse(sb->u.ext2_sb.s_group_desc[j]);
            brelse(bh);
            printk("EXT2-fs: unable to read group descriptor\n");
            return -EINVAL;
        }
    }

    /*
     * s_loaded_inode_bitmaps
     * s_loaded_block_bitmaps
     * unknown
     */

    /*
     * s_inode_bitmap_number
     */
    for (i = 0; i < EXT2_MAX_GROUP_LOADED; i++) {
        sb->u.ext2_sb.s_inode_bitmap_number[i] = 0;
        sb->u.ext2_sb.s_inode_bitmap[i] = NULL;
        sb->u.ext2_sb.s_block_bitmap_number[i] = 0;
        sb->u.ext2_sb.s_block_bitmap[i] = NULL;
    }

    if (bh)
        brelse(bh);

    return 0;
}

/*
 * get_group_desc
 *   The 's_group_desc' is a array which containing the buffer that points
 *   to group descriptor for represented group.
 *
 *   EXT2_DESC_PER_BLOCK indicating the number of group descriptor per
 *   group.
 *
 *   +------+------------+-----------------+-----------------+-------+
 *   |      |            |                 |                 |       |
 *   | Boot | Superblock | s_group_desc[0] | s_group_desc[1] | ..... |
 *   |      |            |                 |                 |       |
 *   +------+------------+-----------------+-----------------+-------+
 *                              |  
 *                              | b_data  
 *                              |
 *   o--------------------------o
 *   |
 *   |
 *   V
 *   0------------+------------+------------+----+------------+-----4k
 *   |            |            |            |    |            |      |
 *   | group_desc | group_desc | group_desc | .. | group_desc | hole |
 *   |            |            |            |    |            |      |
 *   +------------+------------+------------+----+------------+------+
 */
static struct ext2_group_desc *get_group_desc(struct super_block *sb,
             unsigned int block_group, struct buffer_head **bh)
{
    unsigned long group_desc;
    unsigned long desc;
    struct ext2_group_desc *gdp;

    if (block_group >= sb->u.ext2_sb.s_groups_count)
        panic("get_group_desc: block_group > groups_count");

    group_desc = block_group / EXT2_DESC_PER_BLOCK(sb);
    desc = block_group % EXT2_DESC_PER_BLOCK(sb);
    if (!sb->u.ext2_sb.s_group_desc[group_desc])
        panic("Group descriptor not loaded");

    gdp = (struct ext2_group_desc *)
          sb->u.ext2_sb.s_group_desc[group_desc]->b_data;
    if (bh)
        *bh = sb->u.ext2_sb.s_group_desc[group_desc];
    return gdp + desc;
}

/*
 * read_block_bitmap
 *   Read block bitmap from block group. The block bitmap stored
 *   in the block per group. The value of 'bg_block_bitmap' indicating
 *   the block id for the represented group.
 *
 *   The value of 's_block_bitmap_number' indicating the group id
 *   for the represented group.
 *
 *   The value of 's_block_bitmap' indicating the cache for block
 *   bitmap for the represented group.
 */
static void read_block_bitmap(struct super_block *sb,
           unsigned int block_group, unsigned long bitmap_nr)
{
    struct ext2_group_desc *gdp;
    struct buffer_head *bh;

    gdp = get_group_desc(sb, block_group, NULL);
    bh = bread(sb->s_dev, gdp->bg_block_bitmap, sb->s_blocksize);
    if (!bh)
        printk("Cannot read block bitmap\n");
    sb->u.ext2_sb.s_block_bitmap_number[bitmap_nr] = block_group;
    sb->u.ext2_sb.s_block_bitmap[bitmap_nr] = bh;
}

/*
 * load_block_bitmap loads the block bitmap for a block group
 *
 * It maintains a cache for the last bitmaps loaded. This cache is 
 * managed with a LRU algorithm.
 *
 * Notes:
 *   1/ There is one cache per mounted file system
 *   2/ If the file system contains less than EXT2_MAX_GROUP_LOADED
 *      groups, this function reads the bitmap without maintaining a
 *      LRU cache
 */
static int load__block_bitmap(struct super_block *sb,
                         unsigned int block_group)
{
    int i, j;
    unsigned long block_bitmap_number;
    struct buffer_head *block_bitmap;

    if (block_group >= sb->u.ext2_sb.s_groups_count)
        panic("block_group >= groups_count");

    if (sb->u.ext2_sb.s_groups_count <= EXT2_MAX_GROUP_LOADED) {
        if (sb->u.ext2_sb.s_block_bitmap[block_group]) {
            if (sb->u.ext2_sb.s_block_bitmap_number[block_group] !=
                               block_group)
                panic("block_group != block_bitmap_number");
            else
                return block_group;
        } else {
            read_block_bitmap(sb, block_group, block_group);
            return block_group;
        }
    }
    /* LRU */
    for (i = 0; i < sb->u.ext2_sb.s_loaded_block_bitmaps &&
              sb->u.ext2_sb.s_block_bitmap_number[i] != block_group; i++)
        ;
    if (i < sb->u.ext2_sb.s_loaded_block_bitmaps &&
             sb->u.ext2_sb.s_block_bitmap_number[i] == block_group) {
        block_bitmap_number = sb->u.ext2_sb.s_block_bitmap_number[i];
        block_bitmap = sb->u.ext2_sb.s_block_bitmap[i];
        for (j = i; j > 0; j--) {
            sb->u.ext2_sb.s_block_bitmap_number[j] = 
                    sb->u.ext2_sb.s_block_bitmap_number[j - 1];
            sb->u.ext2_sb.s_block_bitmap[j] = 
                    sb->u.ext2_sb.s_block_bitmap[j - 1];
        }
        sb->u.ext2_sb.s_block_bitmap_number[0] = block_bitmap_number;
        sb->u.ext2_sb.s_block_bitmap[0] = block_bitmap;
    } else {
        if (sb->u.ext2_sb.s_loaded_block_bitmaps < EXT2_MAX_GROUP_LOADED)
            sb->u.ext2_sb.s_loaded_block_bitmaps++;
        else
            brelse(sb->u.ext2_sb.s_block_bitmap[EXT2_MAX_GROUP_LOADED - 1]);
        for (j = sb->u.ext2_sb.s_loaded_block_bitmaps - 1; j > 0; j--) {
            sb->u.ext2_sb.s_block_bitmap_number[j] =
                     sb->u.ext2_sb.s_block_bitmap_number[j - 1];
            sb->u.ext2_sb.s_block_bitmap[j] =
                     sb->u.ext2_sb.s_block_bitmap[j - 1];
        }
        read_block_bitmap(sb, block_group, 0);
    }
    return 0;
}

static inline int load_block_bitmap(struct super_block *sb, 
              unsigned int block_group)
{
    if (sb->u.ext2_sb.s_loaded_block_bitmaps > 0 &&
               sb->u.ext2_sb.s_block_bitmap_number[0] == block_group)
        return 0;
    
    if (sb->u.ext2_sb.s_groups_count <= EXT2_MAX_GROUP_LOADED &&
         sb->u.ext2_sb.s_block_bitmap_number[block_group] == block_group &&
         sb->u.ext2_sb.s_block_bitmap[block_group])
        return block_group;

    return load__block_bitmap(sb, block_group);
}

static inline int block_in_use(unsigned long block,
          struct super_block *sb, unsigned char *map)
{
    return test_bit((block - sb->u.ext2_sb.s_es->s_first_data_block) %
                        EXT2_BLOCKS_PER_GROUP(sb), map);
}

static int nibblemap[] = {4, 3, 3, 2, 3, 2, 2, 1, 3, 2, 2, 1, 2, 1, 1, 0};

static unsigned long ext2_counts_free(struct buffer_head *map, 
                             unsigned int numchars)
{
    unsigned int i;
    unsigned long sum = 0;

    if (!map)
        return 0;
    for (i = 0; i < numchars; i++)
        sum += nibblemap[map->b_data[i] & 0xf] +
                     nibblemap[(map->b_data[i] >> 4) & 0xf];

    return (sum);    
}

/*
 * ext2_check_block_bitmap
 *  Check the usage of reserved, used and free block on per group.
 */
static void ext2_check_block_bitmap(struct super_block *sb)
{
    struct buffer_head *bh;
    struct ext2_super_block *es;
    unsigned long desc_count, bitmap_count, x;
    unsigned long desc_blocks;
    int bitmap_nr;
    struct ext2_group_desc *gdp;
    int i, j;

    lock_super(sb);
    es = sb->u.ext2_sb.s_es;
    desc_count = 0;
    bitmap_count = 0;
    gdp = NULL;
    desc_blocks = (sb->u.ext2_sb.s_groups_count +
                    EXT2_DESC_PER_BLOCK(sb) - 1) /
                    EXT2_DESC_PER_BLOCK(sb);
    for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
        gdp = get_group_desc(sb, i, NULL);
        desc_count += gdp->bg_free_blocks_count;
        bitmap_nr = load_block_bitmap(sb, i);
        bh = sb->u.ext2_sb.s_block_bitmap[bitmap_nr];

        /*
         * The superblock is first block in represented group.
         *
         *
         * | <------------------ block group x ------------------> |
         * +------------+-----------------+-----------------+------+
         * |            |                 |                 |      |
         * | superblock | s_group_desc[0] | s_group_desc[1] | ...  |
         * |            |                 |                 |      |
         * +------------+-----------------+-----------------+------+
         *
         * The block of 'superblock' and 's_group_desc[]' must be
         * set on block bitmap on represented group.
         */
        if (!test_bit(0, bh->b_data))
            printk("Superblock in group %d is marked free\n", i);

        for (j = 0; j < desc_blocks; j++)
            if (!test_bit(j + 1, bh->b_data))
                printk("Descriptor block %d in group is marked free\n", j);

        if (!block_in_use(gdp->bg_block_bitmap, sb,
                              (unsigned char *)bh->b_data))
            printk("Block bitmap for group %d is marked free\n", i);

        if (!block_in_use(gdp->bg_inode_bitmap, sb,
                              (unsigned char *)bh->b_data))
            printk("Inode bitmap for group %d is marked free\n", i);

        for (j = 0; j < sb->u.ext2_sb.s_itb_per_group; j++)
            if (!block_in_use(gdp->bg_inode_table + j, sb,
                                (unsigned char *)bh->b_data))
            printk("Block %d of the inode table is marked free\n", j);

        x = ext2_counts_free(bh, sb->s_blocksize);
        if (gdp->bg_free_blocks_count != x)
            printk("Wrong free blocks count for group %d\n", j);

        bitmap_count += x;
    }
    if (es->s_free_blocks_count != bitmap_count)
        printk("Wrong free blocks count in super block\n");
    unlock_super(sb);
}

/*
 * ext2_check_descriptors
 *   Check group descriptor.
 */
static int ext2_check_descriptors(struct super_block *sb)
{
    int i;
    int desc_block = 0;
    unsigned long block = sb->u.ext2_sb.s_es->s_first_data_block;
    struct ext2_group_desc *gdp = NULL;

    for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
        /* 
         * The pointer 's_group_desc' is array which containing buffer
         * that point group descriptor for represented EXT2-fs. And
         * the value of 'EXT2_DESC_PER_BLOCK' indicating the numbe of
         * group descriptor for per block.
         *
         * The block of 's_group_desc'
         * 
         * 0-------------+-------------+-------+-------------+------4k
         * |             |             |       |             |      |
         * | buffer_head | buffer_head | ..... | buffer_head | hole |
         * |             |             |       |             |      |
         * +-------------+-------------+-------+-------------+------+
         * A                    |
         * |                    |
         * |                    |
         * o--s_group_desc      | b_data
         *                      |
         *                      |         
         *                      V
         *                      +------------+------------+-----+------+
         *                      |            |            |     |      |
         *                      | group_desc | group_desc | ... | hole |
         *                      |            |            |     |      |
         *                      +------------+------------+-----+------+
         *
         * 
         */
        if ((i % EXT2_DESC_PER_BLOCK(sb)) == 0)
            gdp = (struct ext2_group_desc *)(
                 sb->u.ext2_sb.s_group_desc[desc_block++]->b_data);

        /*
         * Check bitmap on group descriptor 0
         *   The block of bitmap for represented group must be contained 
         *   from first block to last block for represented group.
         *
         *        | <---------------- block group 0 ----------------> |
         * +------+------------+----+-------------+-------------+-----+----+
         * |      |            |    |             |             |     |    |
         * | boot | superblock | gd | blockBitmap | inodeBitmap | ... | .. |
         * |      |            |    |             |             |     |    |
         * +------+------------+----+-------------+-------------+-----+----+
         *        A <------------ EXT2_BLOCKS_PER_GROUP ------------> |
         *        |
         *        |
         *        |
         *        o--s_first_data_block
         *
         */

        if (gdp->bg_block_bitmap < block || 
                    gdp->bg_block_bitmap >= block +
                        EXT2_BLOCKS_PER_GROUP(sb)) {
            printk("EXT2-fs: block bitmap for group no in group\n");
            return 0;
        }

        if (gdp->bg_inode_bitmap < block ||
                    gdp->bg_inode_bitmap >= block +
                        EXT2_BLOCKS_PER_GROUP(sb)) {
            printk("EXT2-fs: inode bitmap for group no in group\n");
            return 0;
        } 

        if (gdp->bg_inode_table < block ||
                    gdp->bg_inode_table >= block +
                        EXT2_BLOCKS_PER_GROUP(sb)) {
            printk("EXT2-fs: inode table for group no in group\n");
            return 0;
        }
        block += EXT2_BLOCKS_PER_GROUP(sb);
        gdp++;
    }
    return 1;
}

static void read_inode_bitmap(struct super_block *sb,
       unsigned long block_group, unsigned int bitmap_nr)
{
    struct ext2_group_desc *gdp;
    struct buffer_head *bh;

    gdp = get_group_desc(sb, block_group, NULL);
    bh = bread(sb->s_dev, gdp->bg_inode_bitmap, sb->s_blocksize);
    if (!bh)
        panic("cannot read inode bitmap");
    sb->u.ext2_sb.s_inode_bitmap_number[bitmap_nr] = block_group;
    sb->u.ext2_sb.s_inode_bitmap[bitmap_nr] = bh;
}

/*
 * load_inode_bitmap loads the inode bitmap for a blocks group
 * 
 * It maintains a cache for the last bitmaps loaded. This cache is
 * managed with a LRU algorithm.
 *
 * Notes:
 * 1/ There is one cache per mounted file system.
 * 2/ If the file system contains less than EXT2_MAX_GROUP_LOADED groups,
 *    this function reads the bitmap without maintaining a LRU cache.
 */
static int load_inode_bitmap(struct super_block *sb,
                                   unsigned int block_group)
{
    int i, j;
    unsigned long inode_bitmap_number;
    struct buffer_head *inode_bitmap;

    if (block_group >= sb->u.ext2_sb.s_groups_count)
        printk("block_group >= groups_count\n");
    if (sb->u.ext2_sb.s_loaded_inode_bitmaps > 0 &&
          sb->u.ext2_sb.s_inode_bitmap_number[0] == block_group)
        return 0;
    if (sb->u.ext2_sb.s_groups_count <= EXT2_MAX_GROUP_LOADED) {
        if (sb->u.ext2_sb.s_inode_bitmap[block_group]) {
            if (sb->u.ext2_sb.s_inode_bitmap_number[block_group] !=
                           block_group)
                panic("block_group != inode_bitmap_number");
            else
                return block_group;
        } else {
            read_inode_bitmap(sb, block_group, block_group);
            return block_group;
        }
    }

    /* LRU */
    for (i = 0; i < sb->u.ext2_sb.s_loaded_inode_bitmaps &&
            sb->u.ext2_sb.s_inode_bitmap_number[i] != block_group; i++)
        ;
    if (i < sb->u.ext2_sb.s_loaded_inode_bitmaps &&
               sb->u.ext2_sb.s_inode_bitmap_number[i] == block_group) {
        inode_bitmap_number = sb->u.ext2_sb.s_inode_bitmap_number[i];
        inode_bitmap = sb->u.ext2_sb.s_inode_bitmap[i];
        for (j = i; j > 0; j--) {
            sb->u.ext2_sb.s_inode_bitmap_number[j] =
                 sb->u.ext2_sb.s_inode_bitmap_number[j - 1];
            sb->u.ext2_sb.s_inode_bitmap[j] =
                 sb->u.ext2_sb.s_inode_bitmap[j - 1];
        }
        sb->u.ext2_sb.s_inode_bitmap_number[0] = inode_bitmap_number;
        sb->u.ext2_sb.s_inode_bitmap[0] = inode_bitmap;
    } else {
        if (sb->u.ext2_sb.s_loaded_inode_bitmaps < EXT2_MAX_GROUP_LOADED)
            sb->u.ext2_sb.s_loaded_inode_bitmaps++;
        else
            brelse(sb->u.ext2_sb.s_inode_bitmap[EXT2_MAX_GROUP_LOADED - 1]);
        for (j = sb->u.ext2_sb.s_loaded_inode_bitmaps - 1; j > 0; j--) {
            sb->u.ext2_sb.s_inode_bitmap_number[j] =
                 sb->u.ext2_sb.s_inode_bitmap_number[j - 1];
            sb->u.ext2_sb.s_inode_bitmap[j] =
                 sb->u.ext2_sb.s_inode_bitmap[j - 1];
        }
        read_inode_bitmap(sb, block_group, 0);
    }
    return 0;
}

static void ext2_check_inode_bitmap(struct super_block *sb)
{
    struct ext2_super_block *es;
    unsigned long desc_count, bitmap_count, x;
    int bitmap_nr;
    struct ext2_group_desc *gdp;
    int i;

    lock_super(sb);
    es = sb->u.ext2_sb.s_es;
    desc_count = 0;
    bitmap_count = 0;
    gdp = NULL;
    for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
        gdp = get_group_desc(sb, i, NULL);
        desc_count += gdp->bg_free_inodes_count;
        bitmap_nr = load_inode_bitmap(sb, i);
        x = ext2_counts_free(sb->u.ext2_sb.s_inode_bitmap[bitmap_nr],
                     EXT2_INODES_PER_GROUP(sb) / 8);
        if (gdp->bg_free_inodes_count != x)
            printk("Wrong free inodes count in group %d\n", i);

        bitmap_count += x;
    }
    if (es->s_free_inodes_count != bitmap_count)
        printk("Wrong free inodes count in super block\n");
    unlock_super(sb);
}

/*
 * Block Group Descriptor Table
 *   Blocks are clustered into block groups in order to reduce fragmentation
 *   and minimise the amount of head seeking when reading a large amount
 *   of consecutive data. Information about each block group is kept in a
 *   descriptor table stored in the block(s) immediately after the 
 *   superblock. Two blocks near the start of each group are reserved for
 *   the block usage bitmap and the inode usage bitmap which show which
 *   blocks and inodes are in use. Since each bitmap is limited to a single
 *   block, this means that the maximum size of a block group is 8 times
 *   the size of a block.
 *
 *   +------+-------+-------------+--------+--------+----+--------+--------+
 *   |      |       |             |        |        |    |        |        |
 *   |      | super | block group | block  | inode  |    | block  | inode  |
 *   | boot | block | descriptor  | bitmap | bitmap | .. | bitmap | bitmap |
 *   |      |       | table       |        |        |    |        |        |
 *   |      |       |             |        |        |    |        |        |
 *   +------+-------+-------------+--------+--------+----+--------+--------+
 *          | <------------ block group 00 ------------> |<-block group01->|
 *
 *   The block(s) following the bitmap in each block group are designated 
 *   as the inode table for that block group and the remainder are the data
 *   blocks. The block allocation algorithm attempts to allocate data
 *   blocks in the same block group as the inode which contains them.
 *
 *   The block group descriptor table is an array of block group descriptor,
 *   used to define parameters of all the block groups. It provides the 
 *   location of the inode bitmap and inode table, block bitmap, number of
 *   free blocks and inodes, and some other useful information.
 *
 *   The block group descriptor table starts on the first block following
 *   the superblock. This would are the third block on a 1KiB block file
 *   system, or the second block for 2KiB and larger block file systems.
 *   Shadow copies of the block group descriptor table are also stored
 *   with every copy of the superblock.
 *
 *   Depending on how many block groups are defined, this table can require
 *   multiple blocks of storage. Always refre to the superblock in case of 
 *   doubt.
 *
 *   The layout of a block group descriptor is as follows:
 *
 *   Table: Block Group Descriptor Structure
 *
 *    Offset (bytes)  Size (bytes)    Description
 *   -------------------------------------------------------------
 *    0               4               bg_block_bitmap
 *    4               4               bg_inode_bitmap
 *    8               4               bg_inode_table
 *    12              2               bg_free_blocks_count
 *    14              2               bg_free_inodes_count
 *    16              2               bg_used_dirs_count
 *    18              2               bg_pad
 *    20              12              bg_reserved
 *   -------------------------------------------------------------
 *
 *  For each block group in the file system, such a group_desc is create.
 *  Each represent a single block group within the file system and the
 *  information within any one of them is pertinent only to the group it
 *  is describing. Every block group descriptor table contains all the 
 *  information about all the block groups.
 */
static __unused int ext2_group_descriptor(struct super_block *sb)
{
    struct ext2_super_block *es;
    struct buffer_head *bh = NULL;
    struct buffer_head *itbh = NULL;
    struct ext2_group_desc *gdp; 
    int bh_count;
    int i, j;
    int ret = 0;

    /* Verify ext2-fs super block */
    es = sb->u.ext2_sb.s_es;
    if (!es) {
        if (!(bh = bread(sb->s_dev, SUPER_BLOCK, BLOCK_SIZE))) {
            printk(KERN_ERR "EXT2-FS: unable to read superblock\n");
            ret = -EINVAL;
            goto out;
        }
        es = (struct ext2_super_block *)bh->b_data;
        sb->u.ext2_sb.s_es = es;
    }

    /* Block size */
    sb->s_blocksize = EXT2_MIN_BLOCK_SIZE << es->s_log_block_size;
    /*
     * s_blocks_per_group
     *   32bit value indicating the total number of blocks per group.
     *   This value in combination with s_first_data_block can be
     *   used to determine the block groups boundaries.
     */
    sb->u.ext2_sb.s_blocks_per_group = es->s_blocks_per_group;

    /*
     * s_groups_count
     *   32bit value indicating the total number of groups on ext2.
     *   The value 's_blocks_count' indicating the total number of blocks,
     *   and the value 'EXT2_BLOCKS_PER_GROUP' indicating the total number
     *   of block per group, these value in combination with 
     *   's_first_data_block' can be used to determine the total number
     *   of groups on ext2.
     *   
     */
    sb->u.ext2_sb.s_groups_count = (es->s_blocks_count -
                        es->s_first_data_block +
                        EXT2_BLOCKS_PER_GROUP(sb) - 1) /
                        EXT2_BLOCKS_PER_GROUP(sb);
    /*
     * s_desc_per_block
     *   32bit value indicating the total number of group descriptor
     *   per block.
     */
    sb->u.ext2_sb.s_desc_per_block = sb->s_blocksize /
                       sizeof(struct ext2_group_desc);

    /*
     * s_inodes_per_group
     *   32bit value indicating the total number of inodes per group.
     *   This is also used to determine the size of the inode bitmap of
     *   each block group. Note that you cannot have more than (block size
     *   in bytes * 8) inodes per group as the inode bitmap must fit within
     *   a signal block. This value must be a perfect multiple of the 
     *   number of inodes that can fit in a block ((1024 < 
     *   s_log_block_size) / s_inode_size).
     */
    sb->u.ext2_sb.s_inodes_per_group = es->s_inodes_per_group;

    /*
     * s_group_desc
     *   The pointer array points the buffer containing the group
     *   descriptor. On ext2-fs layout, the group descriptor is behind
     *   superblock, as figure:
     *
     *   +------+------------+---------------+---------------+--------+
     *   |      |            |               |               |        |
     *   | Boot | superblock | group_desc[0] | group_desc[1] | ...    |
     *   |      |            |               |               |        |
     *   +------+------------+---------------+---------------+--------+
     *
     *   This value 's_groups_per_block' in combination with 's_groups_count' 
     *   can be used to determine the total number of block for reading.
     * 
     *     count = (s->groups_count + s_groups_per_block - 1) /
     *                          s_groups_per_block
     *
     */

    for (i = 0; i < EXT2_MAX_GROUP_DESC; i++)
        sb->u.ext2_sb.s_group_desc[i] = NULL;
    bh_count = (sb->u.ext2_sb.s_groups_count +
                     EXT2_DESC_PER_BLOCK(sb) - 1) /
                     EXT2_DESC_PER_BLOCK(sb);
    for (i = 0; i < bh_count; i++) {
        sb->u.ext2_sb.s_group_desc[i] =
                bread(sb->s_dev, SUPER_BLOCK + i + 1, sb->s_blocksize);
        if (!sb->u.ext2_sb.s_group_desc[i]) {
            sb->s_dev = 0;
            for (j = 0; j < i; j++)
                brelse(sb->u.ext2_sb.s_group_desc[j]);
            brelse(bh);
            printk("EXT2-fs: unable to read group descriptor\n");
            ret = -EINVAL;
            goto out1;
        }
    }

    /* Parse ext2-fs group descriptor 
     *   The first version of ext2 (revision 0) stores a copy at the 
     *   start of every block group, along with backups of the group 
     *   descriptor block(s). Because this can consume a considerable 
     *   amount of space for filesystems, later revisions can optionally
     *   reduce the number of backup copies by only putting backups in 
     *   special groups (this is the sparse superblock feature). The 
     *   groups chosen are 0, 1 and powers of 3, 5 and 7.
     *
     *   Contain backup of superblock and group descriptor table
     *   +-------+------------+--------+--------+-------+------------+
     *   |       |            |        |        |       |            |
     *   | super | group      | block  | inode  | inode |            |
     *   | block | descriptor | bitmap | bitmap | table | block data |
     *   |       | table      |        |        |       |            |
     *   +-------+------------+--------+--------+-------+------------+
     *
     *   No contain back of superblock and group descriptor table
     *   +--------------+--------------+-------------+---------------+
     *   |              |              |             |               |
     *   | block bitmap | inode bitmap | inode table | block data    |
     *   |              |              |             |               |
     *   +--------------+--------------+-------------+---------------+
     */
    gdp = (struct ext2_group_desc *)
                sb->u.ext2_sb.s_group_desc[0]->b_data;

    /*
     * bg_block_bitmap
     *   32bit block id of the first block of the 'block bitmap' for the 
     *   group represented.
     *   
     *   The actual block bitmap is located within its own allocated
     *   blocks starting at the block ID specified by this value. 
     *
     *   Since each bitmap is limited to a single block, this means that 
     *   the maximum size of a block group is 8 times the size of a block.
     */
    for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
        sb->u.ext2_sb.s_block_bitmap_number[i] = i;
        sb->u.ext2_sb.s_block_bitmap[i] = 
            bread(sb->s_dev, gdp[i].bg_block_bitmap, sb->s_blocksize);
        if (!sb->u.ext2_sb.s_block_bitmap[i]) {
            for (j = 0; j < i; j++) {
                sb->u.ext2_sb.s_block_bitmap_number[j] = 0;
                brelse(sb->u.ext2_sb.s_block_bitmap[j]);
            }
            printk(KERN_ERR "Ext2-fs: unable to read block bitmap\n");
            ret = -EINVAL;
            goto out2;
        }
    }

    /*
     * bg_inode_bitmap
     *   32bit block id of the first block of the "inode bitmap" for
     *   the group represented.
     */
    for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
        sb->u.ext2_sb.s_inode_bitmap_number[i] = i;
        sb->u.ext2_sb.s_inode_bitmap[i] =
            bread(sb->s_dev, gdp[i].bg_inode_bitmap, sb->s_blocksize);
        if (!sb->u.ext2_sb.s_inode_bitmap[i]) {
             for (j = 0; j < i; j++) {
                 sb->u.ext2_sb.s_inode_bitmap_number[j] = 0;
                 brelse(sb->u.ext2_sb.s_inode_bitmap[j]);
             }
             printk(KERN_ERR "Ext2-fs: unable to read inode bitmap\n");
             ret = -EINVAL;
             goto out3;
        }
    }
    
    /*
     * bg_inode_table
     *   32bit block id of the first block of the "inode table" for the
     *   group represented.
     */
    if (!(itbh = bread(sb->s_dev, gdp->bg_inode_table, sb->s_blocksize))) {
        printk(KERN_ERR "Ext2-fs: unable to read inode table\n");
        ret = -EINVAL;
        goto out4;
    }    

    /*
     * bg_free_blocks_count
     *   16bit value indicating the total number of free blocks for the 
     *   represented group.
     */
    if (gdp->bg_free_blocks_count < 0)
        printk("Ext2-fs: block group no free block.\n");

    /*
     * bg_free_inodes_count
     *   16bit value indicating the total number of free inodes for
     *   the represented group.
     */
    if (gdp->bg_free_inodes_count < 0)
        printk("Ext2-fs: block group no free inodes.\n");

    /*
     * bg_used_dirs_count
     *   16bit value indicating the number of inodes allocated to
     *   directories for the represented group.
     */
    if (gdp->bg_used_dirs_count < 0)
        printk("Ext2-fs: no inodes allocated to directories\n");

    /* Check ext2 group descriptor */
    ext2_check_descriptors(sb);
    /* Check ext2 block bitmap */
    ext2_check_block_bitmap(sb);
    /* Check ext2 block */
    ext2_check_inode_bitmap(sb);

    if (itbh)
        brelse(itbh);

out4:
    for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
        sb->u.ext2_sb.s_inode_bitmap_number[i] = 0;
        brelse(sb->u.ext2_sb.s_inode_bitmap[i]);
    }

out3:
    for (i = 0; i < sb->u.ext2_sb.s_groups_count; i++) {
        sb->u.ext2_sb.s_block_bitmap_number[i] = 0;
        brelse(sb->u.ext2_sb.s_block_bitmap[i]);
    }
  
out2:
    /* Release buffer of group_desc */
    for (i = 0; i < bh_count; i++)
        brelse(sb->u.ext2_sb.s_group_desc[i]);
out1:
    if (bh)
        brelse(bh);
    
out:
    return ret;
}

/*
 * Ext2-fs inode
 *
 * The inode (index inode) is a fundamental concept in ext2 filesystem.
 * Each object in the filesystem is represented by an inode. The inode
 * structure contains pointer to the filesystem blocks which contain the
 * data held in the object and all of the metadata about an object except
 * its name. The metadata about an object includes the permissions, owner,
 * group, flags, size, number of blocks used, access time, change time,
 * modification time, deletion time, number of links, fragments, version (
 * for NFS) and extened attributes (EAs) and Access Control List (ACLs).
 *
 * There are some reserved field which are currently unused in the inode
 * structure and several which are overloaded. One field is reserved for the
 * directory ACL if the inode is a directory and alternately for the top
 * 32 bits of the file size if the inode is a regular file (allowing file
 * size larger than 2GB). The translator used to interpret this object.
 * Most of the remaining reserved fields have been used up for both Linux
 * and the HURD for larger owner and group fields, The HURD also has a 
 * larger mode field so it uses another of the remaining fields to store
 * the extra bits.
 *
 * There are pointers to the first 12 blocks which contain the file's data
 * in the inode. There is a pointer to an indirect block (which contains 
 * pointers to the next set of blocks), a pointer to a doubly-indirect
 * block (which contains pointers to indirect blocks) and a pointer to
 * a trebly-indirect block (which contains pointers to doubly-indirect 
 * blocks).
 *
 *                      Direct blocks
 *                       +-------+
 *                       |       |
 *      inode            |       |
 *    +-------+     o--->+-------+         Indirect blocks
 *    | infos |     |                        +-------+
 *    +-------+     |                        |       |
 *    |      -|-----o    +-------+           |       |
 *    +-------+          |      -|---------->+-------+
 *    |       |          +-------+
 *    +-------+          |       |
 *    |      -|--------->+-------+                   Double indirect blocks
 *    +-------+                           +-------+      +-------+
 *    |       |          +-------+        |       |      |       |
 *    +-------+          |       |        +-------+      |       |
 *    |      -|--------->+-------+        |      -|----->+-------+
 *    +-------+          |      -|------->+-------+
 *                       +-------+
 *
 *
 * Some file system specific behaviour flags are also stored and allow for
 * specific filesystem behaviour on a per-file basic. The are flags for
 * secure delection, undeletable, compression, synchronous updates,
 * immutability, append-only, dumpable, no-atime, indexed directories, and
 * data-journaling.
 *
 * Many of the filesystem specific behaviour flags, like journaling, have
 * been implemented in newer filesystem like EXT3 and EXT4, while some 
 * other are still under development.
 *
 * All the inodes are stored in inode tables, with one inode table per 
 * block group.
 *
 * Inode Table
 *   
 *   The inode table is used to keep track of every directory, regular file,
 *   symbolic link, or special file. Their location, size, type and access
 *   rights are all stored in inodes. There is no filename stored in the
 *   inode itself, names are contained in directory file only.
 *
 *   Each inode contain the information about a single physical file on
 *   the system. A file can be a directory, a socket, a buffer, character
 *   or block device, symbolic link or a regular file. So an inode can be
 *   seen as a block of information related to an entity, describing its
 *   location on disk, its size and its owner. Am inode looks like this:
 *
 *   Table: Inode structure
 *
 *    Offset (bytes)  Size (bytes)  Description
 *   --------------------------------------------------------------------
 *    0               2             i_mode
 *    2               2             i_uid
 *    4               4             i_size
 *    8               4             i_atime
 *    12              4             i_ctime
 *    16              4             i_mtime
 *    20              4             i_dtime
 *    24              2             i_gid
 *    26              2             i_links_count
 *    28              4             i_blocks
 *    32              4             i_flags
 *    36              4             i_osd1
 *    40              15*4          i_block
 *    100             4             i_generation
 *    104             4             i_file_acl
 *    108             4             i_dir_acl
 *    112             4             i_faddr
 *    116             12            i_osd2
 *   --------------------------------------------------------------------
 *
 *   The first few entries of the inode tables are reserved. In revision 0
 *   there are 11 entries reserved while in revision 1 (EXT2_DYNAMIC_REV)
 *   and later the number of reserved inodes entries is specified in the
 *   s_first_ino of the superblock structure. Here's a listing of the
 *   known reserved inode entris:
 *
 *   Table: Defined Reserved Inodes
 *
 *    Constant Name            Value    Description
 *   --------------------------------------------------------------------
 *    EXT2_BAD_INO             1        bad blocks inode
 *    EXT2_ROOT_INO            2        root directory inode
 *    EXT2_ACL_IDX_INO         3        ACL index inode (deprecated?)
 *    EXT2_ACL_DATA_INO        4        ACL data inode (deprecated?)
 *    EXT2_BOOT_LOADER_INO     5        boot loader inode
 *    EXT2_UNDEL_DIR_INO       6        undelete directory inode
 *   --------------------------------------------------------------------
 *
 */
static __unused int ext2_inode(struct super_block *sb, struct inode *inode)
{
    struct ext2_inode *raw_inode;
    struct buffer_head *bh;
    unsigned long block_group;
    unsigned long group_desc;
    unsigned long desc;
    unsigned long block;
    unsigned long offset;
    unsigned long addr_per_block;
    struct ext2_group_desc *gdp;
    struct buffer_head *mbh;
    char *buffer;

    /*
     * The filesytem is divided into block group, on revision 0, special
     * block group contain copies of superblock and block group descriptor 
     * table, block bitmap, inode bitmap, inode table and data block. But 
     * some special block group only contain block bitmap, inode bitmap,
     * inode table and data block.
     * 
     * EXT2-fs layout:
     *
     * +---------------+---------------+---------------+-----------+
     * |               |               |               |           |
     * | Block group 0 | block group 1 | block group 2 | .......   | 
     * |               |               |               |           |
     * +---------------+---------------+---------------+-----------+
     *
     * Block group 0 layout:
     *
     * +------------+------------------+--------+--------+-------+-+
     * |            |                  |        |        |       | |
     * | superblock | block group      | block  | inode  | inode | |
     * |            | descriptor table | bitmap | bitmap | table | |
     * |            |                  |        |        |       | |
     * +------------+------------------+--------+--------+-------+-+
     *  
     * Block group 2 layout:
     *
     * +--------------+--------------+-------------+---------------+
     * |              |              |             |               |
     * | block bitmap | inode bitmap | inode table | data block    |
     * |              |              |             |               |
     * +--------------+--------------+-------------+---------------+
     *
     *
     * Inodes are all numerically ordered. The "inode number" is an index
     * in the inode table to an inode structure. The size of the inode
     * table is fixed at format time. It's built to hold a maximum number
     * of entries. Due to the large amount of entries created, the table
     * is quite big and thus, it is split equally amount all the block
     * groups.
     *
     * The s_inodes_per_group field in the superblock structure tells us
     * how many inodes are defined per gorup. Knowing that inode 1 is the
     * first inode defined in the inode table. one can use the following
     * formulaes:
     *
     *   block group = (inode - 1) / s_inodes_per_group
     *   or
     *   block group = (inode - 1) / EXT2_INODES_PER_GROUP(sb)
     *
     * Once the block is identified, the local inode index for the local
     * inode table can be identified using:
     *
     *   local inode index = (inode - 1) % s_inodes_per_group
     *
     * Here are a couple of sample values that could be used to test yuour
     * implementation.
     *
     * Table: Sample Inode Computations
     *
     *  s_inodes_per_group = 1712
     *
     *  Inode Number       Block Group Number       Local Inode Index
     * ------------------------------------------------------------------
     *  1                  0                        0
     *  963                0                        962
     *  1712               0                        1711
     *  1713               1                        0
     *  3424               1                        1711
     *  3425               2                        0
     * ------------------------------------------------------------------
     *  
     * As many of you are most likely already familiar with, an index of 0
     * means the first entry. The reason behind using 0 rather than 1 is 
     * that it can more easily be multiplied by the structure size to find
     * the final byte offset of its location in memory or on disk.
     */

    block_group = (inode->i_ino - 1) / EXT2_INODES_PER_GROUP(inode->i_sb);
    if (block_group >= inode->i_sb->u.ext2_sb.s_groups_count)
        panic("EXT2-fs: group > groups count");
    
    group_desc = block_group / EXT2_DESC_PER_BLOCK(inode->i_sb);
    desc = block_group % EXT2_DESC_PER_BLOCK(inode->i_sb);
    /* buffer of group descriptor */
    bh = inode->i_sb->u.ext2_sb.s_group_desc[group_desc];
    if (!bh)
        printk("Descriptor not loaded\n");
    /*
     * 
     * +-------------+-------------+-------------+----+-------------+
     * |             |             |             |    |             |
     * | block group | block group | block group | .. | block group |
     * | descriptor  | descriptor  | descriptor  |    |             |
     * |             |             |             |    |             |
     * +-------------+-------------+-------------+----+-------------+
     * A
     * |
     * |
     * |
     * |
     * o---gpd       
     *
     * bg_inode_table
     *   32bit block id of the first block of the 'inode table' for the
     *   represented group.
     *
     * EXT2_INODES_PER_GROUP
     *   Macro indicating the number of inode per group.
     *
     * EXT2_INODES_PER_BLOCK
     *   Macro indicating the number of inode per group.
     *
     * block group descriptor
     * +-----------------+
     * |                 |   
     * |                 |   
     * |                 |   
     * +-----------------+            +--------------------------------+
     * | bg_inode_table -|----------->| first block id of inode table  | 
     * +-----------------+            +--------------------------------+
     * |                 |            | second block id of inode table |
     * |                 |            +--------------------------------+
     * +-----------------+            | third block id of inode table  |
     *                                +--------------------------------+
     *                                |      .......                   |
     *                                +--------------------------------+
     *                                | xth block id of inode table    |
     *                                +--------------------------------+
     *
     *
     */
    gdp = (struct ext2_group_desc *)bh->b_data;
    block = gdp[desc].bg_inode_table +
                (((inode->i_ino - 1) % EXT2_INODES_PER_GROUP(inode->i_sb)) /
                EXT2_INODES_PER_BLOCK(inode->i_sb));

    /* read block from EXT2-fs */
    if (!(bh = bread(inode->i_dev, block, inode->i_sb->s_blocksize)))
        printk(KERN_ERR "unable to read inode block\n");

    /* Read ext2_inode from EXT2-fs */
    raw_inode = ((struct ext2_inode *) bh->b_data) +
               ((inode->i_ino - 1) % EXT2_INODES_PER_BLOCK(inode->i_sb));

    /*
     * i_mode
     *   16bit value used to indicate the format of the described and the
     *   access rights. Here are the possible value, which can be
     *   combined in various ways.
     *
     *   Table: Defined i_mode values
     *
     *    Constant                 Value       Description
     *   ------------------------------------------------------------
     *    -- file format --
     *   ------------------------------------------------------------
     *   EXT2_S_IFSOCK             0xC000      socket
     *   EXT2_S_IFLNK              0xA000      symbolic link
     *   EXT2_S_IFREG              0x8000      regular file
     *   EXT2_S_IFBLK              0x6000      block device
     *   EXT2_S_IFDIR              0x4000      directory
     *   EXT2_S_IFCHR              0x2000      character device
     *   ------------------------------------------------------------
     *    -- process execution user/group override --
     *   ------------------------------------------------------------
     *   EXT2_S_ISUID              0x0800      Set process User ID
     *   EXT2_S_ISGID              0x0400      Set process Group ID
     *   EXT2_S_ISVTX              0x0200      sticky bit
     *   ------------------------------------------------------------
     *    -- access rights --
     *   ------------------------------------------------------------
     *   EXT2_S_IRUSR              0x0100      user read
     *   EXT2_S_IWUSR              0x0080      user write
     *   EXT2_S_IXUSR              0x0040      user execute
     *   EXT2_S_IRGRP              0x0020      group read
     *   EXT2_S_IWGRP              0x0010      group write
     *   EXT2_S_IXGRP              0x0008      group execute
     *   EXT2_S_IROTH              0x0004      others read
     *   EXT2_S_IWOTH              0x0002      others write
     *   EXT2_S_IXOTH              0x0001      others execute
     *   ------------------------------------------------------------
     */
    if (inode->i_mode != raw_inode->i_mode)
        panic("Error on i_mode");

    /*
     * i_uid
     *   16bit user id associated with the file.
     *
     * i_gid
     *   16bit value of the POSIX group having access to this file.
     */
    if (inode->i_uid != raw_inode->i_uid)
        panic("Error on i_uid");
 
    /*
     * i_size
     *   In revision 0, (signed) 32bit value indicating the size of the
     *   file in bytes. In revision 1 and later revisions, and only for
     *   regular file, this represents the lower 32-bit of the file size.
     *   The upper 32-bit is located in the i_dir_acl.
     */
    if (inode->i_size != raw_inode->i_size)
        panic("Error on i_size");
    
    /*
     * Time field
     *
     *   i_atime
     *     32bit value representing the number of seconds since january 1st
     *     1970 of the last time this inode was accessed.
     *
     *   i_ctime
     *     32bit value representing the number of seconds since january 1st
     *     1970, of when the inode was created.
     *
     *   i_mtime
     *     32bit value representing the number of seconds since january 1st
     *     1970, of the last time this inode was modify.
     *
     *   i_dtime
     *     32bit value representing the number of seconds since january 1st
     *     1970, of when the inode was deleted.
     */
    if (inode->i_ctime != raw_inode->i_ctime)
        panic("Error on create time!\n");

    /*
     * i_links_count
     *   16bit value indicating how many times this particular inode is
     *   linked (referred to). Most files will have a link count of 1.
     *   Files with hard links pointing to them will have an additional
     *   count for each hard link.
     *
     *   Symbolic links do not affect the link count of an inode. When
     *   the link count reaches 0 the inode and all its associated blocks
     *   are freed.
     */
    if (inode->i_nlink != raw_inode->i_links_count)
        panic("Error on symbolic link.");

    /*
     * i_blocks
     *   32bit value representing the total number of 512-bytes blocks
     *   reserved to contain the data of this inode, regardless if these
     *   blocks are used or not. The block number of these reserved blocks
     *   are contained in the i_block_array.
     *
     *   Since this value represents 512-byte block and not file system
     *   blocks, this value should not directory used as an index to the 
     *   i_block_array. Rather, the maximum index of the i_block array
     *   should be computed from i_blocks / ((1024 << s_log_block_size) / 
     *   512), or once simplified, i_blocks / (2 << s_log_block_size). 
     *
     */
    if (inode->i_blocks != raw_inode->i_blocks)
        panic("Error on block number");
    
    /*
     * i_flags
     *   32bit value indicating how the ext2 implementation should behave
     *   when accessing the data for this inode.
     *
     *   Table: Defined i_flags value
     *   
     *    Constant Name          Value        Descriptor   
     *   ----------------------------------------------------------------
     *   EXT2_SECRM_FL           0x00000001   secure delection
     *   EXT2_UNRM_FL            0x00000002   record for undelete
     *   EXT2_COMPR_FL           0x00000004   compressed file
     *   EXT2_SYNC_FL            0x00000008   synchronous updates
     *   EXT2_IMMUTABLE_FL       0x00000010   immutable file
     *   EXT2_APPEND_FL          0x00000020   append only
     *   EXT2_NODUMP_FL          0x00000040   do not dump/delete file
     *   EXT2_NOATIME_FL         0x00000080   do not update .i_atime
     *   ----------------------------------------------------------------
     *    -- Reserved for compression usage --
     *   ----------------------------------------------------------------
     *   EXT2_DIRTY_FL           0x00000100   Dirty (modified)
     *   EXT2_COMPRBLK_FL        0x00000200   compressed blocks
     *   EXT2_NOCOMPR_FL         0x00000400   accss raw compressed data
     *   EXT2_ECOMPR_FL          0x00000800   compression error
     *   ----------------------------------------------------------------
     *    -- End of compression flags --
     *   ----------------------------------------------------------------
     *   EXT2_BTREE_FL           0x00001000   b-tree format directory
     *   EXT2_INDEX_FL           0x00001000   hash indexed directory
     *   EXT2_IMAGIC_FL          0x00002000   AFS directory
     *   EXT2_JOURNAL_DATA_FL    0x00004000   journal file data
     *   EXT2_RESERVED_FL        0x00008000   reserved for ext2 library
     *   ----------------------------------------------------------------
     */
    if (inode->i_flags != raw_inode->i_flags)
        panic("Error on inode flags");

    /*
     * i_block
     *   15 * 32bit block numbers pointing to block containing the data
     *   for this inode. The first 12 blocks are direct block. The 13th
     *   entry in this array is the block number of the first indirect
     *   block. Which is a block containing an array of block ID containing
     *   the data. Therefore, the 13th block of the file will be the block
     *   ID contained in the indirect block. With a 1KiB block size, block 
     *   13 to 268 of the file data are contained in this indirect block.
     *
     *   The 14th entry in this array is the block number of the first
     *   double-indirect block, which is a block containing an array of
     *   indirect block IDs, with each of those indirect blocks containing
     *   an array of blocks containing the data. In a 1KiB block size, 
     *   there would be 256 indirect blocks per double-indirect block,
     *   with 256 direct blocks per indirect block for a total of 65536
     *   blocks per double-indirect block.
     *
     *   The 15th entry in this array is the block number of the triply-
     *   indirect block, which is a block containing an array of double-
     *   indirect block IDs, with each of those doubly-indirect block
     *   containing an array of indirect block, and each of those indirect
     *   block containing an array of direct block. In a 1KiB file system,
     *   this would be a total of 16777216 blocks per triply-indirect
     *   block.
     *
     *   A value of 0 in this array effectively terminates it with no
     *   further block being defined. All the remaining entries of the
     *   array should still be set to 0.
     *
     *
     *   ext2_inode
     *
     *   +--------+            direct block           direct block
     *   |        |             +--------+              +-------+
     *   |        |             |        |              |       |
     *   |        |             |        |              |       |
     *   |        |        o--->+--------+         o--->+-------+
     *   |        |        |                       |
     *   |        |        |                       |
     *   |        |        |       indirct block   |   direct block
     *   |        |        |         +--------+    |    +--------+
     *   |        |        |         |        |    |    |        |
     *   |        |        |         +--------+    |    |        |
     *   |        |        |         |       -|----o    +--------+<----o
     *   |        |        |         +--------+                        |
     *   |        |        |         |        |                        |
     *   |        |        |         +--------+       indirect block   |
     *   |        |        |         |        |         +--------+     |
     *   |        |        |   o---->+--------+         |        |     |
     *   |        |        |   |                        +--------+     |
     *   |        |        |   |     double-indirect    |        |     |
     *   |        |        |   |          block         +--------+     |
     *   |        |        |   |        +-------+       |        |     |
     *   |        |        |   |        |       |       +--------+     |
     *   |        |        |   |        +-------+       |       -|-----o
     *   +--------+        |   |        |      -|------>+--------+
     *   |i_block-|--------o   |        +-------+
     *   +--------+            |        |       |
     *   |i_block-|------------o        +-------+
     *   +--------+                     |       |
     *   |i_block-|-------------------->+-------+
     *   +--------+
     *   |i_block-|-------o
     *   +--------+       |                               
     *                    |                                
     *   triply-indirect  |                                
     *     block          |                               direct block
     *   +--------+<------o                                +--------+
     *   |        |                      indirect block    |        |
     *   +--------+                        +--------+      |        |
     *   |        |    double-indirect     |       -|----->+--------+
     *   +--------+       block            +--------+
     *   |       -|----->+--------+        |        |
     *   +--------+      |        |        +--------+
     *                   +--------+        |        |
     *                   |       -|------->+--------+
     *                   +--------+
     *                   |        |
     *                   +--------+
     *
     */          

    offset = 0;
    block = offset / inode->i_sb->s_blocksize;
    addr_per_block = EXT2_ADDR_PER_BLOCK(inode->i_sb);

    /* 
     * Direct block 
     *
     * ext2_inode
     * +--------------+
     * |              |                    direct block
     * |              |                +-----------------+
     * |              |                |                 |
     * |              |                |                 |
     * +--------------+                |                 |
     * | i_data[0]   -|--------------->+-----------------+
     * +--------------+
     * | ....         |
     * +--------------+
     * | i_data[11]   |
     * +--------------+
     * |              |
     * +--------------+
     * |              |
     * +--------------+
     * |              |
     * +--------------+
     *
     *
     * Here are a couple of sample values that could be used to test yuour
     * implementation.
     *
     * Table: Sample direct block 
     *
     *  addr_per_block = 1024 / 4
     *
     *  Range: 0 to 11
     *
     *  index   triply-indirect    double-indirect    indirect   block nr
     * ------------------------------------------------------------------
     *  0       x                  x                  x          254
     *  1       x                  x                  x          258
     *  2       x                  x                  x          345
     *  3       x                  x                  x          287
     *  4       x                  x                  x          29
     *  5       x                  x                  x          437
     *  6       x                  x                  x          8
     *  7       x                  x                  x          167
     *  8       x                  x                  x          34
     *  9       x                  x                  x          90
     *  10      x                  x                  x          91
     *  11      x                  x                  x          92
     * ------------------------------------------------------------------
     *
     */
    if (block < EXT2_NDIR_BLOCKS) {
        block = inode->u.ext2_i.i_data[block];
        goto load_block;
    }
    /* 
     * Indirect block 
     *
     * ext2_inode
     * +--------------+
     * |              |                
     * |              |                
     * |              |                
     * |              |                             direct block
     * +--------------+    indirect block       +-----------------+
     * | i_data[0]    |     +----------+        |                 |
     * +--------------+     |          |        |                 |
     * | ....         |     +----------+        |                 |
     * +--------------+     |         -|------->+-----------------+
     * | i_data[11]   |     +----------+
     * +--------------+     |          |
     * | i_data[12]  -|---->+----------+
     * +--------------+
     * |              |
     * +--------------+
     * |              |
     * +--------------+
     *
     *
     *
     * Here are a couple of sample values that could be used to test yuour
     * implementation.
     *
     * Table: Sample indirect block 
     *
     *  addr_per_block = 1024 / 4
     *  
     *  Range: 12 to 267
     *
     *  index   triply-indirect    double-indirect    indirect   block nr
     * ------------------------------------------------------------------
     *  12      x                  x                  0          269
     *  13      x                  x                  1          332
     *  19      x                  x                  7          190
     *  100     x                  x                  88         27
     *  156     x                  x                  144        97
     *  199     x                  x                  187        637
     *  200     x                  x                  189        638
     *  250     x                  x                  238        107
     *  254     x                  x                  242        734
     *  255     x                  x                  243        735
     *  267     x                  x                  255        789
     * ------------------------------------------------------------------
     *
     */
    block -= EXT2_NDIR_BLOCKS; /* first block is 12 */
    if (block < addr_per_block) {
        struct buffer_head *ibh;
        unsigned long *tmp;

        if (!(ibh = bread(inode->i_dev, 
                inode->u.ext2_i.i_data[EXT2_IND_BLOCK], 
                                  inode->i_sb->s_blocksize)))
            panic("Unable to read indirect-block");

        tmp = (unsigned long *) ibh->b_data + block;
        block = *tmp;
        brelse(ibh);
        goto load_block;
    }
    /* 
     * Double-indirect block 
     * 
     * ext2_inode
     * +--------------+
     * |              |                                        direct block
     * |              |                                         +---------+
     * |              |                                         |         |
     * |              |                        indirect block   |         |
     * +--------------+                         +---------+     |         |
     * | i_data[0]    |    double-indirect      |        -|---->+---------+
     * +--------------+          block          +---------+
     * | ....         |      +----------+       |         |
     * +--------------+      |          |       +---------+
     * | i_data[11]   |      +----------+       |         |
     * +--------------+      |         -|------>+---------+
     * | i_data[12]   |      +----------+
     * +--------------+      |          |
     * | i_data[13]  -|----->+----------+
     * +--------------+
     * |              |
     * +--------------+
     *
     *
     *
     * Here are a couple of sample values that could be used to test yuour
     * implementation.
     *
     * Table: Sample Double-indirect block 
     *
     *  addr_per_block = 1024 / 4
     *  
     *  Range: 268 to 65803
     *
     *  index   triply-indirect    double-indirect    indirect   block nr
     * ------------------------------------------------------------------
     *  268     x                  0                  0          522
     *  523     x                  0                  255        589
     *  524     x                  1                  0          356
     *  779     x                  1                  255        527
     *  1876    x                  6                  72         497
     *  34567   x                  133                251        797
     *  43321   x                  168                45         109
     *  65279   x                  253                243        134
     *  65803   x                  255                255        35
     * ------------------------------------------------------------------
     *
     */

    block -= addr_per_block; /* first block is (12 + 256) */
    if (block < addr_per_block * addr_per_block) {
        struct buffer_head *dbh;
        unsigned long *tmp;

        /* Double-indirect block */
        if (!(dbh = bread(inode->i_dev,
              inode->u.ext2_i.i_data[EXT2_DIND_BLOCK],
                     inode->i_sb->s_blocksize)))
            panic("Unable to load double-block one");
        
        tmp = (unsigned long *)dbh->b_data + (block / addr_per_block);
        brelse(dbh);

        /* Indirect block */
        if (!(dbh = bread(inode->i_dev, *tmp, inode->i_sb->s_blocksize)))
            panic("Unable to load double-block two");
        tmp = (unsigned long *)dbh->b_data + (block % addr_per_block);
        block = *tmp;
        brelse(dbh);
        goto load_block;
    }
    /* 
     * Triply-indirect block 
     *      
     *      
     *                                                          direct block
     *                                               indirect     +------+
     * ext2_inode                                      block      |      |
     * +--------------+                               +-----+     |      |
     * |              |                               |    -|---->+------+
     * |              |                               +-----+
     * |              |              double-indirect  |     |
     * |              |                    block      +-----+
     * +--------------+                  +------+     |     |
     * | i_data[0]    |                  |     -|---->+-----+
     * +--------------+                  +------+
     * | ....         | triply-indirect  |      |
     * +--------------+     block        +------+
     * | i_data[11]   |   +-------+      |      |
     * +--------------+   |      -|----->+------+
     * | i_data[12]   |   +-------+
     * +--------------+   |       |
     * | i_data[13]   |   +-------+
     * +--------------+   |       |
     * | i_data[14]  -|-->+-------+
     * +--------------+
     *
     *
     * Here are a couple of sample values that could be used to test yuour
     * implementation.
     *
     * Table: Sample Triply-indirect block 
     *
     *  addr_per_block = 1024 / 4
     *  
     *  Range: 65804 to 16843019
     *
     *  index     triply-indirect   double-indirect   indirect   block nr
     * ------------------------------------------------------------------
     *  65804     0                 0                 0          1220
     *  131339    0                 255               255        1589
     *  453456    5                 234               68         1956
     *  898765    12                181               193        4676
     *  999999    14                65                51         5497
     *  10843000  164               114               108        11797
     *  15643600  237               178               196        21091
     *  16842764  255               255               0          87301
     *  16843019  255               255               255        87654
     * ------------------------------------------------------------------
     *
     */

    if ((block -= addr_per_block * addr_per_block) > 0) {
        struct buffer_head *tbh;
        unsigned long *tmp;

        /* load triply-indirect block */
        if (!(tbh = bread(inode->i_dev, EXT2_TIND_BLOCK, 
                                 inode->i_sb->s_blocksize)))
            panic("Unable to load triply-block one");
        
        tmp = (unsigned long *)tbh->b_data + (block / 
                                (addr_per_block * addr_per_block));
        brelse(tbh);
        
        /* load double-indirect block */
        if (!(tbh = bread(inode->i_dev, *tmp, inode->i_sb->s_blocksize)))
            panic("Unable to load triply-block two");

        tmp = (unsigned long *)tbh->b_data + 
                       ((block / addr_per_block) % addr_per_block);
        brelse(tbh);

        /* load indirect block */
        if (!(tbh = bread(inode->i_dev, *tmp, inode->i_sb->s_blocksize)))
            panic("Unable to load triply-block three");

        tmp = (unsigned long *)tbh->b_data + (block % addr_per_block);

        block = *tmp;
        brelse(tbh);
        goto load_block;
    }

load_block:
    if (!(mbh = bread(inode->i_dev, block, inode->i_sb->s_blocksize)))
        panic("Unable to load buffer from disk");

    /* The buffer point the data block for inode */
    buffer = (char *)mbh->b_data;
    printk("Buffer:\n%s\n", buffer);

    /*
     * i_file_acl
     *   32bit value indicating the block number containing the extened
     *   attributes. In revision 0 this value is always 0.
     *
     * i_dir_acl
     *   In revision 0 this 32bit value is always 0. In revision 1, for
     *   regular files this 32bit value contains the high 32 bits of
     *   the 64bit file size.
     */
    if (inode->u.ext2_i.i_dir_acl != raw_inode->i_dir_acl)
        panic("Error i_dir_acl");

    if (mbh)
        brelse(mbh);

    if (bh)
        brelse(bh);

    return 0;
}

/*
 * EXT2-FS directories
 *   A directory is a filesytem object and has an inode just like a file.
 *   It is a specially formatted file containing records which associate
 *   each name with an inode number. Later revisions of the filesystem
 *   also encode the type of the object (file, directory, symblic link,
 *   device, fifo, socket) to avoid the need to check the inode itself
 *   for this information.
 *
 *   The inode allocation code should try to assign inodes which are in
 *   the same block group as the directory in which they are first created.
 *
 *   The original EXT2 revision used singly-linked list to store the 
 *   filesystem in the directory. Newer revisions are able to use hashes
 *   and binary trees.
 *
 *   Also note that as directory grows additional blocks are assigned to
 *   store additional file records. When filename are removed, some
 *   implementations do not free theses additional blocks.
 */
static __unused int ext2_directory(struct super_block *sb, 
                                           struct inode *inode)
{
    unsigned long *p;
    unsigned int tmp;
    struct buffer_head *bh;
    struct ext2_dir_entry *de;
    char *dlimit;

    /*
     * Directory are used to hierarchically organize files. Each directory
     * can contain other directories, regular files and special files.
     *
     *                  +-----------+
     *                  |           |
     *                  | Directory |
     *                  |           |
     *                  +-----------+
     *                    |   |   |  
     *                    |   |   |
     *       o------------o   |   o------------o
     *       |                |                |
     *       |                |                |
     *       V                V                V
     * +-----------+    +-----------+    +-----------+
     * |           |    |           |    |           |
     * | Directory |    |   Files   |    |  Special  |
     * |           |    |           |    |   Files   |
     * +-----------+    +-----------+    |           |
     *                                   +-----------+
     *        
     *
     * Directories are stored as data block and refrenced by an inode. They
     * can be identified by the file type EXT2_S_IFDIR stored in the i_mode
     * field of the inode structure.
     */
    if (!S_ISDIR(inode->i_mode))
        panic("Inode is not a directory");

    /*
     * The second entry of the Inode table contains the inode pointing
     * to the data of the root directory. As defined by the 
     * EXT2_ROOT_INO constant.
     *
     * In revision 0 directory could only be stored in a linked list. 
     * Revision 1 and later introduced indexed directories. The indexed
     * directory is backward compatiblie with the linked list directory,
     * this is achieved by inserting empty directory entry records to 
     * skip over the hash index.
     *
     */
    
    p = inode->u.ext2_i.i_data;
    tmp = *p;
    if (!tmp)
        panic("Empty inode number");
    if (!(bh = bread(inode->i_dev, tmp, sb->s_blocksize))) {
        printk("Unable to read block from Disk\n");
        return -EINVAL;
    }
    
    /*
     * A directory file is a linked list of directory entry structure. 
     * Each structure contains the name of the entry, the inode associated
     * with the data of this entry, and the distance within the directory
     * file to the next entry.
     *
     * In revision 0, the type of the entry (file, directory, special file,
     * etc) has to be looked up in the inode of the file. In revision 0.5
     * and later, the file type is also contained in the directory entry
     * structure.
     *
     * Table: Linked Directory Entry Structure
     * 
     *  Offset(bytes)   Size(bytes)      Description
     * --------------------------------------------------------------
     *  0               4                inode
     *  4               2                rec_len
     *  6               1                namelen
     *  7               1                file_type
     *  8               0-255            name
     *
     * Note:
     *  Revision 0 of EXT2 used a 16bit name_len, since most implementations
     *  restricted filename to a maxumum of 255 characters this value was
     *  truncated to 8bit with the upper 8bit recycled as file_type.
     *
     *  Not available in revision 0. This field was part of the 16bit 
     *  name_len field.
     *
     * Inode
     *   32bit inode number of the file entry. A value of 0 indicate that
     *   the entry is not used.
     *
     * rec_len
     *   16bit unsigned displacement to the next directory entry from the
     *   current directory entry. This field must have a value at least 
     *   equal to the length of the current record.
     *
     *   The directory entries must be alignment on 4-bytes boundaries and
     *   there cannot be any directory entry spanning multiple data blocks.
     *   If an entry cannot completely fit in one block, it must be pushed
     *   to the next data block and the rec_len of the previous entry
     *   properly adjusted.
     *
     *   Note:
     *     Since this value cannot be negative, when a file is remove the 
     *     previous record within the block has to be modified to point to
     *     the next valid record within the block or to the end of the block
     *     when no other directory entry is present.
     *
     *     If the first entry within block is removed, a block record will 
     *     be created and point to the next directory entry or to the end
     *     of the block.
     *
     * name_len
     *   16bit unsigned value indicating how many bytes of character data
     *   are contained in the name.
     *
     *   Note:
     *     This value must never be larger than rec_len - 8. If the directory
     *     entry name is updated and cannot fit in the existing directory
     *     entry, the entry may have to be relocated in a new directory entry
     *     of sufficient size and possibly stored in a new data block.
     *
     * name
     *   Name of the entry. The ISO-Latin-1 character set is expected in
     *   most system. The name must be no longer than 255 bytes after 
     *   encoding.
     *
     */
    de = (struct ext2_dir_entry *)bh->b_data;
    dlimit = bh->b_data + sb->s_blocksize;
    while ((char *) de < dlimit) {
        if (de->inode != 0)
            printk(" %s", de->name);
        /*
         *
         * 0-----------------+----------------+-----+----------------4k
         * |                 |                |     |                |
         * | ext2_dir_entry  | ext2_dir_entry | ... | ext2_dir_entry |
         * |                 |                |     |                |
         * +-----------------+----------------+-----+----------------+
         * A <-de->rec_len-> A
         * |                 |
         * |                 |
         * |                 |
         * |                 |
         * o--de      (de + de->rec_len)
         *
         */
        de = (struct ext2_dir_entry *)
                  ((char *) de + de->rec_len);
    }

    if (bh)
        brelse(bh);
     
    return 0;
}

asmlinkage int sys_vfs_ext2fs(int fd)
{
    struct super_block *sb, *raw_sb;
    struct file *filp;
    struct inode *inode, *root;

    filp = current->filp[fd];
    inode = filp->f_inode;
    inode->i_count++;
    root = current->root;
    root->i_count++;

    /* Obtain super block for EXT2-fs */
    sb = inode->i_sb;

    /* Extablish a VFS super block for EXT2-fs */
    raw_sb = (struct super_block *)kmalloc(sizeof(struct super_block), 
               GFP_KERNEL);
    if (!raw_sb) {
        printk(KERN_ERR "No free memory to allocate sb\n");
        return -ENOMEM;
    }
    memset(raw_sb, 0, sizeof(*raw_sb));
    raw_sb->s_dev = sb->s_dev;

#ifdef CONFIG_DEBUG_EXT2_SUPERBLOCK
    ext2_superblock(raw_sb);
#endif
#ifdef CONFIG_DEBUG_EXT2_SUPERBLOCK_VFS
    ext2_superblock_vfs(raw_sb);
#endif
#ifdef CONFIG_DEBUG_EXT2_GROUP_DESC
    ext2_group_descriptor(raw_sb);
#endif
#ifdef CONFIG_DEBUG_EXT2_INODE
    ext2_inode(sb, inode);
#endif
#ifdef CONFIG_DEBUG_EXT2_DIRECTORY
    ext2_directory(sb, root);
#endif

    kfree(raw_sb);
    iput(root);
    iput(inode);

    return 0;
}

/* System call entry */
inline _syscall1(int, vfs_ext2fs, int, fd);
static inline _syscall3(int, open, const char *, file, int, flag, int, mode);
static inline _syscall1(int, close, int, fd);

static int debug_ext2fs(void)
{
    int fd;

    fd = open("/etc/rc", O_RDWR, 0);
    if (fd < 0) {
        printf("Unable to open /etc/rc\n");
        return -1;
    }
    vfs_ext2fs(fd);

    close(fd);
    return 0;
}
user1_debugcall(debug_ext2fs);
