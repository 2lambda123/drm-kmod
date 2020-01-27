/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <drm/drm_atomic_helper.h>

#include "display/intel_dp.h"

#include "i915_drv.h"
#include "intel_atomic.h"
#include "intel_display_types.h"
#include "intel_psr.h"
#include "intel_sprite.h"

/**
 * DOC: Panel Self Refresh (PSR/SRD)
 *
 * Since Haswell Display controller supports Panel Self-Refresh on display
 * panels witch have a remote frame buffer (RFB) implemented according to PSR
 * spec in eDP1.3. PSR feature allows the display to go to lower standby states
 * when system is idle but display is on as it eliminates display refresh
 * request to DDR memory completely as long as the frame buffer for that
 * display is unchanged.
 *
 * Panel Self Refresh must be supported by both Hardware (source) and
 * Panel (sink).
 *
 * PSR saves power by caching the framebuffer in the panel RFB, which allows us
 * to power down the link and memory controller. For DSI panels the same idea
 * is called "manual mode".
 *
 * The implementation uses the hardware-based PSR support which automatically
 * enters/exits self-refresh mode. The hardware takes care of sending the
 * required DP aux message and could even retrain the link (that part isn't
 * enabled yet though). The hardware also keeps track of any frontbuffer
 * changes to know when to exit self-refresh mode again. Unfortunately that
 * part doesn't work too well, hence why the i915 PSR support uses the
 * software frontbuffer tracking to make sure it doesn't miss a screen
 * update. For this integration intel_psr_invalidate() and intel_psr_flush()
 * get called by the frontbuffer tracking code. Note that because of locking
 * issues the self-refresh re-enable code is done from a work queue, which
 * must be correctly synchronized/cancelled when shutting down the pipe."
 */

static bool psr_global_enabled(u32 debug)
{
	switch (debug & I915_PSR_DEBUG_MODE_MASK) {
	case I915_PSR_DEBUG_DEFAULT:
		return i915_modparams.enable_psr;
	case I915_PSR_DEBUG_DISABLE:
		return false;
	default:
		return true;
	}
}

static bool intel_psr2_enabled(struct drm_i915_private *dev_priv,
			       const struct intel_crtc_state *crtc_state)
{
	/* Cannot enable DSC and PSR2 simultaneously */
	WARN_ON(crtc_state->dsc.compression_enable &&
		crtc_state->has_psr2);

	switch (dev_priv->psr.debug & I915_PSR_DEBUG_MODE_MASK) {
	case I915_PSR_DEBUG_DISABLE:
	case I915_PSR_DEBUG_FORCE_PSR1:
		return false;
	default:
		return crtc_state->has_psr2;
	}
}

static void psr_irq_control(struct drm_i915_private *dev_priv)
{
	enum transcoder trans_shift;
	u32 mask, val;
	i915_reg_t imr_reg;

	/*
	 * gen12+ has registers relative to transcoder and one per transcoder
	 * using the same bit definition: handle it as TRANSCODER_EDP to force
	 * 0 shift in bit definition
	 */
	if (INTEL_GEN(dev_priv) >= 12) {
		trans_shift = 0;
		imr_reg = TRANS_PSR_IMR(dev_priv->psr.transcoder);
	} else {
		trans_shift = dev_priv->psr.transcoder;
		imr_reg = EDP_PSR_IMR;
	}

	mask = EDP_PSR_ERROR(trans_shift);
	if (dev_priv->psr.debug & I915_PSR_DEBUG_IRQ)
		mask |= EDP_PSR_POST_EXIT(trans_shift) |
			EDP_PSR_PRE_ENTRY(trans_shift);

	/* Warning: it is masking/setting reserved bits too */
	val = intel_de_read(dev_priv, imr_reg);
	val &= ~EDP_PSR_TRANS_MASK(trans_shift);
	val |= ~mask;
	intel_de_write(dev_priv, imr_reg, val);
}

static void psr_event_print(u32 val, bool psr2_enabled)
{
	DRM_DEBUG_KMS("PSR exit events: 0x%x\n", val);
	if (val & PSR_EVENT_PSR2_WD_TIMER_EXPIRE)
		DRM_DEBUG_KMS("\tPSR2 watchdog timer expired\n");
	if ((val & PSR_EVENT_PSR2_DISABLED) && psr2_enabled)
		DRM_DEBUG_KMS("\tPSR2 disabled\n");
	if (val & PSR_EVENT_SU_DIRTY_FIFO_UNDERRUN)
		DRM_DEBUG_KMS("\tSU dirty FIFO underrun\n");
	if (val & PSR_EVENT_SU_CRC_FIFO_UNDERRUN)
		DRM_DEBUG_KMS("\tSU CRC FIFO underrun\n");
	if (val & PSR_EVENT_GRAPHICS_RESET)
		DRM_DEBUG_KMS("\tGraphics reset\n");
	if (val & PSR_EVENT_PCH_INTERRUPT)
		DRM_DEBUG_KMS("\tPCH interrupt\n");
	if (val & PSR_EVENT_MEMORY_UP)
		DRM_DEBUG_KMS("\tMemory up\n");
	if (val & PSR_EVENT_FRONT_BUFFER_MODIFY)
		DRM_DEBUG_KMS("\tFront buffer modification\n");
	if (val & PSR_EVENT_WD_TIMER_EXPIRE)
		DRM_DEBUG_KMS("\tPSR watchdog timer expired\n");
	if (val & PSR_EVENT_PIPE_REGISTERS_UPDATE)
		DRM_DEBUG_KMS("\tPIPE registers updated\n");
	if (val & PSR_EVENT_REGISTER_UPDATE)
		DRM_DEBUG_KMS("\tRegister updated\n");
	if (val & PSR_EVENT_HDCP_ENABLE)
		DRM_DEBUG_KMS("\tHDCP enabled\n");
	if (val & PSR_EVENT_KVMR_SESSION_ENABLE)
		DRM_DEBUG_KMS("\tKVMR session enabled\n");
	if (val & PSR_EVENT_VBI_ENABLE)
		DRM_DEBUG_KMS("\tVBI enabled\n");
	if (val & PSR_EVENT_LPSP_MODE_EXIT)
		DRM_DEBUG_KMS("\tLPSP mode exited\n");
	if ((val & PSR_EVENT_PSR_DISABLE) && !psr2_enabled)
		DRM_DEBUG_KMS("\tPSR disabled\n");
}

