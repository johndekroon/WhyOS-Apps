/* SPDX-License-Identifier: MIT */
/* Files: the drives and mounting them (disks.h); from why2025-linux home.c. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disks.h"

#ifdef FILES_HOST
#define vfork fork	/* deprecated on macOS; the badge (NOMMU) has only vfork */
#else
#include <sys/mount.h>
#include <sys/statfs.h>
#endif

char *human(char *p, unsigned long long b)
{
	static const char *const unit[] = { "B", "KB", "MB", "GB", "TB" };
	unsigned long long whole = b, frac = 0;
	int u = 0;

	while (whole >= 1024 && u < 4) {
		frac = (whole % 1024) * 10 / 1024;
		whole /= 1024;
		u++;
	}
	if (u && whole < 100 && frac)
		return p + snprintf(p, 24, "%u.%u %s", (unsigned)whole, (unsigned)frac, unit[u]);
	return p + snprintf(p, 24, "%u %s", (unsigned)whole, unit[u]);
}

char *mounts_next(char *line, char *dev, char *mnt, int max)
{
	char *next;
	int i = 0, j = 0;

	if (!line || !*line)
		return 0;
	next = strchr(line, '\n');
	if (next)
		*next++ = 0;
	while (line[i] && line[i] != ' ' && i < max - 1)
		dev[i] = line[i], i++;
	dev[i] = 0;
	while (line[i] == ' ')
		i++;
	while (line[i] && line[i] != ' ' && j < max - 1)
		mnt[j++] = line[i++];
	mnt[j] = 0;
	return next ? next : line + strlen(line);
}

const struct diskinfo *fs_disk(const struct disks *d, const struct fsinfo *f)
{
	return f->disk >= 0 && f->disk < d->n_disk ? &d->disk[f->disk] : 0;
}

char *fs_name(const struct disks *d, const struct fsinfo *f, char *out, size_t n)
{
	const struct diskinfo *dk = fs_disk(d, f);
	int parts = 0;

	if (!dk) {
		snprintf(out, n, "%s", f->label);
		return out;
	}
	for (int i = 0; i < d->n; i++)
		parts += d->fs[i].disk == f->disk;
	if (parts > 1)
		snprintf(out, n, "%s, %s", dk->label, f->label);
	else
		snprintf(out, n, "%s", dk->label);
	return out;
}

const struct fsinfo *disks_fs_of(const struct disks *d, const char *dir)
{
	const struct fsinfo *best = 0;
	size_t bn = 0;

	for (int i = 0; i < d->n; i++) {
		size_t n = strlen(d->fs[i].mnt);

		if (n == 1)	/* "/": everything is in it */
			n = 0;
		if (d->fs[i].mnt[0] && (n > bn || !best) && !strncmp(dir, d->fs[i].mnt, n) &&
		    (dir[n] == '/' || !dir[n])) {
			best = &d->fs[i];
			bn = n;
		}
	}
	return best;
}

const struct fsinfo *disks_mount_at(const struct disks *d, const char *path)
{
	for (int i = 0; i < d->n; i++)
		if (d->fs[i].mnt[0] && !strcmp(d->fs[i].mnt, path))
			return &d->fs[i];
	return 0;
}

#ifdef FILES_HOST

/* a card in the badge layout with its FAT open, a USB stick, the badge's own */
void disks_read(struct disks *d)
{
	static const struct fsinfo fs[] = {
		{ "Partition 1", "/dev/mmcblk0p1", "/media/sd", "FAT32", 15ULL << 30, 2ULL << 30, 0, 1 },
		{ "Partition 2", "/dev/mmcblk0p2", "/mnt/sd", "f2fs", 256ULL << 20, 41ULL << 20, 0, 2 },
		{ "USB drive", "/dev/sda1", "", "exFAT", 64ULL << 30, 0, 1, 1 },
		{ "Badge storage", "", "/data", "JFFS2", 4480ULL << 10, 1636ULL << 10, -1, 0 },
		{ "Memory", "", "/tmp", "tmpfs", 13ULL << 20, 4ULL << 20, -1, 0 },
		{ "System", "", "/", "squashfs", 4044ULL << 10, 4044ULL << 10, -1, 0 },
	};
	static const struct diskinfo disk[] = {
		{ "mmcblk0", "SD card", 0, 16ULL << 30 },
		{ "sda", "SanDisk Ultra", 1, 64ULL << 30 },
	};

	memset(d, 0, sizeof(*d));
	d->n_disk = 2;
	memcpy(d->disk, disk, sizeof(disk));
	d->n = 6;
	memcpy(d->fs, fs, sizeof(fs));
}

