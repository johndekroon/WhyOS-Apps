/* SPDX-License-Identifier: MIT */
/*
 * Files: the drives on the Drives screen and mounting them (disks.c), as
 * the menu's Settings > Storage & devices > Disks sees them (why2025-linux
 * home.c, "disks"; docs/DATA.md): the SD card and USB drives by partition,
 * then the badge's own (Badge storage /data, Memory /tmp, System /).
 *
 * The SD card in the badge layout has a FAT for computers (partition 1)
 * and f2fs for the badge (partition 2, /mnt/sd). The FAT is mounted at
 * /media/sd only while Files is open (disks_fat_follow), so that it is
 * seldom mounted when the badge resets; one mounted in Disks stays.
 *
 * On the host (FILES_HOST) disks_read gives a fixed set for the screen
 * tests and nothing mounts.
 */
#ifndef FILES_DISKS_H
#define FILES_DISKS_H

#include <stddef.h>

#define MAX_DISKS	6
#define MAX_FS		15

#define SD_MNT		"/mnt/sd"
#define USB_MNT		"/media/"	/* + sda1; a tmpfs (fstab), as / is read-only */
#define SD_FAT		"/dev/mmcblk0p1"
#define SD_F2FS		"/dev/mmcblk0p2"
#define SD_FAT_MNT	"/media/sd"	/* the badge layout's FAT, while Files is open */
#define SD_FAT_KEEP	"/var/run/sdfat.keep"	/* mounted in Disks: Files leaves it */

/* A removable disk: the SD card or a USB drive */
struct diskinfo {
	char name[12];		/* mmcblk0, sda */
	char label[40];		/* "SD card", or the drive's vendor and model */
	int usb;
	unsigned long long size;	/* 0: no medium (empty card reader) */
};

struct fsinfo {
	char label[32];		/* "SD card", "USB drive", "Partition 2", "Badge storage" */
	char dev[24];		/* /dev/mmcblk0p1, /dev/sda1; "" for the badge's own */
	char mnt[32];		/* mount point, "" if not mounted */
	char type[12];		/* FAT32, exFAT, f2fs, JFFS2, ... */
	unsigned long long size, used;	/* bytes; used only when mounted */
	int disk;		/* index into disks.disk; -1 for the badge's own */
	int part;		/* partition number, 0 = the whole disk */
};

struct disks {
	int n_disk;
	struct diskinfo disk[MAX_DISKS];	/* the SD card first */
	int n;
	struct fsinfo fs[MAX_FS];	/* by disk, then partition; the badge's last */
};

/* "15.7 GB", "512 MB", "980 KB" into p (24 bytes); the end of it */
char *human(char *p, unsigned long long b);
/* the next "device mountpoint" of /proc/mounts text; 0 at the end */
char *mounts_next(char *line, char *dev, char *mnt, int max);

void disks_read(struct disks *d);
const struct diskinfo *fs_disk(const struct disks *d, const struct fsinfo *f);
/* a file system's name: its disk's ("SD card", "SanDisk Ultra"), with the
 * partition when the disk has more than one; into out (size n) */
char *fs_name(const struct disks *d, const struct fsinfo *f, char *out, size_t n);
/* the mounted file system dir is in (the longest mount point), or 0 */
const struct fsinfo *disks_fs_of(const struct disks *d, const char *dir);
/* the mounted file system at path, if path is its mount point */
const struct fsinfo *disks_mount_at(const struct disks *d, const char *path);
/* Enter on drive i: mounted already, or mount it (and let the store
 * finish its work for a card that came back). The drive's index after
 * reading the drives again, its mnt where it is; -1 with why in notice */
int disks_open(struct disks *d, int i, char *notice, size_t n);
/* the FAT follows Files: files 1 when Files opens, 0 when it closes */
void disks_fat_follow(int files);

#endif
