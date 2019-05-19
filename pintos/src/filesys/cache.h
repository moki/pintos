#ifndef FS_CACHE_H
#define FS_CACHE_H

#include "devices/block.h"

void fs_cache_init(void);
void fs_cache_destroy(void);

void fs_cache_read(block_sector_t sector, void *t);
void fs_cache_write(block_sector_t sector, const void *s);

#endif