int disks_open(struct disks *d, int i, char *notice, size_t n)
{
	if (d->fs[i].mnt[0])
		return i;
	snprintf(notice, n, "Could not mount it: not FAT, exFAT or f2fs");
	return -1;
}

void disks_fat_follow(int files)
{
	(void)files;
}

#else /* the badge */

static int read_str(const char *path, char *buf, int max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), n;

	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, (size_t)max - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = 0;
	return n;
}

static unsigned long long read_ull(const char *path)
{
	char buf[24];
	unsigned long long v = 0;

	if (read_str(path, buf, sizeof(buf)) <= 0)
		return 0;
	for (char *p = buf; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (unsigned)(*p - '0');
	return v;
}

/* f2fs's superblock magic, 1 KB into dev? */
static int is_f2fs(const char *dev)
{
	unsigned char sb[4];
	int fd = open(dev, O_RDONLY | O_CLOEXEC), ok;

	if (fd < 0)
		return 0;
	ok = pread(fd, sb, 4, 1024) == 4 &&	/* 0xF2F52010, little-endian */
	     sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2;
	close(fd);
	return ok;
}

/* the SD card in the badge layout (Disks formats it so, docs/DATA.md) */
static int sd_badge(void)
{
	return is_f2fs(SD_F2FS);
}

/* where the menu mounts f: the badge layout's FAT in /media/sd, USB drives
 * in /media by partition, any other card where S15sdcard does */
static void mnt_point(const struct disks *d, const struct fsinfo *f, char *out, size_t n)
{
	const struct diskinfo *k = fs_disk(d, f);

	if (k && k->usb)
		snprintf(out, n, USB_MNT "%s", f->dev + 5);
	else if (!strcmp(f->dev, SD_FAT) && sd_badge())
		snprintf(out, n, SD_FAT_MNT);
	else
		snprintf(out, n, SD_MNT);
}

/* has usbd lent the card to a computer: the whole card, or the badge
 * layout's FAT? (usbd's /var/run/usb.state, why2025-linux docs/USB.md) */
static int sd_lent(const struct fsinfo *f)
{
	char buf[192];

	return read_str("/var/run/usb.state", buf, sizeof(buf)) > 0 && strstr(buf, "disk=1") &&
	       !strncmp(f->dev, "/dev/mmcblk0", 12) && (!strcmp(f->dev, SD_FAT) || !sd_badge());
}

static unsigned le16(const unsigned char *p)
{
	return p[0] | p[1] << 8;
}

static unsigned long le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

/* the file system from the boot sector */
static void fs_probe(const char *dev, char *type, size_t n)
{
	unsigned char bs[512];
	int fd = open(dev, O_RDONLY | O_CLOEXEC);

	snprintf(type, n, "unknown");
	if (fd < 0)
		return;
	if (read(fd, bs, sizeof(bs)) == (int)sizeof(bs)) {
		/* FAT12/16/32 by the cluster count, as the FAT spec says */
		unsigned bps = le16(bs + 11), spc = bs[13], rsvd = le16(bs + 14), nfat = bs[16];
		unsigned root = (le16(bs + 17) * 32 + bps - 1) / (bps ? bps : 1);
		unsigned long fatsz = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
		unsigned long tot = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
		unsigned long meta = rsvd + nfat * fatsz + root;

		if (is_f2fs(dev))
			snprintf(type, n, "f2fs");
		else if (!memcmp(bs + 3, "EXFAT   ", 8))
			snprintf(type, n, "exFAT");
		else if (bs[510] == 0x55 && bs[511] == 0xaa && (bps == 512 || bps == 1024 ||
			 bps == 2048 || bps == 4096) && spc && !(spc & (spc - 1)) &&
			 rsvd && nfat && fatsz && tot > meta) {
			unsigned long clusters = (tot - meta) / spc;

			snprintf(type, n, "%s", clusters < 4085 ? "FAT12" : clusters < 65525 ? "FAT16" : "FAT32");
		} else {
			snprintf(type, n, "%s", bs[510] == 0x55 && bs[511] == 0xaa ? "unknown" : "empty");
		}
	}
	close(fd);
}

/* mount point and usage of dev (or of mnt when dev is ""), from /proc/mounts */
static void fs_usage(struct fsinfo *f)
{
	char buf[2048], dev[48], mnt[48], *line = buf;
	struct statfs st;

	if (read_str("/proc/mounts", buf, sizeof(buf)) <= 0)
		return;
	while ((line = mounts_next(line, dev, mnt, sizeof(dev)))) {
		if (f->dev[0] ? strcmp(dev, f->dev) : strcmp(mnt, f->mnt))
			continue;
		snprintf(f->mnt, sizeof(f->mnt), "%s", mnt);
		if (statfs(mnt, &st) == 0) {
			f->size = (unsigned long long)st.f_blocks * st.f_bsize;
			f->used = (unsigned long long)(st.f_blocks - st.f_bfree) * st.f_bsize;
		}
		/* tmpfs on NOMMU has no size of its own (df says 0): it takes
		 * memory, so show the memory, as MemTotal and MemAvailable */
		if (!f->size && !strcmp(f->type, "tmpfs") && read_str("/proc/meminfo", buf, sizeof(buf)) > 0) {
			const char *t = strstr(buf, "MemTotal:"), *a = strstr(buf, "MemAvailable:");

			if (t && a) {
				f->size = strtoull(t + 9, 0, 10) * 1024;
				f->used = f->size - strtoull(a + 13, 0, 10) * 1024;
			}
		}
		return;
	}
	if (f->dev[0])
		f->mnt[0] = 0;
}

/* trim and squeeze spaces: SCSI vendor and model strings are padded */
static void squeeze(char *s)
{
	char *d = s, *start = s;
	int sp = 0;

	for (; *s; s++) {
		if (*s == ' ' || *s == '\t' || *s == '\n') {
			sp = d != start;
			continue;
		}
		if (sp)
			*d++ = ' ';
		sp = 0;
		*d++ = *s;
	}
	*d = 0;
}

/* open a disk so the kernel reads it again after a media change (a card
 * swapped in a reader, the Bus Pirate re-inserting its drive) */
static void poke(const char *name)
{
	char dev[24];
	int fd;

	snprintf(dev, sizeof(dev), "/dev/%s", name);
	fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0)
		close(fd);
}

