/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2021 Advanced Micro Devices, Inc.
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
 * Authors: AMD
 *
 */

#ifndef __DC_LINK_DPIA_H__
#define __DC_LINK_DPIA_H__

/* This module implements functionality for training DPIA links. */

struct dc_link;
struct dc_link_settings;

/* Read tunneling device capability from DPCD and update link capability
 * accordingly.
 */
enum dc_status dpcd_get_tunneling_device_data(struct dc_link *link);

/* Train DP tunneling link for USB4 DPIA display endpoint.
 * DPIA equivalent of dc_link_dp_perfrorm_link_training.
 * Aborts link training upon detection of sink unplug.
 */
enum link_training_result
dc_link_dpia_perform_link_training(struct dc_link *link,
	const struct dc_link_settings *link_setting,
	bool skip_video_pattern);

#endif /* __DC_LINK_DPIA_H__ */
