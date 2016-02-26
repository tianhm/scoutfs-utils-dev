#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <uuid/uuid.h>

#include "sparse.h"
#include "util.h"
#include "format.h"
#include "cmd.h"
#include "crc.h"
#include "lebitmap.h"

/* XXX maybe these go somewhere */
#define SKF "%llu.%u.%llu"
#define SKA(k) le64_to_cpu((k)->inode), (k)->type, \
		le64_to_cpu((k)->offset)

static void *read_block(int fd, u64 blkno)
{
	ssize_t ret;
	void *buf;

	buf = malloc(SCOUTFS_BLOCK_SIZE);
	if (!buf)
		return NULL;

	ret = pread(fd, buf, SCOUTFS_BLOCK_SIZE, blkno << SCOUTFS_BLOCK_SHIFT);
	if (ret != SCOUTFS_BLOCK_SIZE) {
		fprintf(stderr, "read blkno %llu returned %zd: %s (%d)\n",
			blkno, ret, strerror(errno), errno);
		free(buf);
		buf = NULL;
	}

	return buf;
}

static void print_block_header(struct scoutfs_block_header *hdr)
{
	u32 crc = crc_block(hdr);
	char valid_str[40];

	if (crc != le32_to_cpu(hdr->crc))
		sprintf(valid_str, "# != %08x", crc);
	else
		valid_str[0] = '\0';

	printf("    header:\n"
	       "        crc: %08x %s\n"
	       "        fsid: %llx\n"
	       "        seq: %llu\n"
	       "        blkno: %llu\n",
		le32_to_cpu(hdr->crc), valid_str, le64_to_cpu(hdr->fsid),
		le64_to_cpu(hdr->seq), le64_to_cpu(hdr->blkno));
}

static void print_inode(struct scoutfs_inode *inode)
{
	printf("        inode:\n"
	       "                size: %llu\n"
	       "                blocks: %llu\n"
	       "                nlink: %u\n"
	       "                uid: %u\n"
	       "                gid: %u\n"
	       "                mode: 0%o\n"
	       "                rdev: 0x%x\n"
	       "                salt: 0x%x\n"
	       "                atime: %llu.%08u\n"
	       "                ctime: %llu.%08u\n"
	       "                mtime: %llu.%08u\n",
	       le64_to_cpu(inode->size), le64_to_cpu(inode->blocks),
	       le32_to_cpu(inode->nlink), le32_to_cpu(inode->uid),
	       le32_to_cpu(inode->gid), le32_to_cpu(inode->mode),
	       le32_to_cpu(inode->rdev), le32_to_cpu(inode->salt),
	       le64_to_cpu(inode->atime.sec),
	       le32_to_cpu(inode->atime.nsec),
	       le64_to_cpu(inode->ctime.sec),
	       le32_to_cpu(inode->ctime.nsec),
	       le64_to_cpu(inode->mtime.sec),
	       le32_to_cpu(inode->mtime.nsec));
}

static void print_item(struct scoutfs_item_header *ihdr)
{
	printf("    item:\n"
	       "        key: "SKF"\n"
	       "        len: %u\n",
	       SKA(&ihdr->key), le16_to_cpu(ihdr->len));

	switch(ihdr->key.type) {
	case SCOUTFS_INODE_KEY:
		print_inode((void *)(ihdr + 1));
		break;
	}
}

static int print_item_block(int fd, u64 nr)
{
	struct scoutfs_item_header *ihdr;
	struct scoutfs_item_block *iblk;
	size_t off;
	int i;

	iblk = read_block(fd, nr);
	if (!iblk)
		return -ENOMEM;

	printf("item block:\n");
	print_block_header(&iblk->hdr);
	printf("    first: "SKF"\n"
	       "    last: "SKF"\n"
	       "    nr_items: %u\n",
	       SKA(&iblk->first), SKA(&iblk->last),
	       le32_to_cpu(iblk->nr_items));

	off = sizeof(struct scoutfs_item_block);
	for (i = 0; i < le32_to_cpu(iblk->nr_items); i++) {
		ihdr = (void *)((char *)iblk + off);
		print_item(ihdr);

		off += sizeof(struct scoutfs_item_header) +
			le16_to_cpu(ihdr->len);
	}

	free(iblk);

	return 0;
}

