#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "simplefs_ioctl.h"

#define SFS_MAGIC 0xDEADBEEF
#define SFS_BLOCK_SIZE 512
#define SFS_ROOT_INO 1
#define SFS_FILE_INO_START 10

static char *disk_name = "";
static int sb1_offset = 0;
static int sb2_offset = 8;
static int max_name_len = 32;
static int max_file_sectors = 1;
static int format = 0;

module_param(disk_name, charp, 0444);
module_param(sb1_offset, int, 0444);
module_param(sb2_offset, int, 0444);
module_param(max_name_len, int, 0444);
module_param(max_file_sectors, int, 0444);
module_param(format, int, 0444);
MODULE_PARM_DESC(format, "Create a fresh filesystem instead of mounting an existing one");

struct sfs_super_block_disk {
	__le32 magic;
	__le32 file_count;
	__le32 sectors_per_file;
	__le32 sb2_sector;
	__le32 crc;
	__u8 padding[SFS_BLOCK_SIZE - 20];
};

struct sfs_fs_info {
	u32 n_files;
	u32 file_size_sectors;
	u32 backup_sb_sector;
	u32 main_sb_sector;
};

static uint32_t sfs_calc_crc(struct sfs_super_block_disk *sb)
{
	uint32_t old = sb->crc;
	uint32_t res;
	sb->crc = 0;
	res = crc32_le(~0, (unsigned char *)sb, SFS_BLOCK_SIZE);
	sb->crc = old;
	return res;
}

static sector_t sfs_get_sector(struct sfs_fs_info *info, u64 file_idx, u32 sector_in_file)
{
	sector_t s = (sector_t)file_idx * info->file_size_sectors + sector_in_file;
	sector_t low = info->main_sb_sector;
	sector_t high = info->backup_sb_sector;
	if (low > high) { sector_t t = low; low = high; high = t; }
	if (s >= low) s++;
	if (s >= high) s++;
	return s;
}

static ssize_t sfs_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct sfs_fs_info *info = inode->i_sb->s_fs_info;
	u64 idx = inode->i_ino - SFS_FILE_INO_START;
	size_t total_size = (size_t)info->file_size_sectors * SFS_BLOCK_SIZE;
	size_t read_bytes = 0;

	if (idx >= info->n_files) return -EIO;
	if (*ppos >= total_size) return 0;
	if (count > total_size - *ppos) count = total_size - *ppos;

	while (read_bytes < count) {
		u32 sec_idx = (*ppos) / SFS_BLOCK_SIZE;
		u32 offset = (*ppos) % SFS_BLOCK_SIZE;
		u32 chunk = min_t(u32, SFS_BLOCK_SIZE - offset, count - read_bytes);
		struct buffer_head *bh = sb_bread(inode->i_sb, sfs_get_sector(info, idx, sec_idx));
		if (!bh) return -EIO;
		if (copy_to_user(buf + read_bytes, bh->b_data + offset, chunk)) {
			brelse(bh);
			return -EFAULT;
		}
		brelse(bh);
		read_bytes += chunk;
		*ppos += chunk;
	}
	return read_bytes;
}

static ssize_t sfs_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct sfs_fs_info *info = inode->i_sb->s_fs_info;
	u64 idx = inode->i_ino - SFS_FILE_INO_START;
	size_t total_size = (size_t)info->file_size_sectors * SFS_BLOCK_SIZE;
	size_t written = 0;

	if (idx >= info->n_files) return -EIO;
	if (*ppos >= total_size) return -ENOSPC;
	if (count > total_size - *ppos) count = total_size - *ppos;

	while (written < count) {
		u32 sec_idx = (*ppos) / SFS_BLOCK_SIZE;
		u32 offset = (*ppos) % SFS_BLOCK_SIZE;
		u32 chunk = min_t(u32, SFS_BLOCK_SIZE - offset, count - written);
		struct buffer_head *bh = sb_bread(inode->i_sb, sfs_get_sector(info, idx, sec_idx));
		if (!bh) return -EIO;
		if (copy_from_user(bh->b_data + offset, buf + written, chunk)) {
			brelse(bh);
			return -EFAULT;
		}
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		written += chunk;
		*ppos += chunk;
	}
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));
	return written;
}
static int sfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct sfs_fs_info *info = file_inode(file)->i_sb->s_fs_info;
	if (!dir_emit_dots(file, ctx)) return 0;
	while (ctx->pos < info->n_files + 2) {
		char name[256];
		u64 i = ctx->pos - 2;
		int len = scnprintf(name, sizeof(name), "file%03llu", i);
		if (!dir_emit(ctx, name, len, SFS_FILE_INO_START + i, DT_REG)) break;
		ctx->pos++;
	}
	return 0;
}

