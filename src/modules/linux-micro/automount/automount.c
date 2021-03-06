/*
 * This file is part of the Soletta Project
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define SOL_LOG_DOMAIN &_log_domain
#include "sol-log-internal.h"
SOL_LOG_INTERNAL_DECLARE_STATIC(_log_domain, "linux-micro-automount");

#include "sol-file-reader.h"
#include "sol-mainloop.h"
#include "sol-platform-linux-micro.h"
#include "sol-platform.h"
#include "sol-util.h"

#define EXTMAGIC "\123\357"
#define EXT_SB_OFFSET 1024
#define EXT_MAGIC_OFFSET EXT_SB_OFFSET + 0x38
#define EXT_FEATURE_OFFSET EXT_SB_OFFSET + 0x5c

#define EXT3_FEATURE_COMPAT_HAS_JOURNAL     0x0004

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR    0x0004

#define EXT2_FEATURE_INCOMPAT_FILETYPE      0x0002
#define EXT3_FEATURE_INCOMPAT_RECOVER       0x0004
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV   0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG       0x0010

#define EXT2_FEATURE_RO_COMPAT_SUPP (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | \
    EXT2_FEATURE_RO_COMPAT_LARGE_FILE | \
    EXT2_FEATURE_RO_COMPAT_BTREE_DIR)
#define EXT2_FEATURE_INCOMPAT_SUPP  (EXT2_FEATURE_INCOMPAT_FILETYPE | \
    EXT2_FEATURE_INCOMPAT_META_BG)
#define EXT2_FEATURE_INCOMPAT_UNSUPPORTED   ~EXT2_FEATURE_INCOMPAT_SUPP
#define EXT2_FEATURE_RO_COMPAT_UNSUPPORTED  ~EXT2_FEATURE_RO_COMPAT_SUPP

#define EXT3_FEATURE_RO_COMPAT_SUPP (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | \
    EXT2_FEATURE_RO_COMPAT_LARGE_FILE | \
    EXT2_FEATURE_RO_COMPAT_BTREE_DIR)
#define EXT3_FEATURE_INCOMPAT_SUPP  (EXT2_FEATURE_INCOMPAT_FILETYPE | \
    EXT3_FEATURE_INCOMPAT_RECOVER | \
    EXT2_FEATURE_INCOMPAT_META_BG)
#define EXT3_FEATURE_INCOMPAT_UNSUPPORTED   ~EXT3_FEATURE_INCOMPAT_SUPP
#define EXT3_FEATURE_RO_COMPAT_UNSUPPORTED  ~EXT3_FEATURE_RO_COMPAT_SUPP

#define MAGIC(str_, offset_)  { .magic = (str_), .length = sizeof(str_) - 1,  .offset = (offset_) }
#define FSTYPE(id_, magic_, cb_) { .id = id_, .magic = magic_, .fs_cb = cb_ }

/** superblock is the same for ext2/3/4 **/
struct ext_super_block {
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
} __attribute__((packed));

struct filesystem_magic {
    const char *magic;
    int length;
    int offset;
};

struct filesystem {
    const char *id;
    const struct filesystem_magic *magic;
    bool (*fs_cb)(int fd);
};

static const struct filesystem_magic vfat_magic[] = {
    MAGIC("MSWIN", 0x52),
    MAGIC("FAT32   ", 0x52),
    MAGIC("MSDOS", 0x36),
    MAGIC("FAT16   ", 0x36),
    MAGIC("FAT12   ", 0x36),
    MAGIC("FAT     ", 0x36),
    MAGIC("\353", 0),
    MAGIC("\351", 0),
    MAGIC("\125\252", 0x1fe),
    { NULL }
};

static const struct filesystem_magic hfsplus_magic[] = {
    MAGIC("BD", 1024),
    MAGIC("H+", 1024),
    MAGIC( "HX", 1024),
    { NULL }
};

static const struct filesystem_magic hfs_magic[] = {
    MAGIC("BD", 1),
    { NULL }
};

static const struct filesystem_magic ext_magic[] = {
    MAGIC(EXTMAGIC, EXT_MAGIC_OFFSET),
    { NULL }
};

static bool
ext_read_superblock(int fd, char *buff)
{
    int err;

    while (true) {
        err = lseek(fd, EXT_FEATURE_OFFSET, SEEK_SET);
        if (err < 0) {
            if (errno == EINTR) {
                SOL_ERR("Could not lseek, retrying... - %s",
                    sol_util_strerrora(errno));
                continue;
            } else {
                SOL_ERR("Coud not lseek to ext superblock");
                return false;
            }
        }

        err = read(fd, buff, sizeof(struct ext_super_block));
        if (err != sizeof(struct ext_super_block)) {
            if (errno == EINTR) {
                SOL_ERR("Could not read, retrying... - %s",
                    sol_util_strerrora(errno));
                continue;
            } else {
                SOL_ERR("Could not read ext superblock features");
                return false;
            }
        }

        break;
    }

    return true;
}

static bool
ext2_probe_cb(int fd)
{
    char buff[512];
    struct ext_super_block *sb;

    if (!ext_read_superblock(fd, buff))
        return false;

    sb = (struct ext_super_block *)buff;
    if (sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)
        return false;

    if ((sb->s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_UNSUPPORTED) ||
        (sb->s_feature_incompat  & EXT2_FEATURE_INCOMPAT_UNSUPPORTED))
        return false;

    return true;
}