void intel_psr_irq_handler(struct drm_i915_private *dev_priv, u32 psr_iir)
{
	enum transcoder cpu_transcoder = dev_priv->psr.transcoder;
	enum transcoder trans_shift;
	i915_reg_t imr_reg;
	ktime_t time_ns =  ktime_get();

	if (INTEL_GEN(dev_priv) >= 12) {
		trans_shift = 0;
		imr_reg = TRANS_PSR_IMR(dev_priv->psr.transcoder);
	} else {
		trans_shift = dev_priv->psr.transcoder;
		imr_reg = EDP_PSR_IMR;
	}

	if (psr_iir & EDP_PSR_PRE_ENTRY(trans_shift)) {
		dev_priv->psr.last_entry_attempt = time_ns;
		DRM_DEBUG_KMS("[transcoder %s] PSR entry attempt in 2 vblanks\n",
			      transcoder_name(cpu_transcoder));
	}

	if (psr_iir & EDP_PSR_POST_EXIT(trans_shift)) {
		dev_priv->psr.last_exit = time_ns;
		DRM_DEBUG_KMS("[transcoder %s] PSR exit completed\n",
			      transcoder_name(cpu_transcoder));

		if (INTEL_GEN(dev_priv) >= 9) {
			u32 val = intel_de_read(dev_priv,
						PSR_EVENT(cpu_transcoder));
			bool psr2_enabled = dev_priv->psr.psr2_enabled;

			intel_de_write(dev_priv, PSR_EVENT(cpu_transcoder),
				       val);
			psr_event_print(val, psr2_enabled);
		}
	}

	if (psr_iir & EDP_PSR_ERROR(trans_shift)) {
		u32 val;

		DRM_WARN("[transcoder %s] PSR aux error\n",
			 transcoder_name(cpu_transcoder));

		dev_priv->psr.irq_aux_error = true;

		/*
		 * If this interruption is not masked it will keep
		 * interrupting so fast that it prevents the scheduled
		 * work to run.
		 * Also after a PSR error, we don't want to arm PSR
		 * again so we don't care about unmask the interruption
		 * or unset irq_aux_error.
		 */
		val = intel_de_read(dev_priv, imr_reg);
		val |= EDP_PSR_ERROR(trans_shift);
		intel_de_write(dev_priv, imr_reg, val);

		schedule_work(&dev_priv->psr.work);
	}
}

static bool intel_dp_get_alpm_status(struct intel_dp *intel_dp)
{
	u8 alpm_caps = 0;

	if (drm_dp_dpcd_readb(&intel_dp->aux, DP_RECEIVER_ALPM_CAP,
			      &alpm_caps) != 1)
		return false;
	return alpm_caps & DP_ALPM_CAP;
}

static u8 intel_dp_get_sink_sync_latency(struct intel_dp *intel_dp)
{
	u8 val = 8; /* assume the worst if we can't read the value */

	if (drm_dp_dpcd_readb(&intel_dp->aux,
			      DP_SYNCHRONIZATION_LATENCY_IN_SINK, &val) == 1)
		val &= DP_MAX_RESYNC_FRAME_COUNT_MASK;
	else
		DRM_DEBUG_KMS("Unable to get sink synchronization latency, assuming 8 frames\n");
	return val;
}

static u16 intel_dp_get_su_x_granulartiy(struct intel_dp *intel_dp)
{
	u16 val;
	ssize_t r;

	/*
	 * Returning the default X granularity if granularity not required or
	 * if DPCD read fails
	 */
	if (!(intel_dp->psr_dpcd[1] & DP_PSR2_SU_GRANULARITY_REQUIRED))
		return 4;

	r = drm_dp_dpcd_read(&intel_dp->aux, DP_PSR2_SU_X_GRANULARITY, &val, 2);
	if (r != 2)
		DRM_DEBUG_KMS("Unable to read DP_PSR2_SU_X_GRANULARITY\n");

	/*
	 * Spec says that if the value read is 0 the default granularity should
	 * be used instead.
	 */
	if (r != 2 || val == 0)
		val = 4;

	return val;
}

void intel_psr_init_dpcd(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv =
		to_i915(dp_to_dig_port(intel_dp)->base.base.dev);

	if (dev_priv->psr.dp) {
		DRM_WARN("More than one eDP panel found, PSR support should be extended\n");
		return;
	}

	drm_dp_dpcd_read(&intel_dp->aux, DP_PSR_SUPPORT, intel_dp->psr_dpcd,
			 sizeof(intel_dp->psr_dpcd));

	if (!intel_dp->psr_dpcd[0])
		return;
	DRM_DEBUG_KMS("eDP panel supports PSR version %x\n",
		      intel_dp->psr_dpcd[0]);

	if (drm_dp_has_quirk(&intel_dp->desc, DP_DPCD_QUIRK_NO_PSR)) {
		DRM_DEBUG_KMS("PSR support not currently available for this panel\n");
		return;
	}

	if (!(intel_dp->edp_dpcd[1] & DP_EDP_SET_POWER_CAP)) {
		DRM_DEBUG_KMS("Panel lacks power state control, PSR cannot be enabled\n");
		return;
	}

	dev_priv->psr.sink_support = true;
	dev_priv->psr.sink_sync_latency =
		intel_dp_get_sink_sync_latency(intel_dp);

	dev_priv->psr.dp = intel_dp;

	if (INTEL_GEN(dev_priv) >= 9 &&
	    (intel_dp->psr_dpcd[0] == DP_PSR2_WITH_Y_COORD_IS_SUPPORTED)) {
		bool y_req = intel_dp->psr_dpcd[1] &
			     DP_PSR2_SU_Y_COORDINATE_REQUIRED;
		bool alpm = intel_dp_get_alpm_status(intel_dp);

		/*
		 * All panels that supports PSR version 03h (PSR2 +
		 * Y-coordinate) can handle Y-coordinates in VSC but we are
		 * only sure that it is going to be used when required by the
		 * panel. This way panel is capable to do selective update
		 * without a aux frame sync.
		 *
		 * To support PSR version 02h and PSR version 03h without
		 * Y-coordinate requirement panels we would need to enable
		 * GTC first.
		 */
		dev_priv->psr.sink_psr2_support = y_req && alpm;
		DRM_DEBUG_KMS("PSR2 %ssupported\n",
			      dev_priv->psr.sink_psr2_support ? "" : "not ");

		if (dev_priv->psr.sink_psr2_support) {
			dev_priv->psr.colorimetry_support =
				intel_dp_get_colorimetry_status(intel_dp);
			dev_priv->psr.su_x_granularity =
				intel_dp_get_su_x_granulartiy(intel_dp);
		}
	}
}

static void intel_psr_setup_vsc(struct intel_dp *intel_dp,
				const struct intel_crtc_state *crtc_state)
{
	struct intel_digital_port *intel_dig_port = dp_to_dig_port(intel_dp);
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct dp_sdp psr_vsc;

	if (dev_priv->psr.psr2_enabled) {
		/* Prepare VSC Header for SU as per EDP 1.4 spec, Table 6.11 */
		memset(&psr_vsc, 0, sizeof(psr_vsc));
		psr_vsc.sdp_header.HB0 = 0;
		psr_vsc.sdp_header.HB1 = 0x7;
		if (dev_priv->psr.colorimetry_support) {
			psr_vsc.sdp_header.HB2 = 0x5;
			psr_vsc.sdp_header.HB3 = 0x13;
		} else {
			psr_vsc.sdp_header.HB2 = 0x4;
			psr_vsc.sdp_header.HB3 = 0xe;
		}
	} else {
		/* Prepare VSC packet as per EDP 1.3 spec, Table 3.10 */
		memset(&psr_vsc, 0, sizeof(psr_vsc));
		psr_vsc.sdp_header.HB0 = 0;
		psr_vsc.sdp_header.HB1 = 0x7;
		psr_vsc.sdp_header.HB2 = 0x2;
		psr_vsc.sdp_header.HB3 = 0x8;
	}

	intel_dig_port->write_infoframe(&intel_dig_port->base,
					crtc_state,
					DP_SDP_VSC, &psr_vsc, sizeof(psr_vsc));
}