static uint32_t sfs_calc_file_hash(struct super_block *sb, u64 idx)
{
	struct sfs_fs_info *info = sb->s_fs_info;
	uint32_t h = ~0U;
	int i;
	for (i = 0; i < info->file_size_sectors; i++) {
		struct buffer_head *bh = sb_bread(sb, sfs_get_sector(info, idx, i));
		if (bh) {
			h = crc32_le(h, bh->b_data, SFS_BLOCK_SIZE);
			brelse(bh);
		}
	}
	return h;
}

static long sfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct sfs_fs_info *info = sb->s_fs_info;
	u64 i;
	int j;

	switch (cmd) {
	case SFS_IOC_ZERO_FILES:
		for (i = 0; i < info->n_files; i++) {
			for (j = 0; j < info->file_size_sectors; j++) {
				struct buffer_head *bh = sb_bread(sb, sfs_get_sector(info, i, j));
				if (bh) {
					memset(bh->b_data, 0, SFS_BLOCK_SIZE);
					mark_buffer_dirty(bh);
					sync_dirty_buffer(bh);
					brelse(bh);
				}
			}
		}
		return 0;
	case SFS_IOC_ERASE_FS:
		{
			struct buffer_head *bh;
			bh = sb_bread(sb, info->main_sb_sector);
			if (bh) { memset(bh->b_data, 0, SFS_BLOCK_SIZE); mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh); }
			bh = sb_bread(sb, info->backup_sb_sector);
			if (bh) { memset(bh->b_data, 0, SFS_BLOCK_SIZE); mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh); }
			info->n_files = 0;
		}
		return 0;
	case SFS_IOC_LIST_METADATA:
		{
			struct sfs_meta_array arr;
			if (copy_from_user(&arr, (void __user *)arg, sizeof(arr))) return -EFAULT;
			arr.found_count = info->n_files;
			struct sfs_meta_info __user *user_buf = (struct sfs_meta_info __user *)(uintptr_t)arr.buffer_ptr;
			for (i = 0; i < min(arr.capacity, (u64)info->n_files); i++) {
				struct sfs_meta_info entry;
				memset(&entry, 0, sizeof(entry));
				entry.index = i;
				entry.start_sector = sfs_get_sector(info, i, 0);
				entry.n_sectors = info->file_size_sectors;
				entry.content_hash = sfs_calc_file_hash(sb, i);
				scnprintf(entry.filename, sizeof(entry.filename), "file%03llu", i);
				if (copy_to_user(&user_buf[i], &entry, sizeof(entry))) return -EFAULT;
			}
			if (copy_to_user((void __user *)arg, &arr, sizeof(arr))) return -EFAULT;
		}
		return 0;
	case SFS_IOC_GET_BLOCKMAP:
		{
			struct sfs_block_map m;
			unsigned long long idx_long;
			if (copy_from_user(&m, (void __user *)arg, sizeof(m))) return -EFAULT;
			m.filename[255] = 0;
			if (strncmp(m.filename, "file", 4) != 0 || kstrtoull(m.filename + 4, 10, &idx_long)) return -EINVAL;
			if (idx_long >= info->n_files) return -ENOENT;
			m.first_sector = sfs_get_sector(info, idx_long, 0);
			m.sector_count = info->file_size_sectors;
			if (copy_to_user((void __user *)arg, &m, sizeof(m))) return -EFAULT;
		}
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations sfs_file_ops = { .read = sfs_read, .write = sfs_write, .unlocked_ioctl = sfs_ioctl, .llseek = generic_file_llseek };
static const struct file_operations sfs_dir_ops = { .iterate_shared = sfs_iterate, .read = generic_read_dir, .unlocked_ioctl = sfs_ioctl, .llseek = generic_file_llseek };

static struct inode *sfs_get_inode(struct super_block *sb, u64 ino)
{
	struct inode *inode = new_inode(sb);
	struct sfs_fs_info *info = sb->s_fs_info;
	if (inode) {
		inode->i_ino = ino;
		inode_set_atime_to_ts(inode, current_time(inode));
		inode_set_mtime_to_ts(inode, current_time(inode));
		inode_set_ctime_to_ts(inode, current_time(inode));
		if (ino == SFS_ROOT_INO) {
			inode->i_mode = S_IFDIR | 0755;
			inode->i_fop = &sfs_dir_ops;
		} else {
			inode->i_mode = S_IFREG | 0644;
			inode->i_fop = &sfs_file_ops;
			inode->i_size = (loff_t)info->file_size_sectors * SFS_BLOCK_SIZE;
		}
	}
	return inode;
}

static struct dentry *sfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct sfs_fs_info *info = dir->i_sb->s_fs_info;
	u64 idx;
	if (dentry->d_name.len > max_name_len) return ERR_PTR(-ENAMETOOLONG);
	if (strncmp(dentry->d_name.name, "file", 4) == 0 && !kstrtoull(dentry->d_name.name + 4, 10, &idx) && idx < info->n_files)
		return d_splice_alias(sfs_get_inode(dir->i_sb, SFS_FILE_INO_START + idx), dentry);
	return NULL;
}
static const struct inode_operations sfs_dir_inode_ops = { .lookup = sfs_lookup };

