/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2011 Geza Kovacs - based on syslinux for Linux
 *   Copyright 1998-2008 H. Peter Anvin - All Rights Reserved
 *   Copyright 2009-2010 Intel Corporation; author: H. Peter Anvin
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 53 Temple Place Ste 330,
 *   Boston MA 02111-1307, USA; either version 2 of the License, or
 *   (at your option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/*
 * syslinux.c - Linux installer program for SYSLINUX
 *
 * This is Linux-specific by now.
 *
 * This is an alternate version of the installer which doesn't require
 * mtools, but requires root privilege.
 */

/*
 * If DO_DIRECT_MOUNT is 0, call mount(8)
 * If DO_DIRECT_MOUNT is 1, call mount(2)
 */
#ifdef __KLIBC__
# define DO_DIRECT_MOUNT 1
#else
# define DO_DIRECT_MOUNT 0	/* glibc has broken losetup ioctls */
#endif

#define _GNU_SOURCE
#define _XOPEN_SOURCE 500	/* For pread() pwrite() */
#define _FILE_OFFSET_BITS 64
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/param.h>
//#include <sys/mount.h>
#include <sys/statvfs.h>

//#include "linuxioctl.h"
#include <sys/ioctl.h>

#define __u32 uint32_t

#include <paths.h>
#ifndef _PATH_MOUNT
# define _PATH_MOUNT "/bin/mount"
#endif
#ifndef _PATH_UMOUNT
# define _PATH_UMOUNT "/bin/umount"
#endif
#ifndef _PATH_TMP
# define _PATH_TMP "/tmp/"
#endif

#include "syslinux.h"

#if DO_DIRECT_MOUNT
# include <linux/loop.h>
#endif

#include <getopt.h>
#include <sysexits.h>
#include "syslxcom.h"
#include "setadv.h"
#include "syslxopt.h" /* unified options */
#include "libfat.h"
#include "fat.h"

extern const char *program;	/* Name of program */

pid_t mypid;
char *mntpath = NULL;		/* Path on which to mount */

#if DO_DIRECT_MOUNT
int loop_fd = -1;		/* Loop device */
#endif

void __attribute__ ((noreturn)) die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", program, msg);
    exit(1);
}

/*
 * Modify the ADV of an existing installation
 */
int modify_existing_adv(const char *path)
{
    if (opt.reset_adv)
	syslinux_reset_adv(syslinux_adv);
    else if (read_adv(path, "ldlinux.sys") < 0)
	return 1;

    if (modify_adv() < 0)
	return 1;

    if (write_adv(path, "ldlinux.sys") < 0)
	return 1;

    return 0;
}

/*
 * Wrapper for ReadFile suitable for libfat
 */
int libfat_readfile(intptr_t pp, void *buf, size_t secsize,
		    libfat_sector_t sector)
{
    uint64_t offset = (uint64_t) sector * secsize;
    /*LONG loword = (LONG) offset;
    LONG hiword = (LONG) (offset >> 32);
    LONG hiwordx = hiword;
    DWORD bytes_read;

    if (SetFilePointer((HANDLE) pp, loword, &hiwordx, FILE_BEGIN) != loword ||
	hiword != hiwordx ||
	!ReadFile((HANDLE) pp, buf, secsize, &bytes_read, NULL) ||
	bytes_read != secsize) {
	fprintf(stderr, "Cannot read sector %u\n", sector);
	exit(1);
    }*/
    uint64_t filepos = lseek(pp, 0, SEEK_CUR);
    lseek(pp, offset, SEEK_SET);
    
    int numread = read(pp, buf, secsize);
    lseek(pp, filepos, SEEK_SET);
    if (numread != secsize) {
    printf("Cannot read sector %u\n", sector);
    }
    // TODO
    return numread;
    //return secsize;
}

