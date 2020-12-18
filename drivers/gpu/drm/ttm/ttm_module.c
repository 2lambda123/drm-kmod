/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/**************************************************************************
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 * 	    Jerome Glisse
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <drm/drm_sysfs.h>

#ifdef __FreeBSD__
#include <linux/completion.h>
#include <linux/wait.h>

#include <drm/ttm/ttm_sysctl_freebsd.h>

SYSCTL_NODE(_hw, OID_AUTO, ttm,
    CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "TTM memory manager parameters");
#endif

#include "ttm_module.h"

struct dentry *ttm_debugfs_root;

static int __init ttm_init(void)
{
	ttm_debugfs_root = debugfs_create_dir("ttm", NULL);
	return 0;
}

static void __exit ttm_exit(void)
{
	debugfs_remove(ttm_debugfs_root);
}

#ifdef __linux__
module_init(ttm_init);
module_exit(ttm_exit);

MODULE_AUTHOR("Thomas Hellstrom, Jerome Glisse");
MODULE_DESCRIPTION("TTM memory manager subsystem (for DRM device)");
MODULE_LICENSE("GPL and additional rights");
#elif defined(__FreeBSD__)
LKPI_DRIVER_MODULE(ttm, ttm_init, ttm_exit);
MODULE_VERSION(ttm, 1);
#ifdef CONFIG_AGP
MODULE_DEPEND(ttm, agp, 1, 1, 1);
#endif
MODULE_DEPEND(ttm, drmn, 2, 2, 2);
MODULE_DEPEND(ttm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(ttm, linuxkpi_gplv2, 1, 1, 1);
MODULE_DEPEND(ttm, dmabuf, 1, 1, 1);
#ifdef CONFIG_DEBUG_FS
MODULE_DEPEND(amdgpu, lindebugfs, 1, 1, 1);
#endif
#endif
