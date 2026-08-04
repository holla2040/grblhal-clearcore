/*
 * ffconf.h — FatFs R0.15a configuration for grblhal-clearcore.
 *
 * Every value below is chosen to satisfy the grblHAL sdcard plugin
 * (src/sdcard/fs_fatfs.c, sdcard.c, fs_stream.c) on a single-threaded
 * bare-metal target with one removable microSD volume and no RTC.
 * Deviations from the FatFs defaults are commented with the reason.
 *
 * Vendored FatFs files: ff.c, ff.h, ffunicode.c, diskio.h (ChaN, R0.15a).
 * Not vendored: ffsystem.c (only needed for FF_FS_REENTRANT / FF_USE_LFN 3)
 * and diskio.c (replaced by our own SD-over-SPI implementation).
 */

#define FFCONF_DEF	5380	/* Revision ID — must match FF_DEFINED in ff.h */

/*
 * fs_fatfs.h only auto-defines NEW_FATFS for ESP32/STM32/LPC17xx/IMXRT/MSP432.
 * Without it the plugin compiles the legacy f_mount(id, fs) branch, which
 * references fatfs_dev_t.id — a member that does not exist in the legacy
 * struct (it has .drive), so that branch cannot compile at all. Defining it
 * here works because ff.h (and therefore this file) is included before
 * fs_fatfs.h in both sdcard.c and fs_fatfs.c. Selects the modern
 * f_mount(fs, path, opt) / f_unmount(path) signatures that R0.15a provides.
 */
#ifndef NEW_FATFS
#define NEW_FATFS
#endif

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* 0: Read/write. The plugin writes: fs_write/fs_truncate/fs_mkdir/fs_unlink,
/  ymodem uploads and $F-family file commands all need it. fs_getfree()
/  returns false outright when this is 1. */

#define FF_FS_MINIMIZE	0
/* 0: All basic API present. f_stat/f_opendir/f_readdir/f_unlink/f_mkdir/
/  f_rename/f_truncate are all called by fs_fatfs.c, which requires level 0. */

#define FF_USE_FIND		0
/* f_findfirst/f_findnext are not used by the plugin. */

#define FF_USE_MKFS		0
/*
 * MUST stay 0 with R0.15a. fs_fatfs.c picks its f_mkfs() call by
 * `#if FF_DEFINED >= 86631`; R0.15a reports FF_DEFINED 5380 (the revision-ID
 * scheme changed after R0.14b, which was 86606), so the plugin takes the
 * legacy 5-argument f_mkfs(path, opt, au, work, len) branch while R0.15a
 * declares the 4-argument f_mkfs(path, opt, work, len) — a hard compile
 * error. Consequence: the plugin's `.format` and `.device_mount` vfs entries
 * are not registered (they share the same #if block). grbl/vfs.c null-checks
 * device_mount, and sdcard.c mounts/unmounts via f_mount/f_unmount directly,
 * so nothing else is lost — cards must be formatted on a PC.
 */

#define FF_USE_FASTSEEK	0
/* Job streaming is sequential; the cluster-link map would cost RAM per file. */

#define FF_USE_EXPAND	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
/* f_expand/f_getlabel/f_setlabel/f_forward are not used by the plugin. */

#define FF_USE_CHMOD	1
/* 1: enables f_chmod + f_utime. fs_fatfs.c gates both its `.fchmod` vfs entry
/  and the body of fs_utime() on `FF_FS_READONLY == 0 && FF_USE_CHMOD == 1`;
/  with 0 those degrade to a permanent -1. */

#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0
/* The plugin reads g-code through f_read(), never f_gets/f_puts/f_printf.
/  grbl's own stream layer does the line splitting. */

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* U.S. OEM code page. Single-byte, so ffunicode.c links only the 437 table
/  (~340 bytes) instead of the ~30 KB DBCS tables the 932 default pulls in. */