int main(int argc, char *argv[])
{
    static unsigned char sectbuf[SECTOR_SIZE];
    int dev_fd, fd;
    struct stat st;
    int err = 0;
    char mntname[128];
    char *ldlinux_name;
    char *ldlinux_path;
    char *subdir;
    //sector_t *sectors = NULL;
    libfat_sector_t *sectors = NULL;
    int ldlinux_sectors = (boot_image_len + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
    const char *errmsg;
    int mnt_cookie;
    int patch_sectors;
    int i;
    
    printf("syslinux for Mac OS X; created by Geza Kovacs for UNetbootin unetbootin.sf.net\n");

    mypid = getpid();
    umask(077);
    parse_options(argc, argv, MODE_SYSLINUX);

    /* Note: subdir is guaranteed to start and end in / */
    if (opt.directory && opt.directory[0]) {
	int len = strlen(opt.directory);
	int rv = asprintf(&subdir, "%s%s%s",
			  opt.directory[0] == '/' ? "" : "/",
			  opt.directory,
			  opt.directory[len-1] == '/' ? "" : "/");
	if (rv < 0 || !subdir) {
	    perror(program);
	    exit(1);
	}
    } else {
	subdir = "/";
    }

    if (!opt.device || opt.install_mbr || opt.activate_partition)
	usage(EX_USAGE, MODE_SYSLINUX);

    /*
     * First make sure we can open the device at all, and that we have
     * read/write permission.
     */

    if (geteuid()) {
	die("This program needs root privilege");
    }
    char umountCommand[4096];
    memset(umountCommand, 0, 4096);
    strcat(umountCommand, "hdiutil unmount ");
    strcat(umountCommand, opt.device);
    system(umountCommand);
    
    dev_fd = open(opt.device, O_RDWR);
    if (dev_fd < 0 || fstat(dev_fd, &st) < 0) {
	perror("couldn't open device");
	exit(1);
    }

    if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode)) {
	die("not a device or regular file");
    }

    if (opt.offset && S_ISBLK(st.st_mode)) {
	die("can't combine an offset with a block device");
    }

    fs_type = VFAT;
    xpread(dev_fd, sectbuf, SECTOR_SIZE, opt.offset);
    fsync(dev_fd);
    close(dev_fd);

    /*
     * Check to see that what we got was indeed an MS-DOS boot sector/superblock
     */
    if ((errmsg = syslinux_check_bootsect(sectbuf))) {
	fprintf(stderr, "%s: %s\n", opt.device, errmsg);
	exit(1);
    }

    /*
     * Now mount the device.
     */
    if (geteuid()) {
	die("This program needs root privilege");
    }
#if 0
    else {
	int i = 0;
	struct stat dst;
	int rv;

	/* We're root or at least setuid.
	   Make a temp dir and pass all the gunky options to mount. */

	if (chdir(_PATH_TMP)) {
	    fprintf(stderr, "%s: Cannot access the %s directory.\n",
		    program, _PATH_TMP);
	    exit(1);
	}
#define TMP_MODE (S_IXUSR|S_IWUSR|S_IXGRP|S_IWGRP|S_IWOTH|S_IXOTH|S_ISVTX)

	if (stat(".", &dst) || !S_ISDIR(dst.st_mode) ||
	    (dst.st_mode & TMP_MODE) != TMP_MODE) {
	    die("possibly unsafe " _PATH_TMP " permissions");
	}

	for (i = 0;; i++) {
	    snprintf(mntname, sizeof mntname, "syslinux.mnt.%lu.%d",
		     (unsigned long)mypid, i);

	    if (lstat(mntname, &dst) != -1 || errno != ENOENT)
		continue;

	    rv = mkdir(mntname, 0000);

	    if (rv == -1) {
		if (errno == EEXIST || errno == EINTR)
		    continue;
		perror(program);
		exit(1);
	    }

	    if (lstat(mntname, &dst) || dst.st_mode != (S_IFDIR | 0000) ||
		dst.st_uid != 0) {
		die("someone is trying to symlink race us!");
	    }
	    break;		/* OK, got something... */
	}

	mntpath = mntname;
    }

    if (do_mount(dev_fd, &mnt_cookie, mntpath, "vfat") &&
	do_mount(dev_fd, &mnt_cookie, mntpath, "msdos")) {
	rmdir(mntpath);
	die("mount failed");
    }