static void hsw_psr_setup_aux(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 aux_clock_divider, aux_ctl;
	int i;
	static const u8 aux_msg[] = {
		[0] = DP_AUX_NATIVE_WRITE << 4,
		[1] = DP_SET_POWER >> 8,
		[2] = DP_SET_POWER & 0xff,
		[3] = 1 - 1,
		[4] = DP_SET_POWER_D0,
	};
	u32 psr_aux_mask = EDP_PSR_AUX_CTL_TIME_OUT_MASK |
			   EDP_PSR_AUX_CTL_MESSAGE_SIZE_MASK |
			   EDP_PSR_AUX_CTL_PRECHARGE_2US_MASK |
			   EDP_PSR_AUX_CTL_BIT_CLOCK_2X_MASK;

	BUILD_BUG_ON(sizeof(aux_msg) > 20);
	for (i = 0; i < sizeof(aux_msg); i += 4)
		intel_de_write(dev_priv,
			       EDP_PSR_AUX_DATA(dev_priv->psr.transcoder, i >> 2),
			       intel_dp_pack_aux(&aux_msg[i], sizeof(aux_msg) - i));

	aux_clock_divider = intel_dp->get_aux_clock_divider(intel_dp, 0);

	/* Start with bits set for DDI_AUX_CTL register */
	aux_ctl = intel_dp->get_aux_send_ctl(intel_dp, sizeof(aux_msg),
					     aux_clock_divider);

	/* Select only valid bits for SRD_AUX_CTL */
	aux_ctl &= psr_aux_mask;
	intel_de_write(dev_priv, EDP_PSR_AUX_CTL(dev_priv->psr.transcoder),
		       aux_ctl);
}

static void intel_psr_enable_sink(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u8 dpcd_val = DP_PSR_ENABLE;

	/* Enable ALPM at sink for psr2 */
	if (dev_priv->psr.psr2_enabled) {
		drm_dp_dpcd_writeb(&intel_dp->aux, DP_RECEIVER_ALPM_CONFIG,
				   DP_ALPM_ENABLE |
				   DP_ALPM_LOCK_ERROR_IRQ_HPD_ENABLE);

		dpcd_val |= DP_PSR_ENABLE_PSR2 | DP_PSR_IRQ_HPD_WITH_CRC_ERRORS;
	} else {
		if (dev_priv->psr.link_standby)
			dpcd_val |= DP_PSR_MAIN_LINK_ACTIVE;

		if (INTEL_GEN(dev_priv) >= 8)
			dpcd_val |= DP_PSR_CRC_VERIFICATION;
	}

	drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_EN_CFG, dpcd_val);

	drm_dp_dpcd_writeb(&intel_dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
}

static u32 intel_psr1_get_tp_time(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 val = 0;

	if (INTEL_GEN(dev_priv) >= 11)
		val |= EDP_PSR_TP4_TIME_0US;

	if (dev_priv->vbt.psr.tp1_wakeup_time_us == 0)
		val |= EDP_PSR_TP1_TIME_0us;
	else if (dev_priv->vbt.psr.tp1_wakeup_time_us <= 100)
		val |= EDP_PSR_TP1_TIME_100us;
	else if (dev_priv->vbt.psr.tp1_wakeup_time_us <= 500)
		val |= EDP_PSR_TP1_TIME_500us;
	else
		val |= EDP_PSR_TP1_TIME_2500us;

	if (dev_priv->vbt.psr.tp2_tp3_wakeup_time_us == 0)
		val |= EDP_PSR_TP2_TP3_TIME_0us;
	else if (dev_priv->vbt.psr.tp2_tp3_wakeup_time_us <= 100)
		val |= EDP_PSR_TP2_TP3_TIME_100us;
	else if (dev_priv->vbt.psr.tp2_tp3_wakeup_time_us <= 500)
		val |= EDP_PSR_TP2_TP3_TIME_500us;
	else
		val |= EDP_PSR_TP2_TP3_TIME_2500us;

	if (intel_dp_source_supports_hbr2(intel_dp) &&
	    drm_dp_tps3_supported(intel_dp->dpcd))
		val |= EDP_PSR_TP1_TP3_SEL;
	else
		val |= EDP_PSR_TP1_TP2_SEL;

	return val;
}

static u8 psr_compute_idle_frames(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	int idle_frames;

	/* Let's use 6 as the minimum to cover all known cases including the
	 * off-by-one issue that HW has in some cases.
	 */
	idle_frames = max(6, dev_priv->vbt.psr.idle_frames);
	idle_frames = max(idle_frames, dev_priv->psr.sink_sync_latency + 1);

	if (WARN_ON(idle_frames > 0xf))
		idle_frames = 0xf;

	return idle_frames;
}

static void hsw_activate_psr1(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 max_sleep_time = 0x1f;
	u32 val = EDP_PSR_ENABLE;

	val |= psr_compute_idle_frames(intel_dp) << EDP_PSR_IDLE_FRAME_SHIFT;

	val |= max_sleep_time << EDP_PSR_MAX_SLEEP_TIME_SHIFT;
	if (IS_HASWELL(dev_priv))
		val |= EDP_PSR_MIN_LINK_ENTRY_TIME_8_LINES;

	if (dev_priv->psr.link_standby)
		val |= EDP_PSR_LINK_STANDBY;

	val |= intel_psr1_get_tp_time(intel_dp);

	if (INTEL_GEN(dev_priv) >= 8)
		val |= EDP_PSR_CRC_ENABLE;

	val |= (intel_de_read(dev_priv, EDP_PSR_CTL(dev_priv->psr.transcoder)) &
		EDP_PSR_RESTORE_PSR_ACTIVE_CTX_MASK);
	intel_de_write(dev_priv, EDP_PSR_CTL(dev_priv->psr.transcoder), val);
}

static void hsw_activate_psr2(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 val;

	val = psr_compute_idle_frames(intel_dp) << EDP_PSR2_IDLE_FRAME_SHIFT;

	val |= EDP_PSR2_ENABLE | EDP_SU_TRACK_ENABLE;
	if (INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv))
		val |= EDP_Y_COORDINATE_ENABLE;

	val |= EDP_PSR2_FRAME_BEFORE_SU(dev_priv->psr.sink_sync_latency + 1);

	if (dev_priv->vbt.psr.psr2_tp2_tp3_wakeup_time_us >= 0 &&
	    dev_priv->vbt.psr.psr2_tp2_tp3_wakeup_time_us <= 50)
		val |= EDP_PSR2_TP2_TIME_50us;
	else if (dev_priv->vbt.psr.psr2_tp2_tp3_wakeup_time_us <= 100)
		val |= EDP_PSR2_TP2_TIME_100us;
	else if (dev_priv->vbt.psr.psr2_tp2_tp3_wakeup_time_us <= 500)
		val |= EDP_PSR2_TP2_TIME_500us;
	else
		val |= EDP_PSR2_TP2_TIME_2500us;

	/*
	 * PSR2 HW is incorrectly using EDP_PSR_TP1_TP3_SEL and BSpec is
	 * recommending keep this bit unset while PSR2 is enabled.
	 */
	intel_de_write(dev_priv, EDP_PSR_CTL(dev_priv->psr.transcoder), 0);

	intel_de_write(dev_priv, EDP_PSR2_CTL(dev_priv->psr.transcoder), val);
}

static bool
transcoder_has_psr2(struct drm_i915_private *dev_priv, enum transcoder trans)
{
	if (INTEL_GEN(dev_priv) < 9)
		return false;
	else if (INTEL_GEN(dev_priv) >= 12)
		return trans == TRANSCODER_A;
	else
		return trans == TRANSCODER_EDP;
}

static u32 intel_get_frame_time_us(const struct intel_crtc_state *cstate)
{
	if (!cstate || !cstate->hw.active)
		return 0;

	return DIV_ROUND_UP(1000 * 1000,
			    drm_mode_vrefresh(&cstate->hw.adjusted_mode));
}