static int print_log_segments(int fd, __le64 *log_segs, u64 total_chunks)
{
	int ret = 0;
	int err;
	s64 nr;

	while ((nr = find_first_le_bit(log_segs, total_chunks)) >= 0) {
		clear_le_bit(log_segs, nr);

		err = print_item_block(fd, nr << SCOUTFS_CHUNK_BLOCK_SHIFT);
		if (!ret && err)
			ret = err;
	}
	
	return ret;
}

static char *ent_type_str(u8 type)
{
	switch (type) {
		case SCOUTFS_RING_ADD_MANIFEST:
			return "ADD_MANIFEST";
		case SCOUTFS_RING_DEL_MANIFEST:
			return "DEL_MANIFEST";
		case SCOUTFS_RING_BITMAP:
			return "BITMAP";
		default:
			return "(unknown)";
	}
}

static void print_ring_entry(int fd, struct scoutfs_ring_entry *ent)
{
	struct scoutfs_ring_manifest_entry *ment;
	struct scoutfs_ring_del_manifest *del;
	struct scoutfs_ring_bitmap *bm;

	printf("    entry:\n"
	       "        type: %u # %s\n"
	       "        len: %u\n",
	       ent->type, ent_type_str(ent->type), le16_to_cpu(ent->len));

	switch(ent->type) {
	case SCOUTFS_RING_ADD_MANIFEST:
		ment = (void *)(ent + 1);
		printf("            blkno: %llu\n"
		       "            seq: %llu\n"
		       "            level: %u\n"
		       "            first: "SKF"\n"
		       "            last: "SKF"\n",
		       le64_to_cpu(ment->blkno), le64_to_cpu(ment->seq),
		       ment->level, SKA(&ment->first), SKA(&ment->last));
		break;
	case SCOUTFS_RING_DEL_MANIFEST:
		del = (void *)(ent + 1);
		printf("            blkno: %llu\n",
		       le64_to_cpu(del->blkno));
		break;
	case SCOUTFS_RING_BITMAP:
		bm = (void *)(ent + 1);
		printf("            offset: %u\n"
		       "            bits: 0x%llx%llx\n",
		       le32_to_cpu(bm->offset),
		       le64_to_cpu(bm->bits[1]), le64_to_cpu(bm->bits[0]));
		break;
	}
}

static void update_log_segs(struct scoutfs_ring_entry *ent,
			       __le64 *log_segs)
{
	struct scoutfs_ring_manifest_entry *add;
	struct scoutfs_ring_del_manifest *del;
	u64 bit;

	switch(ent->type) {
	case SCOUTFS_RING_ADD_MANIFEST:
		add = (void *)(ent + 1);
		bit = le64_to_cpu(add->blkno) >> SCOUTFS_CHUNK_BLOCK_SHIFT;
		set_le_bit(log_segs, bit);
		break;
	case SCOUTFS_RING_DEL_MANIFEST:
		del = (void *)(ent + 1);
		bit = le64_to_cpu(del->blkno) >> SCOUTFS_CHUNK_BLOCK_SHIFT;
		clear_le_bit(log_segs, bit);
		break;
	}
}

