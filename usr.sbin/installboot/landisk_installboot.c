/*	$OpenBSD: landisk_installboot.c,v 1.11 2022/08/31 18:46:06 miod Exp $	*/

/*
 * Copyright (c) 2013 Joel Sing <jsing@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>	/* DEV_BSIZE */
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <ufs/ffs/fs.h>

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "installboot.h"

void	md_bootstrap(int, char *, char *);

char	*bootldr;

void
md_init(void)
{
	stages = 2;
	stage1 = "/usr/mdec/xxboot";
	stage2 = "/usr/mdec/boot";

	bootldr = "/boot";
}

void
md_loadboot(void)
{
}

void
md_prepareboot(int devfd, char *dev)
{
}

void
md_installboot(int devfd, char *dev)
{
	/* XXX - is this necessary? */
	sync();

	bootldr = fileprefix(root, bootldr);
	if (bootldr == NULL)
		exit(1);
	if (!nowrite)
		if (filecopy(stage2, bootldr) == -1)
			exit(1);

	/*
	 * Write bootblock into the beggining of the OpenBSD partition or
	 * at the beginning of the disk.
	 */
	md_bootstrap(devfd, dev, stage1);
}

void
md_bootstrap(int devfd, char *dev, char *bootfile)
{
	struct disklabel dl;
	struct disklabel *lp;
	struct partition *pp;
	char *boot, *p, part;
	size_t bootsize;
	size_t bootsec;
	struct stat sb;
	daddr_t bootpos = 0;
	int fd, i;

	/*
	 * Install bootstrap code onto the given disk, preserving the
	 * existing disklabel.
	 */

	/* Read disklabel from disk. */
	if (ioctl(devfd, DIOCGDINFO, &dl) == -1)
		err(1, "disklabel");
	if (dl.d_secsize == 0) {
		warnx("disklabel has sector size of 0, assuming %d", DEV_BSIZE);
		dl.d_secsize = DEV_BSIZE;
	}

	/* Read bootstrap file. */
	if (verbose)
		fprintf(stderr, "reading bootstrap from %s\n", bootfile);
	fd = open(bootfile, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", bootfile);
	if (fstat(fd, &sb) == -1)
		err(1, "fstat %s", bootfile);
	bootsec = howmany((ssize_t)sb.st_size, dl.d_secsize);
	bootsize = bootsec * dl.d_secsize;
	if (verbose)
		fprintf(stderr, "bootstrap is %zu bytes "
		    "(%zu sectors @ %u bytes = %zu bytes)\n",
		    (ssize_t)sb.st_size, bootsec, dl.d_secsize, bootsize);
	boot = calloc(1, bootsize);
	if (boot == NULL)
		err(1, "calloc");
	if (read(fd, boot, bootsize) != (ssize_t)sb.st_size)
		err(1, "read");
	close(fd);

	/*
	 * The landisk bootstrap can work when put either at the start of the
	 * disk, or at the start of an OpenBSD MBR partition, invoked from
	 * /usr/mdec/mbr.
	 * Check for a partition table in order to decide where to put the
	 * first-level bootstrap code.
	 */

	if (dl.d_secsize >= sizeof(struct dos_mbr)) {
		uint8_t *secbuf;
		struct dos_mbr *mbr;

		secbuf = malloc(dl.d_secsize);
		if (secbuf == NULL)
			err(1, "malloc");

		/* Read MBR. */
		if (pread(devfd, secbuf, dl.d_secsize, 0) != dl.d_secsize)
			err(4, "can't read mbr");
		mbr = (struct dos_mbr *)secbuf;
		/* safe check because landisk is little endian */
		if (mbr->dmbr_sign == DOSMBR_SIGNATURE) {
			for (i = 0; i < NDOSPART; i++) {
				if (mbr->dmbr_parts[i].dp_typ ==
				    DOSPTYP_OPENBSD) {
					bootpos = mbr->dmbr_parts[i].dp_start;
					break;
				}
			}
		}

		free(secbuf);
	}

	if (bootpos == 0) {
		/*
		 * Installing at the start of the disk.
		 * Check that the bootstrap will fit - partitions must not
		 * overlap, or if they do, the partition type must be either
		 * FS_BOOT or FS_UNUSED. The 'c' partition will always overlap
		 * and is ignored.
		 */
		if (verbose)
			fprintf(stderr,
			    "ensuring used partitions do not overlap "
			    "with bootstrap sectors 0-%zu\n", bootsec);
		for (i = 0; i < dl.d_npartitions; i++) {
			part = 'a' + i;
			pp = &dl.d_partitions[i];
			if (i == RAW_PART)
				continue;
			if (DL_GETPSIZE(pp) == 0)
				continue;
			if (bootpos + (u_int64_t)bootsec <= DL_GETPOFFSET(pp))
				continue;
			switch (pp->p_fstype) {
			case FS_BOOT:
				break;
			case FS_UNUSED:
				warnx("bootstrap overlaps "
				    "with unused partition %c", part);
				break;
			default:
				errx(1, "bootstrap overlaps with partition %c",
				    part);
			}
		}
	} else {
		/*
		 * Installing at the start of the OpenBSD partition.
		 * We only need to ensure the bootstrap code fits in the
		 * BBSIZE reserved area at the beginning of the file
		 * system.
		 */
		if (bootsize > BBSIZE)
			errx(1, "bootstrap is too large");
	}

	/*
	 * Make sure the bootstrap has left space for the disklabel.
	 * N.B.: LABELSECTOR *is* a DEV_BSIZE quantity!
	 */
	lp = (struct disklabel *)(boot + (LABELSECTOR * DEV_BSIZE) +
	    LABELOFFSET);
	for (i = 0, p = (char *)lp; i < (int)sizeof(*lp); i++)
		if (p[i] != 0)
			errx(1, "bootstrap has data in disklabel area");

	/* Patch the disklabel into the bootstrap code. */
	memcpy(lp, &dl, sizeof(dl));

	/* Write the bootstrap out to the disk. */
	bootpos *= dl.d_secsize;
	if (verbose)
		fprintf(stderr, "%s bootstrap to disk at offset %llx\n",
		    (nowrite ? "would write" : "writing"), bootpos);
	if (nowrite)
		return;
	if (pwrite(devfd, boot, bootsize, bootpos) != (ssize_t)bootsize)
		err(1, "pwrite");
}
