/* SPDX-License-Identifier: MIT */
/* disks.c: sizes, /proc/mounts, names and which drive a folder is on. */
#include "disks.h"
#include "t.h"

static void fixture(struct disks *d)
{
	static const struct fsinfo fs[] = {
		{ "Partition 1", "/dev/mmcblk0p1", "/media/sd", "FAT32", 1ULL << 30, 1ULL << 20, 0, 1 },
		{ "Partition 2", "/dev/mmcblk0p2", "/mnt/sd", "f2fs", 256ULL << 20, 10ULL << 20, 0, 2 },
		{ "USB drive", "/dev/sda1", "", "exFAT", 64ULL << 30, 0, 1, 1 },
		{ "Badge storage", "", "/data", "JFFS2", 4480ULL << 10, 1000ULL << 10, -1, 0 },
		{ "Memory", "", "/tmp", "tmpfs", 13ULL << 20, 3ULL << 20, -1, 0 },
		{ "System", "", "/", "squashfs", 4ULL << 20, 4ULL << 20, -1, 0 },
	};

	memset(d, 0, sizeof(*d));
	d->n_disk = 2;
	strcpy(d->disk[0].name, "mmcblk0");
	strcpy(d->disk[0].label, "SD card");
	d->disk[0].size = 16ULL << 30;
	strcpy(d->disk[1].name, "sda");
	strcpy(d->disk[1].label, "SanDisk Ultra");
	d->disk[1].usb = 1;
	d->disk[1].size = 64ULL << 30;
	d->n = 6;
	memcpy(d->fs, fs, sizeof(fs));
}

TEST(sizes_for_people)
{
	char b[24];

	human(b, 1830);
	STREQ(b, "1.7 KB");
	human(b, 3874133);
	STREQ(b, "3.6 MB");
	human(b, 512ULL << 20);
	STREQ(b, "512 MB");
	human(b, 999);
	STREQ(b, "999 B");
	human(b, 16ULL << 30);
	STREQ(b, "16 GB");
	OK(*human(b, 0) == 0);
	STREQ(b, "0 B");
}

TEST(reads_proc_mounts)
{
	char text[] = "/dev/root / squashfs ro 0 0\n/dev/mmcblk0p2 /mnt/sd f2fs rw 0 0\n", *l = text;
	char dev[48], mnt[48];

	OK((l = mounts_next(l, dev, mnt, sizeof(dev))));
	STREQ(dev, "/dev/root");
	STREQ(mnt, "/");
	OK((l = mounts_next(l, dev, mnt, sizeof(dev))));
	STREQ(mnt, "/mnt/sd");
	OK(!mounts_next(l, dev, mnt, sizeof(dev)));
}

TEST(names_and_where_a_folder_is)
{
	static struct disks d;
	char name[64];

	fixture(&d);
	fs_name(&d, &d.fs[0], name, sizeof(name));
	STREQ(name, "SD card, Partition 1");		/* two partitions */
	fs_name(&d, &d.fs[2], name, sizeof(name));
	STREQ(name, "SanDisk Ultra");
	fs_name(&d, &d.fs[3], name, sizeof(name));
	STREQ(name, "Badge storage");
	STREQ(disks_fs_of(&d, "/mnt/sd/badge/apps")->label, "Partition 2");
	STREQ(disks_fs_of(&d, "/mnt/sdx")->label, "System");	/* not under /mnt/sd */
	STREQ(disks_fs_of(&d, "/root")->label, "System");
	STREQ(disks_fs_of(&d, "/data")->label, "Badge storage");
	OK(disks_mount_at(&d, "/media/sd") == &d.fs[0]);
	OK(disks_mount_at(&d, "/media") == 0);
}