static bool
ext3_probe_cb(int fd)
{
    char buff[512];
    struct ext_super_block *sb;

    if (!ext_read_superblock(fd, buff))
        return false;

    sb = (struct ext_super_block *)buff;
    if (!(sb->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL))
        return false;

    if ((sb->s_feature_ro_compat & EXT3_FEATURE_RO_COMPAT_UNSUPPORTED) ||
        (sb->s_feature_incompat & EXT3_FEATURE_INCOMPAT_UNSUPPORTED))
        return false;

    return true;
}

static bool
ext4_probe_cb(int fd)
{
    char buff[512];
    struct ext_super_block *sb;

    if (!ext_read_superblock(fd, buff))
        return false;

    sb = (struct ext_super_block *)buff;
    if (sb->s_feature_incompat & EXT3_FEATURE_INCOMPAT_JOURNAL_DEV)
        return false;

    return true;
}

static const struct filesystem table[] = {
    FSTYPE("vfat", vfat_magic, NULL),
    FSTYPE("hfsplus", hfsplus_magic, NULL),
    FSTYPE("hfs", hfs_magic, NULL),
    FSTYPE("ext2", ext_magic, ext2_probe_cb),
    FSTYPE("ext3", ext_magic, ext3_probe_cb),
    FSTYPE("ext4", ext_magic, ext4_probe_cb),
};

static bool
automount_test_magic(const struct filesystem *fs, const struct filesystem_magic *magic, char *dev)
{
    int fd, err;
    char buff[magic->length];
    bool res = false;

    fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        SOL_ERR("Could not open dev: %s", dev);
        return false;
    }

    err = lseek(fd, magic->offset, SEEK_SET);
    if (err < 0) {
        SOL_ERR("Coud not lseek dev: %s", dev);
        goto err;
    }

    err = read(fd, buff, magic->length);
    if (err != magic->length) {
        SOL_ERR("Could not read dev: %s", dev);
        goto err;
    }

    err = memcmp(buff, magic->magic, magic->length);
    if (err == 0) {
        res = true;
        if (fs->fs_cb)
            res = fs->fs_cb(fd);
    }

    close(fd);
    return res;

err:
    close(fd);
    return false;
}

static const char *
automount_get_fstype(char *dev)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(table); i++) {
        const struct filesystem_magic *magic;

        for (magic = table[i].magic; magic->magic != NULL; magic++) {
            if (automount_test_magic(&table[i], magic, dev))
                return table[i].id;
        }
    }

    return NULL;
}

static void
mount_cb(void *data, const char *mpoint, int status)
{
    if (status == 0)
        SOL_INF("Successfully auto-mounted %s", mpoint);
    else
        SOL_ERR("Could not auto-mount %s", mpoint);
}

static void
uevent_cb(void *data, struct sol_uevent *uevent)
{
    char dev[PATH_MAX], mpoint[PATH_MAX];
    const char *fstype;
    int err;

    if (!sol_str_slice_str_eq(uevent->devtype, "partition")) {
        SOL_DBG("Non partition devtype (%s), skipping", uevent->devtype.data);
        return;
    }

    err = snprintf(dev, sizeof(dev), "/dev/%s", uevent->devname.data);
    if (err < 0 || err >= (int)sizeof(dev)) {
        SOL_ERR("Could not format nodedev path");
        return;
    }

    err = snprintf(mpoint, sizeof(mpoint), "/mnt/%s", uevent->devname.data);
    if (err < 0 || err >= (int)sizeof(mpoint)) {
        SOL_ERR("Could not format mount point path");
        return;
    }

    fstype = automount_get_fstype(dev);
    if (!fstype) {
        SOL_ERR("Could not determine the fstype for %s, not mounting", dev);
        return;
    }

    err = mkdir(mpoint, 0755);
    if (err < 0 && errno != EEXIST) {
        SOL_ERR("Could not create mount point dir: %s - %s", mpoint,
            sol_util_strerrora(errno));
        return;
    }

    err = sol_platform_linux_mount(dev, mpoint, fstype, mount_cb, NULL);
    if (err != 0)
        SOL_WRN("Couldn't spawn mount process to mount %s to %s", dev, mpoint);
    else
        SOL_INF("Mounted %s to %s", dev, mpoint);
}

static int
automount_start(const struct sol_platform_linux_micro_module *mod, const char *service)
{
    return sol_platform_linux_uevent_subscribe("add", "block", uevent_cb, NULL);
}

static int
automount_init(const struct sol_platform_linux_micro_module *module, const char *service)
{
    SOL_LOG_INTERNAL_INIT_ONCE;
    return 0;
}

static void
automount_shutdown(const struct sol_platform_linux_micro_module *module, const char *service)
{
    sol_platform_linux_uevent_unsubscribe("add", "block", uevent_cb, NULL);
}

SOL_PLATFORM_LINUX_MICRO_MODULE(AUTOMOUNT,
    .name = "automount",
    .init = automount_init,
    .shutdown = automount_shutdown,
    .start = automount_start,
    );