static void psr2_program_idle_frames(struct drm_i915_private *dev_priv,
				     u32 idle_frames)
{
	u32 val;

	idle_frames <<=  EDP_PSR2_IDLE_FRAME_SHIFT;
	val = intel_de_read(dev_priv, EDP_PSR2_CTL(dev_priv->psr.transcoder));
	val &= ~EDP_PSR2_IDLE_FRAME_MASK;
	val |= idle_frames;
	intel_de_write(dev_priv, EDP_PSR2_CTL(dev_priv->psr.transcoder), val);
}

static void tgl_psr2_enable_dc3co(struct drm_i915_private *dev_priv)
{
	psr2_program_idle_frames(dev_priv, 0);
	intel_display_power_set_target_dc_state(dev_priv, DC_STATE_EN_DC3CO);
}

static void tgl_psr2_disable_dc3co(struct drm_i915_private *dev_priv)
{
	struct intel_dp *intel_dp = dev_priv->psr.dp;

	intel_display_power_set_target_dc_state(dev_priv, DC_STATE_EN_UPTO_DC6);
	psr2_program_idle_frames(dev_priv, psr_compute_idle_frames(intel_dp));
}

static void tgl_dc5_idle_thread(struct work_struct *work)
{
	struct drm_i915_private *dev_priv =
		container_of(work, typeof(*dev_priv), psr.idle_work.work);

	mutex_lock(&dev_priv->psr.lock);
	/* If delayed work is pending, it is not idle */
	if (delayed_work_pending(&dev_priv->psr.idle_work))
		goto unlock;

	DRM_DEBUG_KMS("DC5/6 idle thread\n");
	tgl_psr2_disable_dc3co(dev_priv);
unlock:
	mutex_unlock(&dev_priv->psr.lock);
}

static void tgl_disallow_dc3co_on_psr2_exit(struct drm_i915_private *dev_priv)
{
	if (!dev_priv->psr.dc3co_enabled)
		return;

	cancel_delayed_work(&dev_priv->psr.idle_work);
	/* Before PSR2 exit disallow dc3co*/
	tgl_psr2_disable_dc3co(dev_priv);
}

static void
tgl_dc3co_exitline_compute_config(struct intel_dp *intel_dp,
				  struct intel_crtc_state *crtc_state)
{
	const u32 crtc_vdisplay = crtc_state->uapi.adjusted_mode.crtc_vdisplay;
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 exit_scanlines;

	if (!(dev_priv->csr.allowed_dc_mask & DC_STATE_EN_DC3CO))
		return;

	/* B.Specs:49196 DC3CO only works with pipeA and DDIA.*/
	if (to_intel_crtc(crtc_state->uapi.crtc)->pipe != PIPE_A ||
	    dig_port->base.port != PORT_A)
		return;

	/*
	 * DC3CO Exit time 200us B.Spec 49196
	 * PSR2 transcoder Early Exit scanlines = ROUNDUP(200 / line time) + 1
	 */
	exit_scanlines =
		intel_usecs_to_scanlines(&crtc_state->uapi.adjusted_mode, 200) + 1;

	if (WARN_ON(exit_scanlines > crtc_vdisplay))
		return;

	crtc_state->dc3co_exitline = crtc_vdisplay - exit_scanlines;
}

static bool intel_psr2_config_valid(struct intel_dp *intel_dp,
				    struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	int crtc_hdisplay = crtc_state->hw.adjusted_mode.crtc_hdisplay;
	int crtc_vdisplay = crtc_state->hw.adjusted_mode.crtc_vdisplay;
	int psr_max_h = 0, psr_max_v = 0, max_bpp = 0;

	if (!dev_priv->psr.sink_psr2_support)
		return false;

	if (!transcoder_has_psr2(dev_priv, crtc_state->cpu_transcoder)) {
		DRM_DEBUG_KMS("PSR2 not supported in transcoder %s\n",
			      transcoder_name(crtc_state->cpu_transcoder));
		return false;
	}

	/*
	 * DSC and PSR2 cannot be enabled simultaneously. If a requested
	 * resolution requires DSC to be enabled, priority is given to DSC
	 * over PSR2.
	 */
	if (crtc_state->dsc.compression_enable) {
		DRM_DEBUG_KMS("PSR2 cannot be enabled since DSC is enabled\n");
		return false;
	}

	if (INTEL_GEN(dev_priv) >= 12) {
		psr_max_h = 5120;
		psr_max_v = 3200;
		max_bpp = 30;
	} else if (INTEL_GEN(dev_priv) >= 10 || IS_GEMINILAKE(dev_priv)) {
		psr_max_h = 4096;
		psr_max_v = 2304;
		max_bpp = 24;
	} else if (IS_GEN(dev_priv, 9)) {
		psr_max_h = 3640;
		psr_max_v = 2304;
		max_bpp = 24;
	}

	if (crtc_hdisplay > psr_max_h || crtc_vdisplay > psr_max_v) {
		DRM_DEBUG_KMS("PSR2 not enabled, resolution %dx%d > max supported %dx%d\n",
			      crtc_hdisplay, crtc_vdisplay,
			      psr_max_h, psr_max_v);
		return false;
	}

	if (crtc_state->pipe_bpp > max_bpp) {
		DRM_DEBUG_KMS("PSR2 not enabled, pipe bpp %d > max supported %d\n",
			      crtc_state->pipe_bpp, max_bpp);
		return false;
	}

	/*
	 * HW sends SU blocks of size four scan lines, which means the starting
	 * X coordinate and Y granularity requirements will always be met. We
	 * only need to validate the SU block width is a multiple of
	 * x granularity.
	 */
	if (crtc_hdisplay % dev_priv->psr.su_x_granularity) {
		DRM_DEBUG_KMS("PSR2 not enabled, hdisplay(%d) not multiple of %d\n",
			      crtc_hdisplay, dev_priv->psr.su_x_granularity);
		return false;
	}

	if (crtc_state->crc_enabled) {
		DRM_DEBUG_KMS("PSR2 not enabled because it would inhibit pipe CRC calculation\n");
		return false;
	}

	tgl_dc3co_exitline_compute_config(intel_dp, crtc_state);
	return true;
}

void intel_psr_compute_config(struct intel_dp *intel_dp,
			      struct intel_crtc_state *crtc_state)
{
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	int psr_setup_time;

	if (!CAN_PSR(dev_priv))
		return;

	if (intel_dp != dev_priv->psr.dp)
		return;

	/*
	 * HSW spec explicitly says PSR is tied to port A.
	 * BDW+ platforms have a instance of PSR registers per transcoder but
	 * for now it only supports one instance of PSR, so lets keep it
	 * hardcoded to PORT_A
	 */
	if (dig_port->base.port != PORT_A) {
		DRM_DEBUG_KMS("PSR condition failed: Port not supported\n");
		return;
	}

	if (dev_priv->psr.sink_not_reliable) {
		DRM_DEBUG_KMS("PSR sink implementation is not reliable\n");
		return;
	}

	if (adjusted_mode->flags & DRM_MODE_FLAG_INTERLACE) {
		DRM_DEBUG_KMS("PSR condition failed: Interlaced mode enabled\n");
		return;
	}

	psr_setup_time = drm_dp_psr_setup_time(intel_dp->psr_dpcd);
	if (psr_setup_time < 0) {
		DRM_DEBUG_KMS("PSR condition failed: Invalid PSR setup time (0x%02x)\n",
			      intel_dp->psr_dpcd[1]);
		return;
	}

