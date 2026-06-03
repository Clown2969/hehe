#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <time.h>
#include "../src/simplefs_ioctl.h"

void do_demo(const char *mnt) {
	DIR *d = opendir(mnt);
	if (!d) return;
	struct dirent *de;
	int ok = 0, fail = 0;
	srand(time(NULL));
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char path[512]; sprintf(path, "%s/%s", mnt, de->d_name);
		uint64_t val = ((uint64_t)rand() << 32) | rand(), res;
		int fd = open(path, O_RDWR);
		if (fd >= 0) {
			if (pwrite(fd, &val, 8, 0) == 8 && pread(fd, &res, 8, 0) == 8 && res == val) ok++;
			else fail++;
			close(fd);
		}
	}
	closedir(d);
	printf("Demo: %d success, %d failed\n", ok, fail);
}

int main(int argc, char **argv) {
	if (argc < 3) return 1;
	char *cmd = argv[1];
	char *mnt = argv[2];

	if (!strcmp(cmd, "demo")) { do_demo(mnt); return 0; }
	
	int fd = open(mnt, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return 1;

	if (!strcmp(cmd, "zero")) {
		if (ioctl(fd, SFS_IOC_ZERO_FILES) == 0) printf("Zeroed\n");
	} else if (!strcmp(cmd, "erase")) {
		if (ioctl(fd, SFS_IOC_ERASE_FS) == 0) printf("Erased\n");
	} else if (!strcmp(cmd, "meta")) {
		struct sfs_meta_info buf[1024];
		struct sfs_meta_array arr = { .capacity = 1024, .buffer_ptr = (uintptr_t)buf };
		if (ioctl(fd, SFS_IOC_LIST_METADATA, &arr) == 0) {
			uint64_t n = arr.found_count < arr.capacity ? arr.found_count : arr.capacity;
			for (uint64_t i = 0; i < n; i++)
				printf("%s: sector %llu, hash %08x\n", buf[i].filename, (unsigned long long)buf[i].start_sector, buf[i].content_hash);
		}
	} else if (!strcmp(cmd, "map") && argc > 3) {
		struct sfs_block_map m; strcpy(m.filename, argv[3]);
		if (ioctl(fd, SFS_IOC_GET_BLOCKMAP, &m) == 0)
			printf("%s starts at sector %llu\n", m.filename, (unsigned long long)m.first_sector);
	}
	close(fd);
	return 0;
}
