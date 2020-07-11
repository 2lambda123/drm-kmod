/*
 * Copyright 2020 Mauro Rossi <issor.oruam@gmail.com>
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

#include "dm_services.h"
#include "dc.h"
#include "core_types.h"
#include "dce60_hw_sequencer.h"

#include "dce/dce_hwseq.h"
#include "dce110/dce110_hw_sequencer.h"
#include "dce100/dce100_hw_sequencer.h"

/* include DCE8 register header files */
#include "dce/dce_6_0_d.h"
#include "dce/dce_6_0_sh_mask.h"

/*******************************************************************************
 * Private definitions
 ******************************************************************************/

/***************************PIPE_CONTROL***********************************/

void dce60_hw_sequencer_construct(struct dc *dc)
{
	dce110_hw_sequencer_construct(dc);

	dc->hwseq->funcs.enable_display_power_gating = dce100_enable_display_power_gating;
	dc->hwss.pipe_control_lock = dce_pipe_control_lock;
	dc->hwss.prepare_bandwidth = dce100_prepare_bandwidth;
	dc->hwss.optimize_bandwidth = dce100_optimize_bandwidth;
}