	if (intel_usecs_to_scanlines(adjusted_mode, psr_setup_time) >
	    adjusted_mode->crtc_vtotal - adjusted_mode->crtc_vdisplay - 1) {
		DRM_DEBUG_KMS("PSR condition failed: PSR setup time (%d us) too long\n",
			      psr_setup_time);
		return;
	}

	crtc_state->has_psr = true;
	crtc_state->has_psr2 = intel_psr2_config_valid(intel_dp, crtc_state);
}

static void intel_psr_activate(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (transcoder_has_psr2(dev_priv, dev_priv->psr.transcoder))
		WARN_ON(intel_de_read(dev_priv, EDP_PSR2_CTL(dev_priv->psr.transcoder)) & EDP_PSR2_ENABLE);

	WARN_ON(intel_de_read(dev_priv, EDP_PSR_CTL(dev_priv->psr.transcoder)) & EDP_PSR_ENABLE);
	WARN_ON(dev_priv->psr.active);
	lockdep_assert_held(&dev_priv->psr.lock);

	/* psr1 and psr2 are mutually exclusive.*/
	if (dev_priv->psr.psr2_enabled)
		hsw_activate_psr2(intel_dp);
	else
		hsw_activate_psr1(intel_dp);

	dev_priv->psr.active = true;
}

static void intel_psr_enable_source(struct intel_dp *intel_dp,
				    const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 mask;

	/* Only HSW and BDW have PSR AUX registers that need to be setup. SKL+
	 * use hardcoded values PSR AUX transactions
	 */
	if (IS_HASWELL(dev_priv) || IS_BROADWELL(dev_priv))
		hsw_psr_setup_aux(intel_dp);

	if (dev_priv->psr.psr2_enabled && (IS_GEN(dev_priv, 9) &&
					   !IS_GEMINILAKE(dev_priv))) {
		i915_reg_t reg = CHICKEN_TRANS(cpu_transcoder);
		u32 chicken = intel_de_read(dev_priv, reg);

		chicken |= PSR2_VSC_ENABLE_PROG_HEADER |
			   PSR2_ADD_VERTICAL_LINE_COUNT;
		intel_de_write(dev_priv, reg, chicken);
	}

	/*
	 * Per Spec: Avoid continuous PSR exit by masking MEMUP and HPD also
	 * mask LPSP to avoid dependency on other drivers that might block
	 * runtime_pm besides preventing  other hw tracking issues now we
	 * can rely on frontbuffer tracking.
	 */
	mask = EDP_PSR_DEBUG_MASK_MEMUP |
	       EDP_PSR_DEBUG_MASK_HPD |
	       EDP_PSR_DEBUG_MASK_LPSP |
	       EDP_PSR_DEBUG_MASK_MAX_SLEEP;

	if (INTEL_GEN(dev_priv) < 11)
		mask |= EDP_PSR_DEBUG_MASK_DISP_REG_WRITE;

	intel_de_write(dev_priv, EDP_PSR_DEBUG(dev_priv->psr.transcoder),
		       mask);

	psr_irq_control(dev_priv);

	if (crtc_state->dc3co_exitline) {
		u32 val;

		/*
		 * TODO: if future platforms supports DC3CO in more than one
		 * transcoder, EXITLINE will need to be unset when disabling PSR
		 */
		val = intel_de_read(dev_priv, EXITLINE(cpu_transcoder));
		val &= ~EXITLINE_MASK;
		val |= crtc_state->dc3co_exitline << EXITLINE_SHIFT;
		val |= EXITLINE_ENABLE;
		intel_de_write(dev_priv, EXITLINE(cpu_transcoder), val);
	}
}

static void intel_psr_enable_locked(struct drm_i915_private *dev_priv,
				    const struct intel_crtc_state *crtc_state)
{
	struct intel_dp *intel_dp = dev_priv->psr.dp;
	u32 val;

	WARN_ON(dev_priv->psr.enabled);

	dev_priv->psr.psr2_enabled = intel_psr2_enabled(dev_priv, crtc_state);
	dev_priv->psr.busy_frontbuffer_bits = 0;
	dev_priv->psr.pipe = to_intel_crtc(crtc_state->uapi.crtc)->pipe;
	dev_priv->psr.dc3co_enabled = !!crtc_state->dc3co_exitline;
	dev_priv->psr.transcoder = crtc_state->cpu_transcoder;
	/* DC5/DC6 requires at least 6 idle frames */
	val = usecs_to_jiffies(intel_get_frame_time_us(crtc_state) * 6);
	dev_priv->psr.dc3co_exit_delay = val;

	/*
	 * If a PSR error happened and the driver is reloaded, the EDP_PSR_IIR
	 * will still keep the error set even after the reset done in the
	 * irq_preinstall and irq_uninstall hooks.
	 * And enabling in this situation cause the screen to freeze in the
	 * first time that PSR HW tries to activate so lets keep PSR disabled
	 * to avoid any rendering problems.
	 */
	if (INTEL_GEN(dev_priv) >= 12) {
		val = intel_de_read(dev_priv,
				    TRANS_PSR_IIR(dev_priv->psr.transcoder));
		val &= EDP_PSR_ERROR(0);
	} else {
		val = intel_de_read(dev_priv, EDP_PSR_IIR);
		val &= EDP_PSR_ERROR(dev_priv->psr.transcoder);
	}
	if (val) {
		dev_priv->psr.sink_not_reliable = true;
		DRM_DEBUG_KMS("PSR interruption error set, not enabling PSR\n");
		return;
	}

	DRM_DEBUG_KMS("Enabling PSR%s\n",
		      dev_priv->psr.psr2_enabled ? "2" : "1");
	intel_psr_setup_vsc(intel_dp, crtc_state);
	intel_psr_enable_sink(intel_dp);
	intel_psr_enable_source(intel_dp, crtc_state);
	dev_priv->psr.enabled = true;

	intel_psr_activate(intel_dp);
}

/**
 * intel_psr_enable - Enable PSR
 * @intel_dp: Intel DP
 * @crtc_state: new CRTC state
 *
 * This function can only be called after the pipe is fully trained and enabled.
 */
void intel_psr_enable(struct intel_dp *intel_dp,
		      const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (!CAN_PSR(dev_priv) || dev_priv->psr.dp != intel_dp)
		return;

	dev_priv->psr.force_mode_changed = false;

	if (!crtc_state->has_psr)
		return;

	WARN_ON(dev_priv->drrs.dp);

	mutex_lock(&dev_priv->psr.lock);

	if (!psr_global_enabled(dev_priv->psr.debug)) {
		DRM_DEBUG_KMS("PSR disabled by flag\n");
		goto unlock;
	}

	intel_psr_enable_locked(dev_priv, crtc_state);

unlock:
	mutex_unlock(&dev_priv->psr.lock);
}

static void intel_psr_exit(struct drm_i915_private *dev_priv)
{
	u32 val;

	if (!dev_priv->psr.active) {
		if (transcoder_has_psr2(dev_priv, dev_priv->psr.transcoder)) {
			val = intel_de_read(dev_priv,
					    EDP_PSR2_CTL(dev_priv->psr.transcoder));
			WARN_ON(val & EDP_PSR2_ENABLE);
		}

		val = intel_de_read(dev_priv,
				    EDP_PSR_CTL(dev_priv->psr.transcoder));
		WARN_ON(val & EDP_PSR_ENABLE);

		return;
	}

	if (dev_priv->psr.psr2_enabled) {
		tgl_disallow_dc3co_on_psr2_exit(dev_priv);
		val = intel_de_read(dev_priv,
				    EDP_PSR2_CTL(dev_priv->psr.transcoder));
		WARN_ON(!(val & EDP_PSR2_ENABLE));
		val &= ~EDP_PSR2_ENABLE;
		intel_de_write(dev_priv,
			       EDP_PSR2_CTL(dev_priv->psr.transcoder), val);
	} else {
		val = intel_de_read(dev_priv,
				    EDP_PSR_CTL(dev_priv->psr.transcoder));
		WARN_ON(!(val & EDP_PSR_ENABLE));
		val &= ~EDP_PSR_ENABLE;
		intel_de_write(dev_priv,
			       EDP_PSR_CTL(dev_priv->psr.transcoder), val);
	}
	dev_priv->psr.active = false;
}

