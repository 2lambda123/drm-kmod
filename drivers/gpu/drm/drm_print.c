/*
 * Copyright (C) 2016 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 */

#define DEBUG /* for pr_debug() */

#ifndef __linux__
#include <machine/stdarg.h>
#else
#include <stdarg.h>
#endif
#include <linux/seq_file.h>
#include <drm/drmP.h>
#include <drm/drm_print.h>

void __drm_printfn_seq_file(struct drm_printer *p, struct va_format *vaf)
{
#ifndef __linux__
	seq_printf((struct seq_file *)(p->arg), "%pV", vaf);
#else
	seq_printf(p->arg, "%pV", vaf);
#endif	
}
EXPORT_SYMBOL(__drm_printfn_seq_file);

void __drm_printfn_info(struct drm_printer *p, struct va_format *vaf)
{
#ifndef __linux__
	dev_info((const struct device *)(p->arg), "[" DRM_NAME "] %pV", vaf);
#else
	dev_info(p->arg, "[" DRM_NAME "] %pV", vaf);
#endif	
}
EXPORT_SYMBOL(__drm_printfn_info);

void __drm_printfn_debug(struct drm_printer *p, struct va_format *vaf)
{
	pr_debug("%s %pV", p->prefix, vaf);
}
EXPORT_SYMBOL(__drm_printfn_debug);

/**
 * drm_printf - print to a &drm_printer stream
 * @p: the &drm_printer
 * @f: format string
 */
void drm_printf(struct drm_printer *p, const char *f, ...)
{
	va_list args;

	va_start(args, f);
	drm_vprintf(p, f, &args);
	va_end(args);
}
EXPORT_SYMBOL(drm_printf);

#ifdef __linux__
void drm_dev_printk(const struct device *dev, const char *level,
		    const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	if (dev)
		dev_printk(level, dev, "[" DRM_NAME ":%ps] %pV",
			   __builtin_return_address(0), &vaf);
	else
		printk("%s" "[" DRM_NAME ":%ps] %pV",
		       level, __builtin_return_address(0), &vaf);

	va_end(args);
}
EXPORT_SYMBOL(drm_dev_printk);
#else
void drm_dev_printk(const struct device *dev, const char *level,
		    const char *function_name, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	if (dev)
		device_printf((dev)->bsddev, "[" DRM_NAME ":%s] ", function_name);
	else
		printf("[" DRM_NAME ":%s] ", function_name);
	vprintf(format, args);
	va_end(args);
}
#endif

#ifdef __linux__
void drm_dev_dbg(const struct device *dev, unsigned int category,
		 const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (!(drm_debug & category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	if (dev)
		dev_printk(KERN_DEBUG, dev, "[" DRM_NAME ":%ps] %pV",
			   __builtin_return_address(0), &vaf);
	else
		printk(KERN_DEBUG "[" DRM_NAME ":%ps] %pV",
		       __builtin_return_address(0), &vaf);

	va_end(args);
}
EXPORT_SYMBOL(drm_dev_dbg);
#else
void drm_dev_dbg(const struct device *dev, unsigned int category,
		 const char *function_name, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (!(drm_debug & category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	if (dev)
		device_printf((dev)->bsddev, "[" DRM_NAME ":%s] ", function_name);
	else
		printf("[" DRM_NAME ":%s] ", function_name);
	vprintf(format, args);
	va_end(args);
}
#endif

#ifdef __linux__
void drm_dbg(unsigned int category, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (!(drm_debug & category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	printk(KERN_DEBUG "[" DRM_NAME ":%ps] %pV",
	       __builtin_return_address(0), &vaf);

	va_end(args);
}
EXPORT_SYMBOL(drm_dbg);
#else
void drm_dbg(const char *level, unsigned int category,
	const char *function_name, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (!(drm_debug & category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	if (SCHEDULER_STOPPED() || kdb_active) {
		printf(" ");
		return;
	}
	if (panicstr != NULL)
		return;

	printf("[" DRM_NAME ":%s] ", function_name);
	vprintf(format, args);

	va_end(args);
}
#endif

#ifdef __linux__
void drm_err(const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	printk(KERN_ERR "[" DRM_NAME ":%ps] *ERROR* %pV",
	       __builtin_return_address(0), &vaf);

	va_end(args);
}
EXPORT_SYMBOL(drm_err);
#else
void drm_err(const char *function_name, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (category != DRM_UT_NONE && !(drm_debug & category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	if (SCHEDULER_STOPPED() || kdb_active) {
		printf(" ");
		return;
	}
	if (panicstr != NULL)
		return;

	printf("[" DRM_NAME " ERROR :%s] ", function_name);
	vprintf(format, args);

	va_end(args);
}
#endif