#define FF_USE_LFN		1
#define FF_MAX_LFN		255
/*
 * 1: LFN enabled with a static working buffer in BSS (~608 bytes: a
 * WCHAR[256] LFN buffer plus FatFs' own scratch). Not re-entrant, which is
 * fine — FatFs and the SPI bus are touched only from the grbl foreground
 * loop, never from an ISR. Option 3 (heap) is unavailable because it needs
 * ff_memalloc/ff_memfree from ffsystem.c, which we do not vendor.
 * 255 matches vfs_dirent_t.name[255] in grbl/vfs.h; fs_readdir() skips any
 * entry that would not fit, so a shorter limit would silently hide files.
 */

#define FF_LFN_UNICODE	0
/* 0: ANSI/OEM, so TCHAR == char. Required — fs_fatfs.c does
/  strcpy(dirent->name, fi.fname) and passes plain `const char *` paths
/  straight into f_open/f_stat/f_opendir. */

#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
/* FILINFO.fname / .altname sizes; only meaningful when FF_USE_LFN != 0. */

#define FF_FS_RPATH		2
/*
 * 2: relative paths plus f_getcwd. fs_fatfs.c compiles fs_chdir()/fs_getcwd()
 * and registers `.fchdir`/`.fgetcwd` under `#if FF_FS_RPATH`, and fs_getcwd()
 * calls f_getcwd() — which only exists at level 2. Level 1 would compile the
 * registration but leave f_getcwd undefined at link time.
 */

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES		1
/* One physical drive: the microSD on SERCOM4 (pdrv 0). */

#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"SD"
/*
 * 0: numeric volume IDs only. sdcard.c mounts with f_mount(fs, device.name, 1)
 * where fatfs_dev_t is zero-initialised and device.name stays "" unless a
 * driver supplies sdcard_events_t.on_mount (we do not). An empty path selects
 * the default drive 0, so string volume IDs buy nothing here. FF_VOLUME_STRS
 * is left defined but unused so raising FF_STR_VOLUME_ID later just works.
 */

#define FF_MULTI_PARTITION	0
/* Each physical drive is one volume; f_fdisk/partition tables not used. */

#define FF_MIN_SS		512
#define FF_MAX_SS		512
/*
 * Fixed 512-byte sectors. Equal values tell FatFs the sector size is a
 * compile-time constant, so our diskio.c need not implement GET_SECTOR_SIZE.
 * Our disk_read/disk_write force 512-byte blocks (CMD16 on non-SDHC cards).
 * fs_getfree() in fs_fatfs.c also hardcodes `<< 9` for the sector size.
 */

#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
/* 32-bit LBA: addresses up to 2 TB, far beyond any card this board takes. */

#define FF_USE_TRIM		0
/* Would require a CTRL_TRIM case in disk_ioctl (ACMD/CMD38 erase). */

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0
/*
 * 0: each FIL carries its own 512-byte data buffer. Costs ~512 bytes per open
 * file (grbl keeps at most a job file plus a macro file open) and avoids the
 * read-back-the-sector penalty FF_FS_TINY pays on every interleaved access,
 * which on a polled SPI bus would show up directly as g-code feed stalls.
 */

#define FF_FS_EXFAT		1
/*
 * 0: FAT12/16/32 only, which is what SD/SDHC cards up to 32 GB ship with.
 * exFAT would add ~10 KB of flash and carries Microsoft patent terms; if
 * SDXC support is ever wanted, set this and FF_LBA64 to 1 (exFAT also
 * requires FF_USE_LFN != 0, already satisfied).
 */

#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
/*
 * 1: no RTC on this board, so FatFs stamps every created/modified file with
 * the fixed date above instead of calling get_fattime() — which means we do
 * not have to supply get_fattime() at all. fs_stat() in fs_fatfs.c decodes
 * whatever is stored, so listings will simply all read 2026-01-01.
 */

#define FF_FS_NOFSINFO	0
/* Trust the FAT32 FSINFO free-cluster count; f_getfree stays fast. */

#define FF_FS_LOCK		0
/* No file-lock table. Single-threaded foreground access only. */

#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
/*
 * 0: no OS mutexes. Would require ff_mutex_create/take/give/delete from
 * ffsystem.c, which is deliberately not vendored — there is no RTOS here.
 */