static bool sfs_super_valid(struct sfs_super_block_disk *ds)
{
	return le32_to_cpu(ds->magic) == SFS_MAGIC &&
	       ds->crc == cpu_to_le32(sfs_calc_crc(ds));
}

static void sfs_super_apply(struct sfs_fs_info *info, struct sfs_super_block_disk *ds)
{
	info->n_files = le32_to_cpu(ds->file_count);
	info->file_size_sectors = le32_to_cpu(ds->sectors_per_file);
}

static int sfs_create_super(struct super_block *sb, struct sfs_fs_info *info)
{
	struct buffer_head *bh, *bh2;
	struct sfs_super_block_disk *ds;
	sector_t total = bdev_nr_sectors(sb->s_bdev);

	if (total <= 2) return -ENOSPC;

	info->n_files = (u32)div_u64(total - 2, info->file_size_sectors);

	bh = sb_bread(sb, info->main_sb_sector);
	if (!bh) return -EIO;
	ds = (struct sfs_super_block_disk *)bh->b_data;
	memset(ds, 0, SFS_BLOCK_SIZE);
	ds->magic = cpu_to_le32(SFS_MAGIC);
	ds->file_count = cpu_to_le32(info->n_files);
	ds->sectors_per_file = cpu_to_le32(info->file_size_sectors);
	ds->sb2_sector = cpu_to_le32(info->backup_sb_sector);
	ds->crc = cpu_to_le32(sfs_calc_crc(ds));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	bh2 = sb_bread(sb, info->backup_sb_sector);
	if (!bh2) { brelse(bh); return -EIO; }
	memcpy(bh2->b_data, ds, SFS_BLOCK_SIZE);
	mark_buffer_dirty(bh2);
	sync_dirty_buffer(bh2);
	brelse(bh2);
	brelse(bh);
	return 0;
}

static int sfs_load_super(struct super_block *sb, struct sfs_fs_info *info, int silent)
{
	struct buffer_head *bh, *bh2;
	struct sfs_super_block_disk *ds, *ds2;

	bh = sb_bread(sb, info->main_sb_sector);
	if (!bh) return -EIO;
	ds = (struct sfs_super_block_disk *)bh->b_data;

	if (sfs_super_valid(ds)) {
		sfs_super_apply(info, ds);
		brelse(bh);
		return 0;
	}

	if (!silent)
		pr_warn("simplefs: primary superblock invalid, trying backup\n");

	bh2 = sb_bread(sb, info->backup_sb_sector);
	if (!bh2) { brelse(bh); return -EIO; }
	ds2 = (struct sfs_super_block_disk *)bh2->b_data;

	if (!sfs_super_valid(ds2)) {
		pr_err("simplefs: both superblocks are invalid\n");
		brelse(bh2);
		brelse(bh);
		return -EINVAL;
	}

	sfs_super_apply(info, ds2);
	memcpy(ds, ds2, SFS_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh2);
	brelse(bh);
	return 0;
}

static int sfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct sfs_fs_info *info;
	struct inode *root;
	int err;

	if (!sb_set_blocksize(sb, SFS_BLOCK_SIZE)) return -EINVAL;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) return -ENOMEM;
	sb->s_fs_info = info;

	info->main_sb_sector = sb1_offset;
	info->backup_sb_sector = sb2_offset;
	info->file_size_sectors = max_t(u32, max_file_sectors, 1);

	if (format)
		err = sfs_create_super(sb, info);
	else
		err = sfs_load_super(sb, info, silent);
	if (err) goto fail;

	sb->s_magic = SFS_MAGIC;
	root = sfs_get_inode(sb, SFS_ROOT_INO);
	if (!root) { err = -ENOMEM; goto fail; }
	root->i_op = &sfs_dir_inode_ops;
	sb->s_root = d_make_root(root);
	if (!sb->s_root) { err = -ENOMEM; goto fail; }
	return 0;

fail:
	kfree(info);
	sb->s_fs_info = NULL;
	return err;
}

static struct dentry *sfs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, sfs_fill_super);
}

static void sfs_kill_sb(struct super_block *sb) { kfree(sb->s_fs_info); kill_block_super(sb); }

static struct file_system_type sfs_type = {
	.owner = THIS_MODULE,
	.name = "simplefs",
	.mount = sfs_mount,
	.kill_sb = sfs_kill_sb,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init sfs_init(void) { return register_filesystem(&sfs_type); }
static void __exit sfs_exit(void) { unregister_filesystem(&sfs_type); }

module_init(sfs_init);
module_exit(sfs_exit);

MODULE_LICENSE("GPL");
