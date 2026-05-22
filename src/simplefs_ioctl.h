#ifndef SIMPLEFS_IOCTL_H
#define SIMPLEFS_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <stdint.h>
#endif

#define SFS_IOC_MAGIC 'Z'

struct sfs_meta_info {
	uint64_t index;
	uint64_t start_sector;
	uint32_t n_sectors;
	uint32_t content_hash;
	char filename[256];
};

struct sfs_meta_array {
	uint64_t capacity;
	uint64_t found_count;
	uint64_t buffer_ptr;
};

struct sfs_block_map {
	char filename[256];
	uint64_t first_sector;
	uint32_t sector_count;
};

#define SFS_IOC_ZERO_FILES   _IO(SFS_IOC_MAGIC, 0x01)
#define SFS_IOC_ERASE_FS     _IO(SFS_IOC_MAGIC, 0x02)
#define SFS_IOC_LIST_METADATA _IOWR(SFS_IOC_MAGIC, 0x03, struct sfs_meta_array)
#define SFS_IOC_GET_BLOCKMAP _IOWR(SFS_IOC_MAGIC, 0x04, struct sfs_block_map)

#endif
