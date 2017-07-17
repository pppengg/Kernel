/*
 * linux/fs/inode.c
 *
 * (C) 1991 Linus Torvalds
 */
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <asm/system.h>
#include <sys/stat.h>
#include <linux/kernel.h>

#include <string.h>

struct m_inode inode_table[NR_INODE] = {{0, }, };

static void write_inode(struct m_inode *inode);

static inline void wait_on_inode(struct m_inode *inode)
{
    cli();
    while (inode->i_lock)
        sleep_on(&inode->i_wait);
    sti();
}

static inline void lock_inode(struct m_inode *inode)
{
    cli();
    while (inode->i_lock)
        sleep_on(&inode->i_wait);
    inode->i_lock = 1;
    sti();
}

static inline void unlock_inode(struct m_inode *inode)
{
    inode->i_lock = 0;
    wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev)
{
    int i;
    struct m_inode *inode;

    inode = 0 + inode_table;
    for (i = 0; i < NR_INODE; i++, inode++) {
        wait_on_inode(inode);
        if (inode->i_dev == dev) {
            if (inode->i_count)
                printk("inode in use on removed disk\n\r");
            inode->i_dev = inode->i_dirt = 0;
        }
    }
}

void iput(struct m_inode *inode)
{
    if (!inode)
        return;
    wait_on_inode(inode);
    if (!inode->i_count)
        panic("iput: trying to free free inode");
    if (inode->i_pipe) {
       wake_up(&inode->i_wait);
       if (--inode->i_count)
           return;
       free_page(inode->i_size);
       inode->i_count = 0;
       inode->i_dirt  = 0;
       inode->i_pipe  = 0;
       return;
    }
    if (!inode->i_dev) {
        inode->i_count--;
        return;
    }
    if (S_ISBLK(inode->i_mode)) {
        sync_dev(inode->i_zone[0]);
        wait_on_inode(inode);
    }
repeat:
    if (inode->i_count > 1) {
        inode->i_count--;
        return;
    }
    if (!inode->i_nlinks) {
        truncate(inode);
        free_inode(inode);
        return;
    }
    if (inode->i_dirt) {
        write_inode(inode);    /* we can sleep - so do again */
        wait_on_inode(inode);
        goto repeat;
    }
    inode->i_count--;
    return;
}

static void write_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;
    int block;

    lock_inode(inode);
    if (!inode->i_dirt || !inode->i_dev) {
        unlock_inode(inode);
        return;
    }
    if (!(sb = get_super(inode->i_dev)))
        panic("trying to write inode without device");
    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
           (inode->i_num - 1) / INODES_PER_BLOCK;
    if (!(bh = bread(inode->i_dev, block)))
        panic("unable to read i-node block");
    ((struct d_inode *)bh->b_data)
             [(inode->i_num - 1) % INODES_PER_BLOCK] =
                  *(struct d_inode *)inode;
    bh->b_dirt = 1;
    inode->i_dirt = 0;
    brelse(bh);
    unlock_inode(inode);
}

void sync_inodes(void)
{
    int i;
    struct m_inode *inode;

    inode = 0 + inode_table;
    for (i = 0; i < NR_INODE; i++, inode++) {
       wait_on_inode(inode);
       if (inode->i_dirt && !inode->i_pipe)
          write_inode(inode);
    }
}

struct m_inode *get_empty_inode(void)
{
    struct m_inode *inode;
    static struct m_inode *last_inode = inode_table;
    int i;

    do {
        inode = NULL;
        for (i = NR_INODE; i; i--) {
            if (++last_inode >= inode_table + NR_INODE)
                last_inode = inode_table;
            if (!last_inode->i_count) {
                inode = last_inode;
                if (!inode->i_dirt && !inode->i_lock)
                    break;
            }
        }
        if (!inode) {
            for (i = 0; i < NR_INODE; i++)
                printk("%04x:%6d\t", inode_table[i].i_dev,
                       inode_table[i].i_num);
                panic("No free inodes in mem");
        }
        wait_on_inode(inode);
        while (inode->i_dirt) {
            write_inode(inode);
            wait_on_inode(inode);
        }
    } while (inode->i_count);
    memset(inode, 0, sizeof(*inode));
    inode->i_count = 1;
    return inode;
}

static void read_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;
    int block;

    lock_inode(inode);
    if (!(sb = get_super(inode->i_dev)))
        panic("tring to read inode without dev");
    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
            (inode->i_num - 1) / INODES_PER_BLOCK;
    if (!(bh = bread(inode->i_dev, block)))
        panic("unable to read i_node block");
    *(struct d_inode *)inode =
        ((struct d_inode *)bh->b_data)
            [(inode->i_num - 1) % INODES_PER_BLOCK];
    brelse(bh);
    unlock_inode(inode);
}

struct m_inode *iget(int dev, int nr)
{
    struct m_inode * inode, *empty;

    if (!dev)
        panic("iget with dev==0");
    empty = get_empty_inode();
    inode = inode_table;
    while (inode < NR_INODE + inode_table) {
        if (inode->i_dev != dev || inode->i_num != nr) {
            inode++;
            continue;
        }
        wait_on_inode(inode);
        if (inode->i_dev != dev || inode->i_num != nr) {
            inode = inode_table;
            continue;
        }
        inode->i_count++;
        if (inode->i_mount) {
            int i;

            for (i = 0; i < NR_SUPER; i++)
                if (super_block[i].s_imount == inode)
                    break;
            if (i >= NR_SUPER) {
                printk("Mounted inode hasn't got sb\n");
                if (empty)
                    iput(empty);
                return inode;
            }
            iput(inode);
            dev = super_block[i].s_dev;
            nr = ROOT_INO;
            inode = inode_table;
            continue;
        }
        if (empty)
            iput(empty);
        return inode;
    }
    if (!empty)
        return NULL;
    inode = empty;
    inode->i_dev = dev;
    inode->i_num = nr;
    read_inode(inode);
    return inode;
}