static void intel_psr_disable_locked(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	i915_reg_t psr_status;
	u32 psr_status_mask;

	lockdep_assert_held(&dev_priv->psr.lock);

	if (!dev_priv->psr.enabled)
		return;

	DRM_DEBUG_KMS("Disabling PSR%s\n",
		      dev_priv->psr.psr2_enabled ? "2" : "1");

	intel_psr_exit(dev_priv);

	if (dev_priv->psr.psr2_enabled) {
		psr_status = EDP_PSR2_STATUS(dev_priv->psr.transcoder);
		psr_status_mask = EDP_PSR2_STATUS_STATE_MASK;
	} else {
		psr_status = EDP_PSR_STATUS(dev_priv->psr.transcoder);
		psr_status_mask = EDP_PSR_STATUS_STATE_MASK;
	}

	/* Wait till PSR is idle */
	if (intel_de_wait_for_clear(dev_priv, psr_status,
				    psr_status_mask, 2000))
		DRM_ERROR("Timed out waiting PSR idle state\n");

	/* Disable PSR on Sink */
	drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_EN_CFG, 0);

	if (dev_priv->psr.psr2_enabled)
		drm_dp_dpcd_writeb(&intel_dp->aux, DP_RECEIVER_ALPM_CONFIG, 0);

	dev_priv->psr.enabled = false;
}

/**
 * intel_psr_disable - Disable PSR
 * @intel_dp: Intel DP
 * @old_crtc_state: old CRTC state
 *
 * This function needs to be called before disabling pipe.
 */
void intel_psr_disable(struct intel_dp *intel_dp,
		       const struct intel_crtc_state *old_crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (!old_crtc_state->has_psr)
		return;

	if (WARN_ON(!CAN_PSR(dev_priv)))
		return;

	mutex_lock(&dev_priv->psr.lock);

	intel_psr_disable_locked(intel_dp);

	mutex_unlock(&dev_priv->psr.lock);
	cancel_work_sync(&dev_priv->psr.work);
	cancel_delayed_work_sync(&dev_priv->psr.idle_work);
}

static void psr_force_hw_tracking_exit(struct drm_i915_private *dev_priv)
{
	if (INTEL_GEN(dev_priv) >= 9)
		/*
		 * Display WA #0884: skl+
		 * This documented WA for bxt can be safely applied
		 * broadly so we can force HW tracking to exit PSR
		 * instead of disabling and re-enabling.
		 * Workaround tells us to write 0 to CUR_SURFLIVE_A,
		 * but it makes more sense write to the current active
		 * pipe.
		 */
		intel_de_write(dev_priv, CURSURFLIVE(dev_priv->psr.pipe), 0);
	else
		/*
		 * A write to CURSURFLIVE do not cause HW tracking to exit PSR
		 * on older gens so doing the manual exit instead.
		 */
		intel_psr_exit(dev_priv);
}

/**
 * intel_psr_update - Update PSR state
 * @intel_dp: Intel DP
 * @crtc_state: new CRTC state
 *
 * This functions will update PSR states, disabling, enabling or switching PSR
 * version when executing fastsets. For full modeset, intel_psr_disable() and
 * intel_psr_enable() should be called instead.
 */
void intel_psr_update(struct intel_dp *intel_dp,
		      const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct i915_psr *psr = &dev_priv->psr;
	bool enable, psr2_enable;

	if (!CAN_PSR(dev_priv) || READ_ONCE(psr->dp) != intel_dp)
		return;

	dev_priv->psr.force_mode_changed = false;

	mutex_lock(&dev_priv->psr.lock);

	enable = crtc_state->has_psr && psr_global_enabled(psr->debug);
	psr2_enable = intel_psr2_enabled(dev_priv, crtc_state);

	if (enable == psr->enabled && psr2_enable == psr->psr2_enabled) {
		/* Force a PSR exit when enabling CRC to avoid CRC timeouts */
		if (crtc_state->crc_enabled && psr->enabled)
			psr_force_hw_tracking_exit(dev_priv);
		else if (INTEL_GEN(dev_priv) < 9 && psr->enabled) {
			/*
			 * Activate PSR again after a force exit when enabling
			 * CRC in older gens
			 */
			if (!dev_priv->psr.active &&
			    !dev_priv->psr.busy_frontbuffer_bits)
				schedule_work(&dev_priv->psr.work);
		}

		goto unlock;
	}

	if (psr->enabled)
		intel_psr_disable_locked(intel_dp);

	if (enable)
		intel_psr_enable_locked(dev_priv, crtc_state);

unlock:
	mutex_unlock(&dev_priv->psr.lock);
}

/**
 * intel_psr_wait_for_idle - wait for PSR1 to idle
 * @new_crtc_state: new CRTC state
 * @out_value: PSR status in case of failure
 *
 * This function is expected to be called from pipe_update_start() where it is
 * not expected to race with PSR enable or disable.
 *
 * Returns: 0 on success or -ETIMEOUT if PSR status does not idle.
 */
int intel_psr_wait_for_idle(const struct intel_crtc_state *new_crtc_state,
			    u32 *out_value)
{
	struct intel_crtc *crtc = to_intel_crtc(new_crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	if (!dev_priv->psr.enabled || !new_crtc_state->has_psr)
		return 0;

	/* FIXME: Update this for PSR2 if we need to wait for idle */
	if (READ_ONCE(dev_priv->psr.psr2_enabled))
		return 0;

	/*
	 * From bspec: Panel Self Refresh (BDW+)
	 * Max. time for PSR to idle = Inverse of the refresh rate + 6 ms of
	 * exit training time + 1.5 ms of aux channel handshake. 50 ms is
	 * defensive enough to cover everything.
	 */

	return __intel_wait_for_register(&dev_priv->uncore,
					 EDP_PSR_STATUS(dev_priv->psr.transcoder),
					 EDP_PSR_STATUS_STATE_MASK,
					 EDP_PSR_STATUS_STATE_IDLE, 2, 50,
					 out_value);
}

static bool __psr_wait_for_idle_locked(struct drm_i915_private *dev_priv)
{
	i915_reg_t reg;
	u32 mask;
	int err;

	if (!dev_priv->psr.enabled)
		return false;

	if (dev_priv->psr.psr2_enabled) {
		reg = EDP_PSR2_STATUS(dev_priv->psr.transcoder);
		mask = EDP_PSR2_STATUS_STATE_MASK;
	} else {
		reg = EDP_PSR_STATUS(dev_priv->psr.transcoder);
		mask = EDP_PSR_STATUS_STATE_MASK;
	}

	mutex_unlock(&dev_priv->psr.lock);

	err = intel_de_wait_for_clear(dev_priv, reg, mask, 50);
	if (err)
		DRM_ERROR("Timed out waiting for PSR Idle for re-enable\n");

	/* After the unlocked wait, verify that PSR is still wanted! */
	mutex_lock(&dev_priv->psr.lock);
	return err == 0 && dev_priv->psr.enabled;
}

static int intel_psr_fastset_force(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct intel_crtc *crtc;
	int err;

	state = drm_atomic_state_alloc(dev);
	if (!state)
		return -ENOMEM;

	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);
	state->acquire_ctx = &ctx;

retry:
	for_each_intel_crtc(dev, crtc) {
		struct intel_crtc_state *crtc_state =
			intel_atomic_get_crtc_state(state, crtc);

		if (IS_ERR(crtc_state)) {
			err = PTR_ERR(crtc_state);
			goto error;
		}

		if (crtc_state->hw.active && crtc_state->has_psr) {
			/* Mark mode as changed to trigger a pipe->update() */
			crtc_state->uapi.mode_changed = true;
			break;
		}
	}

	err = drm_atomic_commit(state);

error:
	if (err == -EDEADLK) {
		drm_atomic_state_clear(state);
		err = drm_modeset_backoff(&ctx);
		if (!err)
			goto retry;
	}

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	drm_atomic_state_put(state);

	return err;
}