static void disk_add(struct disks *d, const char *name)
{
	struct diskinfo *k;
	char path[64], v[24], m[24];

	if (d->n_disk == MAX_DISKS || strlen(name) >= sizeof(k->name) ||
	    strstr(name, "boot") || strstr(name, "rpmb"))
		return;
	k = &d->disk[d->n_disk++];
	memset(k, 0, sizeof(*k));
	memcpy(k->name, name, strlen(name) + 1);
	k->usb = name[0] == 's';
	snprintf(path, sizeof(path), "/sys/block/%s/size", name);
	k->size = read_ull(path) * 512;
	if (k->usb && !k->size) {
		poke(name);
		k->size = read_ull(path) * 512;
	}
	if (!k->usb) {
		snprintf(k->label, sizeof(k->label), "SD card");
		return;
	}
	snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", name);
	if (read_str(path, v, sizeof(v)) < 0)
		v[0] = 0;
	snprintf(path, sizeof(path), "/sys/block/%s/device/model", name);
	if (read_str(path, m, sizeof(m)) < 0)
		m[0] = 0;
	squeeze(v);
	squeeze(m);
	snprintf(k->label, sizeof(k->label), "%s%s%s", v, v[0] && m[0] ? " " : "", m);
	if (!k->label[0])
		snprintf(k->label, sizeof(k->label), "USB drive");
}

