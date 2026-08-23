#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <bits/inline-definition.h>

__MLIBC_INLINE_DEFINITION unsigned int __mlibc_dev_major(
        unsigned long long int dev) {
    return ((dev >> 8) & 0xfff) | ((unsigned int)(dev >> 32) & ~0xfff);
}

__MLIBC_INLINE_DEFINITION unsigned int __mlibc_dev_minor(
        unsigned long long int dev) {
    return (dev & 0xff) | ((unsigned int)(dev >> 12) & ~0xff);
}

__MLIBC_INLINE_DEFINITION unsigned long long int __mlibc_dev_makedev(
        unsigned int major, unsigned int minor) {
    return ((minor & 0xff) | ((major & 0xfff) << 8)
            | (((unsigned long long int)(minor & ~0xff)) << 12)
            | (((unsigned long long int)(major & ~0xfff)) << 32));
}

#define major(dev) __mlibc_dev_major(dev)
#define minor(dev) __mlibc_dev_minor(dev)
#define makedev(major, minor) __mlibc_dev_makedev(major, minor)

#endif