int intel_psr_debug_set(struct drm_i915_private *dev_priv, u64 val)
{
	const u32 mode = val & I915_PSR_DEBUG_MODE_MASK;
	u32 old_mode;
	int ret;

	if (val & ~(I915_PSR_DEBUG_IRQ | I915_PSR_DEBUG_MODE_MASK) ||
	    mode > I915_PSR_DEBUG_FORCE_PSR1) {
		DRM_DEBUG_KMS("Invalid debug mask %llx\n", val);
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&dev_priv->psr.lock);
	if (ret)
		return ret;

	old_mode = dev_priv->psr.debug & I915_PSR_DEBUG_MODE_MASK;
	dev_priv->psr.debug = val;

	/*
	 * Do it right away if it's already enabled, otherwise it will be done
	 * when enabling the source.
	 */
	if (dev_priv->psr.enabled)
		psr_irq_control(dev_priv);

	mutex_unlock(&dev_priv->psr.lock);

	if (old_mode != mode)
		ret = intel_psr_fastset_force(dev_priv);

	return ret;
}

static void intel_psr_handle_irq(struct drm_i915_private *dev_priv)
{
	struct i915_psr *psr = &dev_priv->psr;

	intel_psr_disable_locked(psr->dp);
	psr->sink_not_reliable = true;
	/* let's make sure that sink is awaken */
	drm_dp_dpcd_writeb(&psr->dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
}

static void intel_psr_work(struct work_struct *work)
{
	struct drm_i915_private *dev_priv =
		container_of(work, typeof(*dev_priv), psr.work);

	mutex_lock(&dev_priv->psr.lock);

	if (!dev_priv->psr.enabled)
		goto unlock;

	if (READ_ONCE(dev_priv->psr.irq_aux_error))
		intel_psr_handle_irq(dev_priv);

	/*
	 * We have to make sure PSR is ready for re-enable
	 * otherwise it keeps disabled until next full enable/disable cycle.
	 * PSR might take some time to get fully disabled
	 * and be ready for re-enable.
	 */
	if (!__psr_wait_for_idle_locked(dev_priv))
		goto unlock;

	/*
	 * The delayed work can race with an invalidate hence we need to
	 * recheck. Since psr_flush first clears this and then reschedules we
	 * won't ever miss a flush when bailing out here.
	 */
	if (dev_priv->psr.busy_frontbuffer_bits || dev_priv->psr.active)
		goto unlock;

	intel_psr_activate(dev_priv->psr.dp);
unlock:
	mutex_unlock(&dev_priv->psr.lock);
}

/**
 * intel_psr_invalidate - Invalidade PSR
 * @dev_priv: i915 device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 * @origin: which operation caused the invalidate
 *
 * Since the hardware frontbuffer tracking has gaps we need to integrate
 * with the software frontbuffer tracking. This function gets called every
 * time frontbuffer rendering starts and a buffer gets dirtied. PSR must be
 * disabled if the frontbuffer mask contains a buffer relevant to PSR.
 *
 * Dirty frontbuffers relevant to PSR are tracked in busy_frontbuffer_bits."
 */
void intel_psr_invalidate(struct drm_i915_private *dev_priv,
			  unsigned frontbuffer_bits, enum fb_op_origin origin)
{
	if (!CAN_PSR(dev_priv))
		return;

	if (origin == ORIGIN_FLIP)
		return;

	mutex_lock(&dev_priv->psr.lock);
	if (!dev_priv->psr.enabled) {
		mutex_unlock(&dev_priv->psr.lock);
		return;
	}

	frontbuffer_bits &= INTEL_FRONTBUFFER_ALL_MASK(dev_priv->psr.pipe);
	dev_priv->psr.busy_frontbuffer_bits |= frontbuffer_bits;

	if (frontbuffer_bits)
		intel_psr_exit(dev_priv);

	mutex_unlock(&dev_priv->psr.lock);
}

/*
 * When we will be completely rely on PSR2 S/W tracking in future,
 * intel_psr_flush() will invalidate and flush the PSR for ORIGIN_FLIP
 * event also therefore tgl_dc3co_flush() require to be changed
 * accrodingly in future.
 */
static void
tgl_dc3co_flush(struct drm_i915_private *dev_priv,
		unsigned int frontbuffer_bits, enum fb_op_origin origin)
{
	mutex_lock(&dev_priv->psr.lock);

	if (!dev_priv->psr.dc3co_enabled)
		goto unlock;

	if (!dev_priv->psr.psr2_enabled || !dev_priv->psr.active)
		goto unlock;

	/*
	 * At every frontbuffer flush flip event modified delay of delayed work,
	 * when delayed work schedules that means display has been idle.
	 */
	if (!(frontbuffer_bits &
	    INTEL_FRONTBUFFER_ALL_MASK(dev_priv->psr.pipe)))
		goto unlock;

	tgl_psr2_enable_dc3co(dev_priv);
	mod_delayed_work(system_wq, &dev_priv->psr.idle_work,
			 dev_priv->psr.dc3co_exit_delay);

unlock:
	mutex_unlock(&dev_priv->psr.lock);
}

/**
 * intel_psr_flush - Flush PSR
 * @dev_priv: i915 device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 * @origin: which operation caused the flush
 *
 * Since the hardware frontbuffer tracking has gaps we need to integrate
 * with the software frontbuffer tracking. This function gets called every
 * time frontbuffer rendering has completed and flushed out to memory. PSR
 * can be enabled again if no other frontbuffer relevant to PSR is dirty.
 *
 * Dirty frontbuffers relevant to PSR are tracked in busy_frontbuffer_bits.
 */
void intel_psr_flush(struct drm_i915_private *dev_priv,
		     unsigned frontbuffer_bits, enum fb_op_origin origin)
{
	if (!CAN_PSR(dev_priv))
		return;

	if (origin == ORIGIN_FLIP) {
		tgl_dc3co_flush(dev_priv, frontbuffer_bits, origin);
		return;
	}

	mutex_lock(&dev_priv->psr.lock);
	if (!dev_priv->psr.enabled) {
		mutex_unlock(&dev_priv->psr.lock);
		return;
	}

	frontbuffer_bits &= INTEL_FRONTBUFFER_ALL_MASK(dev_priv->psr.pipe);
	dev_priv->psr.busy_frontbuffer_bits &= ~frontbuffer_bits;

	/* By definition flush = invalidate + flush */
	if (frontbuffer_bits)
		psr_force_hw_tracking_exit(dev_priv);

	if (!dev_priv->psr.active && !dev_priv->psr.busy_frontbuffer_bits)
		schedule_work(&dev_priv->psr.work);
	mutex_unlock(&dev_priv->psr.lock);
}

/**
 * intel_psr_init - Init basic PSR work and mutex.
 * @dev_priv: i915 device private
 *
 * This function is  called only once at driver load to initialize basic
 * PSR stuff.
 */
void intel_psr_init(struct drm_i915_private *dev_priv)
{
	if (!HAS_PSR(dev_priv))
		return;

	if (!dev_priv->psr.sink_support)
		return;

	if (IS_HASWELL(dev_priv))
		/*
		 * HSW don't have PSR registers on the same space as transcoder
		 * so set this to a value that when subtract to the register
		 * in transcoder space results in the right offset for HSW
		 */
		dev_priv->hsw_psr_mmio_adjust = _SRD_CTL_EDP - _HSW_EDP_PSR_BASE;

	if (i915_modparams.enable_psr == -1)
		if (INTEL_GEN(dev_priv) < 9 || !dev_priv->vbt.psr.enable)
			i915_modparams.enable_psr = 0;

	/* Set link_standby x link_off defaults */
	if (IS_HASWELL(dev_priv) || IS_BROADWELL(dev_priv))
		/* HSW and BDW require workarounds that we don't implement. */
		dev_priv->psr.link_standby = false;
	else if (INTEL_GEN(dev_priv) < 12)
		/* For new platforms up to TGL let's respect VBT back again */
		dev_priv->psr.link_standby = dev_priv->vbt.psr.full_link;

	INIT_WORK(&dev_priv->psr.work, intel_psr_work);
	INIT_DELAYED_WORK(&dev_priv->psr.idle_work, tgl_dc5_idle_thread);
	mutex_init(&dev_priv->psr.lock);
}

static int psr_get_status_and_error_status(struct intel_dp *intel_dp,
					   u8 *status, u8 *error_status)
{
	struct drm_dp_aux *aux = &intel_dp->aux;
	int ret;

	ret = drm_dp_dpcd_readb(aux, DP_PSR_STATUS, status);
	if (ret != 1)
		return ret;

	ret = drm_dp_dpcd_readb(aux, DP_PSR_ERROR_STATUS, error_status);
	if (ret != 1)
		return ret;

	*status = *status & DP_PSR_SINK_STATE_MASK;

	return 0;
}

static void psr_alpm_check(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct drm_dp_aux *aux = &intel_dp->aux;
	struct i915_psr *psr = &dev_priv->psr;
	u8 val;
	int r;

	if (!psr->psr2_enabled)
		return;

	r = drm_dp_dpcd_readb(aux, DP_RECEIVER_ALPM_STATUS, &val);
	if (r != 1) {
		DRM_ERROR("Error reading ALPM status\n");
		return;
	}

	if (val & DP_ALPM_LOCK_TIMEOUT_ERROR) {
		intel_psr_disable_locked(intel_dp);
		psr->sink_not_reliable = true;
		DRM_DEBUG_KMS("ALPM lock timeout error, disabling PSR\n");

		/* Clearing error */
		drm_dp_dpcd_writeb(aux, DP_RECEIVER_ALPM_STATUS, val);
	}
}

static void psr_capability_changed_check(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct i915_psr *psr = &dev_priv->psr;
	u8 val;
	int r;

	r = drm_dp_dpcd_readb(&intel_dp->aux, DP_PSR_ESI, &val);
	if (r != 1) {
		DRM_ERROR("Error reading DP_PSR_ESI\n");
		return;
	}

	if (val & DP_PSR_CAPS_CHANGE) {
		intel_psr_disable_locked(intel_dp);
		psr->sink_not_reliable = true;
		DRM_DEBUG_KMS("Sink PSR capability changed, disabling PSR\n");

		/* Clearing it */
		drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_ESI, val);
	}
}

