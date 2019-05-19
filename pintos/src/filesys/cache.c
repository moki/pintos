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

void fs_cache_init(void) {
	lock_init(&cache_lock);

	size_t i;
	for (i = 0; i < CACHE_SIZE; i++)
		cache[i].free = true;
}

void fs_cache_destroy(void){

}

void fs_cache_read(block_sector_t sector, void *t) {
	block_read(fs_device, sector, t);
}

void fs_cache_write(block_sector_t sector, const void *s) {
	block_write(fs_device, sector, s);
}