/* the partitions of disk di, or the whole disk when it has no table */
static void parts_add(struct disks *d, int di)
{
	const struct diskinfo *k = &d->disk[di];
	size_t len = strlen(k->name);
	char path[40];
	int first = d->n;

	snprintf(path, sizeof(path), "/sys/block/%s", k->name);
	for (int pass = 0; pass < 2 && d->n == first; pass++) {
		DIR *dir;
		struct dirent *e;

		if (pass) {
			if (!k->size)
				break;
			poke(k->name);
		}
		dir = opendir(path);
		if (!dir)
			return;
		while ((e = readdir(dir)) && d->n < MAX_FS - 3) {
			const char *s = e->d_name + len;
			struct fsinfo *f;
			char sz[80];
			int part = 0;

			if (strncmp(e->d_name, k->name, len))
				continue;
			if (*s == 'p')
				s++;
			if (*s < '1' || *s > '9')
				continue;
			while (*s >= '0' && *s <= '9')
				part = part * 10 + (*s++ - '0');
			if (*s || strlen(e->d_name) + 5 >= sizeof(f->dev))
				continue;
			f = &d->fs[d->n++];
			memset(f, 0, sizeof(*f));
			f->disk = di;
			f->part = part;
			snprintf(f->dev, sizeof(f->dev), "/dev/%s", e->d_name);
			snprintf(sz, sizeof(sz), "%s/%s/size", path, e->d_name);
			f->size = read_ull(sz) * 512;
		}
		closedir(dir);
	}
	for (int i = first + 1; i < d->n; i++)		/* by partition number */
		for (int j = i; j > first && d->fs[j].part < d->fs[j - 1].part; j--) {
			struct fsinfo t = d->fs[j];

			d->fs[j] = d->fs[j - 1];
			d->fs[j - 1] = t;
		}
	if (d->n == first && k->size && d->n < MAX_FS - 3) {	/* no partition table */
		struct fsinfo *f = &d->fs[d->n++];

		memset(f, 0, sizeof(*f));
		f->disk = di;
		snprintf(f->dev, sizeof(f->dev), "/dev/%s", k->name);
		f->size = k->size;
	}
	for (int i = first; i < d->n; i++) {
		struct fsinfo *f = &d->fs[i];

		if (d->n - first > 1)
			snprintf(f->label, sizeof(f->label), "Partition %d", f->part);
		else
			snprintf(f->label, sizeof(f->label), "%s", k->usb ? "USB drive" : "SD card");
		fs_probe(f->dev, f->type, sizeof(f->type));
		fs_usage(f);
	}
}

static int disk_before(const struct diskinfo *a, const struct diskinfo *b)
{
	if (a->usb != b->usb)
		return !a->usb;
	if (strlen(a->name) != strlen(b->name))		/* sdz before sdaa */
		return strlen(a->name) < strlen(b->name);
	return strcmp(a->name, b->name) < 0;
}

/*
 * Reading a disk's boot sector or opening it is cheap on a healthy disk,
 * but a failing SD card answers every sector with a timeout: Drives is
 * read again every 1.5 s, so the disks only when this signature (the block
 * devices, their sizes and partitions, the mounts) changed; else only
 * the usage of what is mounted.
 */
static int disks_sig(char *sig, int max)
{
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	char *p = sig, buf[2048];
	int again = 0;

	*p = 0;
	while (d && (e = readdir(d)) && p < sig + max - 160) {
		char path[96];
		DIR *pd;
		struct dirent *pe;
		unsigned long long size;

		if (strncmp(e->d_name, "mmcblk", 6) && strncmp(e->d_name, "sd", 2))
			continue;
		snprintf(path, sizeof(path), "/sys/block/%s/size", e->d_name);
		size = read_ull(path);
		p += snprintf(p, 64, "%s=%u", e->d_name, (unsigned)(size >> 11));
		/* a USB drive with no medium: opened at every refresh (disk_add) */
		again |= e->d_name[0] == 's' && !size;
		path[strlen(path) - 5] = 0;
		pd = opendir(path);
		while (pd && (pe = readdir(pd)) && p < sig + max - 80)
			if (!strncmp(pe->d_name, e->d_name, strlen(e->d_name)))
				p += snprintf(p, 64, " %s", pe->d_name);
		if (pd)
			closedir(pd);
		*p++ = ';';
		*p = 0;
	}
	if (d)
		closedir(d);
	if (read_str("/proc/mounts", buf, sizeof(buf)) > 0)
		for (char *l = buf; *l && p < sig + max - 2; l++)
			*p++ = *l;
	*p = 0;
	return again;
}

