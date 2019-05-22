#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_BLOCKS (124)
#define INDIRECT_BLOCKS (128)
#define DOUBLE_INDIRECT_BLOCKS ((INDIRECT_BLOCKS) * (INDIRECT_BLOCKS))

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    // block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    // uint32_t unused[125];               /* Not used. */

    block_sector_t direct_blocks[DIRECT_BLOCKS];
    block_sector_t indirect_block;
    block_sector_t double_indirect_block;
  };

struct inode_indirect_block {
	block_sector_t blocks[INDIRECT_BLOCKS];
};

static void inode_allocate_indirect(block_sector_t *entry, size_t nsectors, int depth);
static void inode_deallocate_indirect(block_sector_t entry, size_t nsectors, int depth);
static bool inode_allocate(struct inode_disk *disk_inode);
static bool inode_deallocate(struct inode *inode);

static inline size_t min(size_t a, size_t b) {
	return a < b ? a : b;
}

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

static block_sector_t index_to_sector(const struct inode_disk *disk, off_t index) {
	off_t index_base = 0;
	off_t index_limit = 0;
	block_sector_t ret;

	index_limit += DIRECT_BLOCKS;
	if (index < index_limit) {
		return disk->direct_blocks[index];
	}

	index_base = index_limit;
	index_limit += INDIRECT_BLOCKS;

	if (index < index_limit) {
		struct inode_indirect_block *indir_block;
		indir_block = calloc(1, sizeof(struct inode_indirect_block));
		fs_cache_read(disk->indirect_block, indir_block);
		ret = indir_block->blocks[index-index_base];
		free(indir_block);
		return ret;
	}

	index_base = index_limit;
	index_limit += DOUBLE_INDIRECT_BLOCKS;

	if (index < index_limit) {
		off_t index_first = (index - index_base) / INDIRECT_BLOCKS;
		off_t index_second = (index - index_base) % INDIRECT_BLOCKS;

		struct inode_indirect_block *indir_block;
		indir_block = calloc(1, sizeof(struct inode_indirect_block));

		fs_cache_read(disk->double_indirect_block, indir_block);
		fs_cache_read(indir_block->blocks[index_first], indir_block);
		ret = indir_block->blocks[index_second];

		free(indir_block);
		return ret;
	}

	return -1;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
	ASSERT (inode != NULL);
	if (0 <= pos && pos < inode->data.length) {
		return index_to_sector(&inode->data, pos / BLOCK_SECTOR_SIZE);
	} else {
		return -1;
	}
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if (inode_allocate(disk_inode))
        {
          fs_cache_write(sector, disk_inode);
          success = true;
        }
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  fs_cache_read(inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          free_map_release (inode->sector, 1);
          inode_deallocate(inode);
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          fs_cache_read(sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          fs_cache_read(sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  if (byte_to_sector(inode, offset + size - 1) == -1) {
    PANIC("extendable fs not implement");
  }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          fs_cache_write(sector_idx, (buffer + bytes_written));
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            fs_cache_read(sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          fs_cache_write(sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

static void inode_allocate_indirect(block_sector_t *entry, size_t nsectors, int depth) {
	static char data[BLOCK_SECTOR_SIZE];
	if (depth == 0) {
		free_map_allocate(1, entry);
		fs_cache_write(*entry, data);
		return;
	}

	struct inode_indirect_block indir_block;
	if (*entry == 0) {
		free_map_allocate(1, entry);
		fs_cache_write(*entry, data);
	}
	fs_cache_read(*entry, &indir_block);

	size_t n = (depth == 1 ? 1 : INDIRECT_BLOCKS);
	size_t l = DIV_ROUND_UP(nsectors, n);

	size_t i;
	for (i = 0; i < l; i++) {
		size_t subsize = min(nsectors, n);
		inode_allocate_indirect(&indir_block.blocks[i], subsize, depth - 1);
		nsectors -= subsize;
	}

	fs_cache_write(*entry, &indir_block);
}

static void inode_deallocate_indirect(block_sector_t entry, size_t nsectors, int depth) {
	if (depth == 0) {
		free_map_release(entry, 1);
		return;
	}

	struct inode_indirect_block indir_block;
	fs_cache_read(entry, &indir_block);

	size_t n = (depth == 1 ? 1 : INDIRECT_BLOCKS);
	size_t l = DIV_ROUND_UP(nsectors, n);

	size_t i;
	for (i = 0; i < l; i++) {
		size_t size = min(nsectors, n);
		inode_deallocate_indirect(indir_block.blocks[i], size, depth - 1);
		nsectors -= size;
	}

	free_map_release(entry, 1);
}

static bool inode_allocate(struct inode_disk *disk_inode) {
	static char data[BLOCK_SECTOR_SIZE];
	off_t length = disk_inode->length;
	if (length < 0)
		return false;

	size_t nsectors = bytes_to_sectors(length);
	size_t l = min(nsectors, DIRECT_BLOCKS);

	size_t i;
	for (i = 0; i < l; i++) {
		free_map_allocate(1, &disk_inode->direct_blocks[i]);
		fs_cache_write(disk_inode->direct_blocks[i], data);
	}

	nsectors -= l;
	if (nsectors == 0)
		return true;

	l = min(nsectors, INDIRECT_BLOCKS);
	inode_allocate_indirect(&disk_inode->indirect_block, l, 1);

	nsectors -= l;
	if (nsectors == 0)
		return true;

	l = min(nsectors, DOUBLE_INDIRECT_BLOCKS);
	inode_allocate_indirect(&disk_inode->indirect_block, l, 1);

	nsectors -= l;
	if (nsectors == 0)
		return true;

	return false;
}

static bool inode_deallocate(struct inode *inode) {
	off_t length = inode->data.length;
	if (length < 0)
		return false;

	size_t nsectors = bytes_to_sectors(length);
	size_t l = min(nsectors, DIRECT_BLOCKS);

	size_t i;
	for (i = 0; i < l; i++) {
		free_map_release(inode->data.direct_blocks[i], 1);
	}
	nsectors -= l;

	l = min(nsectors, INDIRECT_BLOCKS);
	if (l > 0) {
		inode_deallocate_indirect(inode->data.indirect_block, l, 1);
		nsectors -= l;
	}

	l = min(nsectors, DOUBLE_INDIRECT_BLOCKS);
	if (l > 0) {
		inode_deallocate_indirect(inode->data.double_indirect_block, l, 2);
		nsectors -= l;
	}

	return true;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