static int print_ring_block(int fd, u64 blkno, __le64 *log_segs)
{
	struct scoutfs_ring_block *ring;
	struct scoutfs_ring_entry *ent;
	size_t off;
	int ret = 0;
	int i;

	/* XXX just printing the first block for now */

	ring = read_block(fd, blkno);
	if (!ring)
		return -ENOMEM;

	printf("ring block:\n");
	print_block_header(&ring->hdr);
	printf("    nr_entries: %u\n", le16_to_cpu(ring->nr_entries));

	off = sizeof(struct scoutfs_ring_block);
	for (i = 0; i < le16_to_cpu(ring->nr_entries); i++) {
		ent = (void *)((char *)ring + off);

		update_log_segs(ent, log_segs);
		print_ring_entry(fd, ent);

		off += sizeof(struct scoutfs_ring_entry) + 
		       le16_to_cpu(ent->len);
	}

	free(ring);
	return ret;
}

static int print_map_block(int fd, u64 blkno, __le64 *log_segs)
{
	struct scoutfs_ring_map_block *map;
	int ret = 0;
	int err;
	int i;

	map = read_block(fd, blkno);
	if (!map)
		return -ENOMEM;

	printf("map block:\n");
	print_block_header(&map->hdr);
	printf("    nr_chunks: %u\n", le32_to_cpu(map->nr_chunks));

	printf("    blknos: ");
	for (i = 0; i < le32_to_cpu(map->nr_chunks); i++)
		printf("    %llu\n", le64_to_cpu(map->blknos[i]));

	for (i = 0; i < le32_to_cpu(map->nr_chunks); i++) {
		err = print_ring_block(fd, le64_to_cpu(map->blknos[i]),
				       log_segs);
		if (err && !ret)
			ret = err;
	}

	free(map);
	return 0;
}

static int print_super_brick(int fd)
{
	struct scoutfs_super_block *super;
	char uuid_str[37];
	__le64 *log_segs;
	u64 total_chunks;
	size_t bytes;
	int ret = 0;
	int err;

	/* XXX print both */
	super = read_block(fd, SCOUTFS_SUPER_BLKNO);
	if (!super)
		return -ENOMEM;

	uuid_unparse(super->uuid, uuid_str);

	total_chunks = le64_to_cpu(super->total_chunks);

	printf("super:\n");
	print_block_header(&super->hdr);
	printf("    id: %llx\n"
	       "    uuid: %s\n"
	       "    total_chunks: %llu\n"
	       "    ring_map_blkno: %llu\n"
	       "    ring_map_seq: %llu\n"
	       "    ring_first_block: %llu\n"
	       "    ring_active_blocks: %llu\n"
	       "    ring_total_blocks: %llu\n"
	       "    ring_seq: %llu\n",
	       le64_to_cpu(super->id),
	       uuid_str,
	       total_chunks,
	       le64_to_cpu(super->ring_map_blkno),
	       le64_to_cpu(super->ring_map_seq),
	       le64_to_cpu(super->ring_first_block),
	       le64_to_cpu(super->ring_active_blocks),
	       le64_to_cpu(super->ring_total_blocks),
	       le64_to_cpu(super->ring_seq));

	/* XXX by hand? */
	bytes = (total_chunks + 63) / 8;
	log_segs = malloc(bytes);
	if (!log_segs) {
		ret = -ENOMEM;
		goto out;
	}
	memset(log_segs, 0, bytes);

	err = print_map_block(fd, le64_to_cpu(super->ring_map_blkno), log_segs);
	if (err && !ret)
		ret = err;

	err = print_log_segments(fd, log_segs, total_chunks);
	if (err && !ret)
		ret = err;

out:
	free(super);
	free(log_segs);
	return ret;
}

static int print_cmd(int argc, char **argv)
{
	char *path;
	int ret;
	int fd;

	if (argc != 1) {
		printf("scoutfs print: a single path argument is required\n");
		return -EINVAL;
	}
	path = argv[0];

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "failed to open '%s': %s (%d)\n",
			path, strerror(errno), errno);
		return ret;
	}

	ret = print_super_brick(fd);
	close(fd);
	return ret;
};

static void __attribute__((constructor)) print_ctor(void)
{
	cmd_register("print", "<device>", "print metadata structures",
			print_cmd);
}
