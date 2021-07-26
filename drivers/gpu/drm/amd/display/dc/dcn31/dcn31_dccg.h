/*
 * Copyright 2018 Advanced Micro Devices, Inc.
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

#ifndef __DCN31_DCCG_H__
#define __DCN31_DCCG_H__

#include "dcn30/dcn30_dccg.h"

#define DCCG_SFII(block, reg_name, field_prefix, field_name, inst, post_fix)\
	.field_prefix ## _ ## field_name[inst] = block ## inst ## _ ## reg_name ## __ ## field_prefix ## inst ## _ ## field_name ## post_fix


#define DCCG_REG_LIST_DCN31() \
	SR(DPPCLK_DTO_CTRL),\
	DCCG_SRII(DTO_PARAM, DPPCLK, 0),\
	DCCG_SRII(DTO_PARAM, DPPCLK, 1),\
	DCCG_SRII(DTO_PARAM, DPPCLK, 2),\
	DCCG_SRII(DTO_PARAM, DPPCLK, 3),\
	SR(PHYASYMCLK_CLOCK_CNTL),\
	SR(PHYBSYMCLK_CLOCK_CNTL),\
	SR(PHYCSYMCLK_CLOCK_CNTL),\
	SR(PHYDSYMCLK_CLOCK_CNTL),\
	SR(PHYESYMCLK_CLOCK_CNTL),\
	SR(DPSTREAMCLK_CNTL),\
	SR(SYMCLK32_SE_CNTL),\
	SR(SYMCLK32_LE_CNTL),\
	DCCG_SRII(PIXEL_RATE_CNTL, OTG, 0),\
	DCCG_SRII(PIXEL_RATE_CNTL, OTG, 1),\
	DCCG_SRII(PIXEL_RATE_CNTL, OTG, 2),\
	DCCG_SRII(PIXEL_RATE_CNTL, OTG, 3),\
	DCCG_SRII(MODULO, DTBCLK_DTO, 0),\
	DCCG_SRII(MODULO, DTBCLK_DTO, 1),\
	DCCG_SRII(MODULO, DTBCLK_DTO, 2),\
	DCCG_SRII(MODULO, DTBCLK_DTO, 3),\
	DCCG_SRII(PHASE, DTBCLK_DTO, 0),\
	DCCG_SRII(PHASE, DTBCLK_DTO, 1),\
	DCCG_SRII(PHASE, DTBCLK_DTO, 2),\
	DCCG_SRII(PHASE, DTBCLK_DTO, 3),\
	SR(DCCG_AUDIO_DTBCLK_DTO_MODULO),\
	SR(DCCG_AUDIO_DTBCLK_DTO_PHASE),\
	SR(DCCG_AUDIO_DTO_SOURCE),\
	SR(DENTIST_DISPCLK_CNTL)


#define DCCG_MASK_SH_LIST_DCN31(mask_sh) \
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_ENABLE, DPPCLK, 0, mask_sh),\
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_DB_EN, DPPCLK, 0, mask_sh),\
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_ENABLE, DPPCLK, 1, mask_sh),\
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_DB_EN, DPPCLK, 1, mask_sh),\
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_ENABLE, DPPCLK, 2, mask_sh),\
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_DB_EN, DPPCLK, 2, mask_sh),\
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_ENABLE, DPPCLK, 3, mask_sh),\
	DCCG_SFI(DPPCLK_DTO_CTRL, DTO_DB_EN, DPPCLK, 3, mask_sh),\
	DCCG_SF(DPPCLK0_DTO_PARAM, DPPCLK0_DTO_PHASE, mask_sh),\
	DCCG_SF(DPPCLK0_DTO_PARAM, DPPCLK0_DTO_MODULO, mask_sh),\
	DCCG_SF(PHYASYMCLK_CLOCK_CNTL, PHYASYMCLK_FORCE_EN, mask_sh),\
	DCCG_SF(PHYASYMCLK_CLOCK_CNTL, PHYASYMCLK_FORCE_SRC_SEL, mask_sh),\
	DCCG_SF(PHYBSYMCLK_CLOCK_CNTL, PHYBSYMCLK_FORCE_EN, mask_sh),\
	DCCG_SF(PHYBSYMCLK_CLOCK_CNTL, PHYBSYMCLK_FORCE_SRC_SEL, mask_sh),\
	DCCG_SF(PHYCSYMCLK_CLOCK_CNTL, PHYCSYMCLK_FORCE_EN, mask_sh),\
	DCCG_SF(PHYCSYMCLK_CLOCK_CNTL, PHYCSYMCLK_FORCE_SRC_SEL, mask_sh),\
	DCCG_SF(PHYDSYMCLK_CLOCK_CNTL, PHYDSYMCLK_FORCE_EN, mask_sh),\
	DCCG_SF(PHYDSYMCLK_CLOCK_CNTL, PHYDSYMCLK_FORCE_SRC_SEL, mask_sh),\
	DCCG_SF(PHYESYMCLK_CLOCK_CNTL, PHYESYMCLK_FORCE_EN, mask_sh),\
	DCCG_SF(PHYESYMCLK_CLOCK_CNTL, PHYESYMCLK_FORCE_SRC_SEL, mask_sh),\
	DCCG_SF(DPSTREAMCLK_CNTL, DPSTREAMCLK_PIPE0_EN, mask_sh),\
	DCCG_SF(DPSTREAMCLK_CNTL, DPSTREAMCLK_PIPE1_EN, mask_sh),\
	DCCG_SF(DPSTREAMCLK_CNTL, DPSTREAMCLK_PIPE2_EN, mask_sh),\
	DCCG_SF(DPSTREAMCLK_CNTL, DPSTREAMCLK_PIPE3_EN, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE0_SRC_SEL, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE1_SRC_SEL, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE2_SRC_SEL, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE3_SRC_SEL, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE0_EN, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE1_EN, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE2_EN, mask_sh),\
	DCCG_SF(SYMCLK32_SE_CNTL, SYMCLK32_SE3_EN, mask_sh),\
	DCCG_SF(SYMCLK32_LE_CNTL, SYMCLK32_LE0_SRC_SEL, mask_sh),\
	DCCG_SF(SYMCLK32_LE_CNTL, SYMCLK32_LE1_SRC_SEL, mask_sh),\
	DCCG_SF(SYMCLK32_LE_CNTL, SYMCLK32_LE0_EN, mask_sh),\
	DCCG_SF(SYMCLK32_LE_CNTL, SYMCLK32_LE1_EN, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, ENABLE, 0, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, ENABLE, 1, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, ENABLE, 2, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, ENABLE, 3, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLKDTO, ENABLE_STATUS, 0, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLKDTO, ENABLE_STATUS, 1, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLKDTO, ENABLE_STATUS, 2, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLKDTO, ENABLE_STATUS, 3, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, PIPE, DTO_SRC_SEL, 0, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, PIPE, DTO_SRC_SEL, 1, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, PIPE, DTO_SRC_SEL, 2, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, PIPE, DTO_SRC_SEL, 3, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, DIV, 0, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, DIV, 1, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, DIV, 2, mask_sh),\
	DCCG_SFII(OTG, PIXEL_RATE_CNTL, DTBCLK_DTO, DIV, 3, mask_sh),\
	DCCG_SF(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO_SEL, mask_sh),\
	DCCG_SF(DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO0_SOURCE_SEL, mask_sh),\
	DCCG_SF(DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_CHG_MODE, mask_sh)


struct dccg *dccg31_create(
	struct dc_context *ctx,
	const struct dccg_registers *regs,
	const struct dccg_shift *dccg_shift,
	const struct dccg_mask *dccg_mask);

void dccg31_init(struct dccg *dccg);

void dccg31_enable_symclk32_se(
		struct dccg *dccg,
		int hpo_se_inst,
		enum phyd32clk_clock_source phyd32clk);

void dccg31_disable_symclk32_se(
		struct dccg *dccg,
		int hpo_se_inst);

void dccg31_enable_symclk32_le(
		struct dccg *dccg,
		int hpo_le_inst,
		enum phyd32clk_clock_source phyd32clk);

void dccg31_disable_symclk32_le(
		struct dccg *dccg,
		int hpo_le_inst);

void dccg31_set_physymclk(
		struct dccg *dccg,
		int phy_inst,
		enum physymclk_clock_source clk_src,
		bool force_enable);

void dccg31_set_audio_dtbclk_dto(
		struct dccg *dccg,
		uint32_t req_audio_dtbclk_khz);

void dccg31_set_hdmistreamclk(
		struct dccg *dccg,
		enum hdmistreamclk_source src);

#endif //__DCN31_DCCG_H__