void disks_read(struct disks *d)
{
	static char last[3072];
	char sig[3072];
	DIR *dir;
	struct dirent *e;

	if (!disks_sig(sig, sizeof(sig)) && d->n && !strcmp(sig, last)) {
		for (int i = 0; i < d->n; i++)
			if (d->fs[i].mnt[0])
				fs_usage(&d->fs[i]);
		return;
	}
	memcpy(last, sig, sizeof(last));
	dir = opendir("/sys/block");
	d->n_disk = d->n = 0;
	if (dir) {
		while ((e = readdir(dir)))
			if (!strncmp(e->d_name, "mmcblk", 6) || !strncmp(e->d_name, "sd", 2))
				disk_add(d, e->d_name);
		closedir(dir);
	}
	for (int i = 1; i < d->n_disk; i++)
		for (int j = i; j > 0 && disk_before(&d->disk[j], &d->disk[j - 1]); j--) {
			struct diskinfo t = d->disk[j];

			d->disk[j] = d->disk[j - 1];
			d->disk[j - 1] = t;
		}
	for (int i = 0; i < d->n_disk; i++)
		parts_add(d, i);
	/* the badge's own: flash (/data, with /etc and /root), memory, the image */
	for (int i = 0; i < 3; i++) {
		static const char *const lab[3] = { "Badge storage", "Memory", "System" };
		static const char *const mnt[3] = { "/data", "/tmp", "/" };
		static const char *const type[3] = { "JFFS2", "tmpfs", "squashfs" };
		struct fsinfo *f = &d->fs[d->n++];

		memset(f, 0, sizeof(*f));
		f->disk = -1;
		snprintf(f->label, sizeof(f->label), "%s", lab[i]);
		snprintf(f->mnt, sizeof(f->mnt), "%s", mnt[i]);
		snprintf(f->type, sizeof(f->type), "%s", type[i]);
		fs_usage(f);
	}
}

/* mount as S15sdcard does: vfat, else exfat, else f2fs; read-only when the
 * drive is write-protected. 0 read-write, 1 read-only, -1 none of those */
static int fs_mount(const struct disks *d, const struct fsinfo *f)
{
	char mnt[32];

	mnt_point(d, f, mnt, sizeof(mnt));
	mkdir(mnt, 0755);
	for (int ro = 0; ro < 2; ro++) {
		unsigned long fl = MS_NOATIME | (ro ? MS_RDONLY : 0);

		if (mount(f->dev, mnt, "vfat", fl, "utf8=1,flush") == 0 ||
		    mount(f->dev, mnt, "exfat", fl, "") == 0 ||
		    mount(f->dev, mnt, "f2fs", fl, "nodiscard") == 0)
			return ro;
	}
	if (strcmp(mnt, SD_MNT))
		rmdir(mnt);
	return -1;
}

/* the store finishes its work for a card that comes back (WhyOS-Apps
 * docs/device.md §5.7); in the background */
static void store_maintain(void)
{
	static char *const argv[] = { "/data/apps/store/store", "maintain", 0 };
	pid_t pid;

	if (access(argv[0], X_OK))
		return;
	pid = vfork();
	if (pid == 0) {
		int fd = open("/tmp/store-maintain.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execv(argv[0], argv);
		_exit(127);
	}
}

int disks_open(struct disks *d, int i, char *notice, size_t n)
{
	struct fsinfo *f = &d->fs[i];
	const struct diskinfo *dk = fs_disk(d, f);
	char dev[sizeof(f->dev)];

	if (f->mnt[0])
		return i;
	memcpy(dev, f->dev, sizeof(dev));
	if (sd_lent(f)) {
		snprintf(notice, n, "The computer is using the SD card");
		return -1;
	}
	if (fs_mount(d, f) == 0 && dk && !dk->usb)
		store_maintain();
	d->n = 0;		/* read again, signature or not */
	disks_read(d);
	for (int k = 0; k < d->n; k++)
		if (!strcmp(d->fs[k].dev, dev) && d->fs[k].mnt[0])
			return k;
	snprintf(notice, n, "Could not mount it: not FAT, exFAT or f2fs");
	return -1;
}

static int mounted_at(const char *mnt)
{
	char buf[2048], dev[48], m[48], *line = buf;

	if (read_str("/proc/mounts", buf, sizeof(buf)) <= 0)
		return 0;
	while ((line = mounts_next(line, dev, m, sizeof(dev))))
		if (!strcmp(m, mnt))
			return 1;
	return 0;
}

void disks_fat_follow(int files)
{
	struct disks d;
	struct fsinfo f;
	char buf[192];

	if (!files) {
		/* one mounted in Disks stays; still in use (a shell in it): the
		 * menu tries again (its sd_fat_follow) */
		if (access(SD_FAT_KEEP, F_OK) && mounted_at(SD_FAT_MNT) && umount(SD_FAT_MNT) == 0)
			rmdir(SD_FAT_MNT);
		return;
	}
	if ((read_str("/var/run/usb.state", buf, sizeof(buf)) > 0 && strstr(buf, "disk=1")) ||
	    mounted_at(SD_FAT_MNT) || !sd_badge())
		return;
	memset(&d, 0, sizeof(d));
	memset(&f, 0, sizeof(f));
	f.disk = -1;
	snprintf(f.dev, sizeof(f.dev), SD_FAT);
	fs_mount(&d, &f);
}

#endif
