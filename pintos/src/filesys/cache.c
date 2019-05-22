#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

#define CACHE_SIZE 64

struct cache_entry_t {
	block_sector_t sector;
	uint8_t data[BLOCK_SECTOR_SIZE];
	bool access;
	bool dirty;
	bool free;
};

static struct cache_entry_t cache[CACHE_SIZE];

static struct lock cache_lock;

static struct cache_entry_t *fs_cache_find(block_sector_t sector);
static void fs_cache_flush(struct cache_entry_t *entry);
static struct cache_entry_t *fs_cache_evict(void);

static void fs_cache_flush(struct cache_entry_t *entry) {
	if (entry->dirty) {
		block_write(fs_device, entry->sector, entry->data);
		entry->dirty = false;
	}
}

/* find cache entry with specified sector, otherwise return NULL */
static struct cache_entry_t *fs_cache_find(block_sector_t sector) {
	size_t i;
	for (i = 0; i < CACHE_SIZE; i++) {
		if (!(cache[i].free) && cache[i].sector == sector) {
			return &(cache[i]);
		}
	}
	return NULL;
}

static struct cache_entry_t *fs_cache_evict(void) {
	static size_t clock = 0;
	for (;;clock = (clock + 1) % CACHE_SIZE) {
		if (cache[clock].free)
			return &(cache[clock]);
		if (cache[clock].access)
			cache[clock].access = false;
		else
			break;

	}

	struct cache_entry_t *entry = &(cache[clock]);
	if (entry->dirty)
		fs_cache_flush(entry);

	entry->free = true;
	return entry;
}

void fs_cache_init(void) {
	lock_init(&cache_lock);
	size_t i;
	for (i = 0; i < CACHE_SIZE; i++) {
		cache[i].free = true;
	}
}

void fs_cache_destroy(void){
	lock_acquire(&cache_lock);
	size_t i;
	for (i = 0; i < CACHE_SIZE; i++) {
		if (!(cache[i].free))
			fs_cache_flush(&(cache[i]));
	}
	lock_release(&cache_lock);
}

void fs_cache_read(block_sector_t sector, void *t) {
	lock_acquire(&cache_lock);
	struct cache_entry_t *entry = fs_cache_find(sector);
	if (entry)
		goto read;
	entry = fs_cache_evict();
	entry->free = false;
	entry->sector = sector;
	entry->dirty = false;
	block_read(fs_device, sector, entry->data);
read:
	entry->access = true;
	memcpy(t, entry->data, BLOCK_SECTOR_SIZE);
	lock_release(&cache_lock);
}

void fs_cache_write(block_sector_t sector, const void *s) {
	lock_acquire(&cache_lock);
	struct cache_entry_t *entry = fs_cache_find(sector);
	if (entry)
		goto write;
	entry = fs_cache_evict();
	entry->free = false;
	entry->sector = sector;
	entry->dirty = false;
	block_read(fs_device, sector, entry->data);
write:
	entry->access = true;
	entry->dirty = true;
	memcpy(entry->data, s, BLOCK_SECTOR_SIZE);
	lock_release(&cache_lock);
}