#endif

    char mountCmd[4096];
    memset(mountCmd, 0, 4096);
    strcat(mountCmd, "hdiutil mount ");
    strcat(mountCmd, opt.device);
    system(mountCmd);
    
    char mountGrepCmd[4096];
    memset(mountGrepCmd, 0, 4096);
    strcat(mountGrepCmd, "echo `mount | grep ");
    strcat(mountGrepCmd, opt.device);
    strcat(mountGrepCmd, " | sed 's/ on /$/' | tr '$' '\n' | head -n 2 | tail -n 1 | tr '(' '\n' | head -n 1`");
    char mountPoint[4096];
    memset(mountPoint, 0, 4096);
    FILE *mountCmdOutput = popen(mountGrepCmd, "r");
    fgets(mountPoint, 4096, mountCmdOutput);
    mountPoint[strlen(mountPoint)-1] = 0; // removes trailing newline
    printf("mountpoint is %s\n", mountPoint);
    
    mntpath = mountPoint;
    ldlinux_path = alloca(strlen(mntpath) + strlen(subdir) + 1);
    sprintf(ldlinux_path, "%s%s", mntpath, subdir);

    ldlinux_name = alloca(strlen(ldlinux_path) + 14);
    if (!ldlinux_name) {
	perror(program);
	err = 1;
	goto umount;
    }
    sprintf(ldlinux_name, "%sldlinux.sys", ldlinux_path);

    /* update ADV only ? */
    if (opt.update_only == -1) {
	if (opt.reset_adv || opt.set_once) {
	    modify_existing_adv(ldlinux_path);
	    //do_umount(mntpath, mnt_cookie);
	    sync();
	    //rmdir(mntpath);
	    exit(0);
	} else {
	    fprintf(stderr, "%s: please specify --install or --update for the future\n", argv[0]);
	    opt.update_only = 0;
	}
    }

    printf("checkpoint1\n");

    /* Read a pre-existing ADV, if already installed */
    if (opt.reset_adv)
	syslinux_reset_adv(syslinux_adv);
    else if (read_adv(ldlinux_path, "ldlinux.sys") < 0)
	syslinux_reset_adv(syslinux_adv);
    if (modify_adv() < 0)
	exit(1);
	
	printf("checkpoint1.5\n");

    if ((fd = open(ldlinux_name, O_RDONLY)) >= 0) {
	uint32_t zero_attr = 0;
	//ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &zero_attr);
	printf("checkpoint1.55bad\n");
	close(fd);
    }
    
    printf("checkpoint1.6\n");

    unlink(ldlinux_name);
    printf(ldlinux_name);
    printf(" ldlinuxname\n");
    fd = open(ldlinux_name, O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd < 0) {
	perror(opt.device);
	err = 1;
	goto umount;
    }
    
    printf("checkpoint2\n");

    /* Write it the first time */
    if (xpwrite(fd, boot_image, boot_image_len, 0) != (int)boot_image_len ||
	xpwrite(fd, syslinux_adv, 2 * ADV_SIZE,
		boot_image_len) != 2 * ADV_SIZE) {
	fprintf(stderr, "%s: write failure on %s\n", program, ldlinux_name);
	exit(1);
    }

    fsync(fd);
    /*
     * Set the attributes
     */
    {
	uint32_t attr = 0x07;	/* Hidden+System+Readonly */
	//ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attr);
    }
    
    printf("checkpoint3\n");

    /*
     * Create a block map.
     */
    
    ldlinux_sectors += 2; /* 2 ADV sectors */
    
    close(fd);
    sync();
    /*
    sectors = calloc(ldlinux_sectors, sizeof *sectors);
    if (sectmap(fd, sectors, ldlinux_sectors)) {
	perror("bmap");
	exit(1);
    }
    close(fd);
    sync();
    printf("checkpoint4\n");
    */
    
    /* Map the file (is there a better way to do this?) */
    //ldlinux_sectors = (syslinux_ldlinux_len + 2 * ADV_SIZE + SECTOR_SIZE - 1) >> SECTOR_SHIFT;
    struct libfat_filesystem *fs;
    libfat_sector_t s;
    libfat_sector_t *secp;
    //libfat_sector_t *sectors;
    uint32_t ldlinux_cluster;
    
    sectors = calloc(ldlinux_sectors, sizeof *sectors);
    //fs = libfat_open(libfat_readfile, (intptr_t) d_handle);
    system(umountCommand);
    dev_fd = open(opt.device, O_RDWR);
    if (dev_fd < 0 || fstat(dev_fd, &st) < 0) {
	perror("couldn't open device");
	exit(1);
    }
    fs = libfat_open(libfat_readfile, dev_fd);
    if (fs == NULL) printf("null fs struct\n");
    ldlinux_cluster = libfat_searchdir(fs, 0, "LDLINUX SYS", NULL);
    
    printf("checkpoint4\n");
    
    secp = sectors;
    int nsectors;
    nsectors  = 0;
    s = libfat_clustertosector(fs, ldlinux_cluster);
    while (s && nsectors < ldlinux_sectors) {
	*secp++ = s;
	nsectors++;
	s = libfat_nextsector(fs, s);
    }
    libfat_close(fs);
    printf("checkpoint5\n");

umount:
    //do_umount(mntpath, mnt_cookie);
    sync();
    //rmdir(mntpath);

    if (err)
	exit(err);
	
	printf("checkpoint6\n");

    /*
     * Patch ldlinux.sys and the boot sector
     */
    i = syslinux_patch(sectors, ldlinux_sectors, opt.stupid_mode,
		       opt.raid_mode, subdir, NULL);
    patch_sectors = (i + SECTOR_SIZE - 1) >> SECTOR_SHIFT;

    /*
     * Write the now-patched first sectors of ldlinux.sys
     */
    for (i = 0; i < patch_sectors; i++) {
	xpwrite(dev_fd, boot_image + i * SECTOR_SIZE, SECTOR_SIZE,
		opt.offset + ((off_t) sectors[i] << SECTOR_SHIFT));
    }
    
    printf("checkpoint7\n");

    /*
     * To finish up, write the boot sector
     */

    /* Read the superblock again since it might have changed while mounted */
    xpread(dev_fd, sectbuf, SECTOR_SIZE, opt.offset);

    /* Copy the syslinux code into the boot sector */
    syslinux_make_bootsect(sectbuf);

    /* Write new boot sector */
    xpwrite(dev_fd, sectbuf, SECTOR_SIZE, opt.offset);
    
    printf("checkpoint8\n");

    close(dev_fd);
    sync();
    system(mountCmd);

    /* Done! */

    return 0;
}
