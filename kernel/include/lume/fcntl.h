/*
 * LumeOS open(2)/fcntl(2) flags.
 *
 * Numeric values are the Linux 32-bit ARM ones
 * (arch/arm/include/uapi/asm/fcntl.h over asm-generic/fcntl.h) so that a
 * Linux-targeted libc running on LumeOS sees the flags it expects.
 *
 * NOTE: on ARM, O_DIRECTORY/O_NOFOLLOW/O_DIRECT/O_LARGEFILE differ from the
 * x86 values - they are the octal values below, verified against the Linux
 * v6.6 UAPI headers.
 */
#ifndef LUME_FCNTL_H
#define LUME_FCNTL_H

#define O_ACCMODE   00000003
#define O_RDONLY    00000000
#define O_WRONLY    00000001
#define O_RDWR      00000002
#define O_CREAT     00000100
#define O_EXCL      00000200
#define O_NOCTTY    00000400
#define O_TRUNC     00001000
#define O_APPEND    00002000
#define O_NONBLOCK  00004000
#define O_DSYNC     00010000
#define FASYNC      00020000
#define O_DIRECT    0200000
#define O_LARGEFILE 0400000
#define O_DIRECTORY 040000
#define O_NOFOLLOW  0100000
#define O_NOATIME   01000000
#define O_CLOEXEC   02000000
#define __O_SYNC    04000000
#define O_SYNC      (__O_SYNC | O_DSYNC)
#define O_PATH      010000000
#define O_TMPFILE   020200000

/* fcntl(2) commands. */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_SETOWN    8
#define F_GETOWN    9
#define F_SETSIG    10
#define F_GETSIG    11
#define F_SETLK     6
#define F_SETLKW    7
#define F_GETLK     5
#define F_DUPFD_CLOEXEC 1030

#define FD_CLOEXEC  1

#endif /* LUME_FCNTL_H */