void intel_psr_short_pulse(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct i915_psr *psr = &dev_priv->psr;
	u8 status, error_status;
	const u8 errors = DP_PSR_RFB_STORAGE_ERROR |
			  DP_PSR_VSC_SDP_UNCORRECTABLE_ERROR |
			  DP_PSR_LINK_CRC_ERROR;

	if (!CAN_PSR(dev_priv) || !intel_dp_is_edp(intel_dp))
		return;

	mutex_lock(&psr->lock);

	if (!psr->enabled || psr->dp != intel_dp)
		goto exit;

	if (psr_get_status_and_error_status(intel_dp, &status, &error_status)) {
		DRM_ERROR("Error reading PSR status or error status\n");
		goto exit;
	}

	if (status == DP_PSR_SINK_INTERNAL_ERROR || (error_status & errors)) {
		intel_psr_disable_locked(intel_dp);
		psr->sink_not_reliable = true;
	}

	if (status == DP_PSR_SINK_INTERNAL_ERROR && !error_status)
		DRM_DEBUG_KMS("PSR sink internal error, disabling PSR\n");
	if (error_status & DP_PSR_RFB_STORAGE_ERROR)
		DRM_DEBUG_KMS("PSR RFB storage error, disabling PSR\n");
	if (error_status & DP_PSR_VSC_SDP_UNCORRECTABLE_ERROR)
		DRM_DEBUG_KMS("PSR VSC SDP uncorrectable error, disabling PSR\n");
	if (error_status & DP_PSR_LINK_CRC_ERROR)
		DRM_DEBUG_KMS("PSR Link CRC error, disabling PSR\n");

	if (error_status & ~errors)
		DRM_ERROR("PSR_ERROR_STATUS unhandled errors %x\n",
			  error_status & ~errors);
	/* clear status register */
	drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_ERROR_STATUS, error_status);

	psr_alpm_check(intel_dp);
	psr_capability_changed_check(intel_dp);

exit:
	mutex_unlock(&psr->lock);
}

bool intel_psr_enabled(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	bool ret;

	if (!CAN_PSR(dev_priv) || !intel_dp_is_edp(intel_dp))
		return false;

	mutex_lock(&dev_priv->psr.lock);
	ret = (dev_priv->psr.dp == intel_dp && dev_priv->psr.enabled);
	mutex_unlock(&dev_priv->psr.lock);

	return ret;
}

void intel_psr_atomic_check(struct drm_connector *connector,
			    struct drm_connector_state *old_state,
			    struct drm_connector_state *new_state)
{
	struct drm_i915_private *dev_priv = to_i915(connector->dev);
	struct intel_connector *intel_connector;
	struct intel_digital_port *dig_port;
	struct drm_crtc_state *crtc_state;

	if (!CAN_PSR(dev_priv) || !new_state->crtc ||
	    !dev_priv->psr.force_mode_changed)
		return;

	intel_connector = to_intel_connector(connector);
	dig_port = enc_to_dig_port(intel_attached_encoder(intel_connector));
	if (dev_priv->psr.dp != &dig_port->dp)
		return;

	crtc_state = drm_atomic_get_new_crtc_state(new_state->state,
						   new_state->crtc);
	crtc_state->mode_changed = true;
}

void intel_psr_set_force_mode_changed(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;

	if (!intel_dp)
		return;

	dev_priv = dp_to_i915(intel_dp);
	if (!CAN_PSR(dev_priv) || intel_dp != dev_priv->psr.dp)
		return;

	dev_priv->psr.force_mode_changed = true;
}
