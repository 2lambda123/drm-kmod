/*
 * Copyright 2015 Advanced Micro Devices, Inc.
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
 */

#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "cgs_common.h"
#include "smu/smu_8_0_d.h"
#include "smu/smu_8_0_sh_mask.h"
#include "smu8.h"
#include "smu8_fusion.h"
#include "cz_smumgr.h"
#include "cz_ppsmc.h"
#include "smu_ucode_xfer_cz.h"
#include "gca/gfx_8_0_d.h"
#include "gca/gfx_8_0_sh_mask.h"
#include "smumgr.h"

#define SIZE_ALIGN_32(x)    (((x) + 31) / 32 * 32)

static const enum cz_scratch_entry firmware_list[] = {
	CZ_SCRATCH_ENTRY_UCODE_ID_SDMA0,
	CZ_SCRATCH_ENTRY_UCODE_ID_SDMA1,
	CZ_SCRATCH_ENTRY_UCODE_ID_CP_CE,
	CZ_SCRATCH_ENTRY_UCODE_ID_CP_PFP,
	CZ_SCRATCH_ENTRY_UCODE_ID_CP_ME,
	CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT1,
	CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT2,
	CZ_SCRATCH_ENTRY_UCODE_ID_RLC_G,
};

static int cz_smum_get_argument(struct pp_hwmgr *hwmgr)
{
	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	return cgs_read_register(hwmgr->device,
					mmSMU_MP1_SRBM2P_ARG_0);
}

static int cz_send_msg_to_smc_async(struct pp_hwmgr *hwmgr, uint16_t msg)
{
	int result = 0;

	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	result = PHM_WAIT_FIELD_UNEQUAL(hwmgr,
					SMU_MP1_SRBM2P_RESP_0, CONTENT, 0);
	if (result != 0) {
		pr_err("cz_send_msg_to_smc_async (0x%04x) failed\n", msg);
		return result;
	}

	cgs_write_register(hwmgr->device, mmSMU_MP1_SRBM2P_RESP_0, 0);
	cgs_write_register(hwmgr->device, mmSMU_MP1_SRBM2P_MSG_0, msg);

	return 0;
}

/* Send a message to the SMC, and wait for its response.*/
static int cz_send_msg_to_smc(struct pp_hwmgr *hwmgr, uint16_t msg)
{
	int result = 0;

	result = cz_send_msg_to_smc_async(hwmgr, msg);
	if (result != 0)
		return result;

	return PHM_WAIT_FIELD_UNEQUAL(hwmgr,
					SMU_MP1_SRBM2P_RESP_0, CONTENT, 0);
}

static int cz_set_smc_sram_address(struct pp_hwmgr *hwmgr,
				     uint32_t smc_address, uint32_t limit)
{
	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	if (0 != (3 & smc_address)) {
		pr_err("SMC address must be 4 byte aligned\n");
		return -EINVAL;
	}

	if (limit <= (smc_address + 3)) {
		pr_err("SMC address beyond the SMC RAM area\n");
		return -EINVAL;
	}

	cgs_write_register(hwmgr->device, mmMP0PUB_IND_INDEX_0,
				SMN_MP1_SRAM_START_ADDR + smc_address);

	return 0;
}

static int cz_write_smc_sram_dword(struct pp_hwmgr *hwmgr,
		uint32_t smc_address, uint32_t value, uint32_t limit)
{
	int result;

	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	result = cz_set_smc_sram_address(hwmgr, smc_address, limit);
	if (!result)
		cgs_write_register(hwmgr->device, mmMP0PUB_IND_DATA_0, value);

	return result;
}

static int cz_send_msg_to_smc_with_parameter(struct pp_hwmgr *hwmgr,
					  uint16_t msg, uint32_t parameter)
{
	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	cgs_write_register(hwmgr->device, mmSMU_MP1_SRBM2P_ARG_0, parameter);

	return cz_send_msg_to_smc(hwmgr, msg);
}

static int cz_check_fw_load_finish(struct pp_hwmgr *hwmgr,
				   uint32_t firmware)
{
	int i;
	uint32_t index = SMN_MP1_SRAM_START_ADDR +
			 SMU8_FIRMWARE_HEADER_LOCATION +
			 offsetof(struct SMU8_Firmware_Header, UcodeLoadStatus);

	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	cgs_write_register(hwmgr->device, mmMP0PUB_IND_INDEX, index);

	for (i = 0; i < hwmgr->usec_timeout; i++) {
		if (firmware ==
			(cgs_read_register(hwmgr->device, mmMP0PUB_IND_DATA) & firmware))
			break;
		udelay(1);
	}

	if (i >= hwmgr->usec_timeout) {
		pr_err("SMU check loaded firmware failed.\n");
		return -EINVAL;
	}

	return 0;
}

static int cz_load_mec_firmware(struct pp_hwmgr *hwmgr)
{
	uint32_t reg_data;
	uint32_t tmp;
	int ret = 0;
	struct cgs_firmware_info info = {0};
	struct cz_smumgr *cz_smu;

	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	ret = cgs_get_firmware_info(hwmgr->device,
						CGS_UCODE_ID_CP_MEC, &info);

	if (ret)
		return -EINVAL;

	/* Disable MEC parsing/prefetching */
	tmp = cgs_read_register(hwmgr->device,
					mmCP_MEC_CNTL);
	tmp = PHM_SET_FIELD(tmp, CP_MEC_CNTL, MEC_ME1_HALT, 1);
	tmp = PHM_SET_FIELD(tmp, CP_MEC_CNTL, MEC_ME2_HALT, 1);
	cgs_write_register(hwmgr->device, mmCP_MEC_CNTL, tmp);

	tmp = cgs_read_register(hwmgr->device,
					mmCP_CPC_IC_BASE_CNTL);

	tmp = PHM_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, VMID, 0);
	tmp = PHM_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, ATC, 0);
	tmp = PHM_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, CACHE_POLICY, 0);
	tmp = PHM_SET_FIELD(tmp, CP_CPC_IC_BASE_CNTL, MTYPE, 1);
	cgs_write_register(hwmgr->device, mmCP_CPC_IC_BASE_CNTL, tmp);

	reg_data = smu_lower_32_bits(info.mc_addr) &
			PHM_FIELD_MASK(CP_CPC_IC_BASE_LO, IC_BASE_LO);
	cgs_write_register(hwmgr->device, mmCP_CPC_IC_BASE_LO, reg_data);

	reg_data = smu_upper_32_bits(info.mc_addr) &
			PHM_FIELD_MASK(CP_CPC_IC_BASE_HI, IC_BASE_HI);
	cgs_write_register(hwmgr->device, mmCP_CPC_IC_BASE_HI, reg_data);

	return 0;
}

static uint8_t cz_translate_firmware_enum_to_arg(struct pp_hwmgr *hwmgr,
			enum cz_scratch_entry firmware_enum)
{
	uint8_t ret = 0;

	switch (firmware_enum) {
	case CZ_SCRATCH_ENTRY_UCODE_ID_SDMA0:
		ret = UCODE_ID_SDMA0;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_SDMA1:
		if (hwmgr->chip_id == CHIP_STONEY)
			ret = UCODE_ID_SDMA0;
		else
			ret = UCODE_ID_SDMA1;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_CP_CE:
		ret = UCODE_ID_CP_CE;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_CP_PFP:
		ret = UCODE_ID_CP_PFP;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_CP_ME:
		ret = UCODE_ID_CP_ME;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT1:
		ret = UCODE_ID_CP_MEC_JT1;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT2:
		if (hwmgr->chip_id == CHIP_STONEY)
			ret = UCODE_ID_CP_MEC_JT1;
		else
			ret = UCODE_ID_CP_MEC_JT2;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_GMCON_RENG:
		ret = UCODE_ID_GMCON_RENG;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_RLC_G:
		ret = UCODE_ID_RLC_G;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SCRATCH:
		ret = UCODE_ID_RLC_SCRATCH;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_ARAM:
		ret = UCODE_ID_RLC_SRM_ARAM;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_DRAM:
		ret = UCODE_ID_RLC_SRM_DRAM;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_DMCU_ERAM:
		ret = UCODE_ID_DMCU_ERAM;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_DMCU_IRAM:
		ret = UCODE_ID_DMCU_IRAM;
		break;
	case CZ_SCRATCH_ENTRY_UCODE_ID_POWER_PROFILING:
		ret = TASK_ARG_INIT_MM_PWR_LOG;
		break;
	case CZ_SCRATCH_ENTRY_DATA_ID_SDMA_HALT:
	case CZ_SCRATCH_ENTRY_DATA_ID_SYS_CLOCKGATING:
	case CZ_SCRATCH_ENTRY_DATA_ID_SDMA_RING_REGS:
	case CZ_SCRATCH_ENTRY_DATA_ID_NONGFX_REINIT:
	case CZ_SCRATCH_ENTRY_DATA_ID_SDMA_START:
	case CZ_SCRATCH_ENTRY_DATA_ID_IH_REGISTERS:
		ret = TASK_ARG_REG_MMIO;
		break;
	case CZ_SCRATCH_ENTRY_SMU8_FUSION_CLKTABLE:
		ret = TASK_ARG_INIT_CLK_TABLE;
		break;
	}

	return ret;
}

static enum cgs_ucode_id cz_convert_fw_type_to_cgs(uint32_t fw_type)
{
	enum cgs_ucode_id result = CGS_UCODE_ID_MAXIMUM;

	switch (fw_type) {
	case UCODE_ID_SDMA0:
		result = CGS_UCODE_ID_SDMA0;
		break;
	case UCODE_ID_SDMA1:
		result = CGS_UCODE_ID_SDMA1;
		break;
	case UCODE_ID_CP_CE:
		result = CGS_UCODE_ID_CP_CE;
		break;
	case UCODE_ID_CP_PFP:
		result = CGS_UCODE_ID_CP_PFP;
		break;
	case UCODE_ID_CP_ME:
		result = CGS_UCODE_ID_CP_ME;
		break;
	case UCODE_ID_CP_MEC_JT1:
		result = CGS_UCODE_ID_CP_MEC_JT1;
		break;
	case UCODE_ID_CP_MEC_JT2:
		result = CGS_UCODE_ID_CP_MEC_JT2;
		break;
	case UCODE_ID_RLC_G:
		result = CGS_UCODE_ID_RLC_G;
		break;
	default:
		break;
	}

	return result;
}

static int cz_smu_populate_single_scratch_task(
			struct pp_hwmgr *hwmgr,
			enum cz_scratch_entry fw_enum,
			uint8_t type, bool is_last)
{
	uint8_t i;
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	struct TOC *toc = (struct TOC *)cz_smu->toc_buffer.kaddr;
	struct SMU_Task *task = &toc->tasks[cz_smu->toc_entry_used_count++];

	task->type = type;
	task->arg = cz_translate_firmware_enum_to_arg(hwmgr, fw_enum);
	task->next = is_last ? END_OF_TASK_LIST : cz_smu->toc_entry_used_count;

	for (i = 0; i < cz_smu->scratch_buffer_length; i++)
		if (cz_smu->scratch_buffer[i].firmware_ID == fw_enum)
			break;

	if (i >= cz_smu->scratch_buffer_length) {
		pr_err("Invalid Firmware Type\n");
		return -EINVAL;
	}

	task->addr.low = smu_lower_32_bits(cz_smu->scratch_buffer[i].mc_addr);
	task->addr.high = smu_upper_32_bits(cz_smu->scratch_buffer[i].mc_addr);
	task->size_bytes = cz_smu->scratch_buffer[i].data_size;

	if (CZ_SCRATCH_ENTRY_DATA_ID_IH_REGISTERS == fw_enum) {
		struct cz_ih_meta_data *pIHReg_restore =
		     (struct cz_ih_meta_data *)cz_smu->scratch_buffer[i].kaddr;
		pIHReg_restore->command =
			METADATA_CMD_MODE0 | METADATA_PERFORM_ON_LOAD;
	}

	return 0;
}

static int cz_smu_populate_single_ucode_load_task(
					struct pp_hwmgr *hwmgr,
					enum cz_scratch_entry fw_enum,
					bool is_last)
{
	uint8_t i;
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	struct TOC *toc = (struct TOC *)cz_smu->toc_buffer.kaddr;
	struct SMU_Task *task = &toc->tasks[cz_smu->toc_entry_used_count++];

	task->type = TASK_TYPE_UCODE_LOAD;
	task->arg = cz_translate_firmware_enum_to_arg(hwmgr, fw_enum);
	task->next = is_last ? END_OF_TASK_LIST : cz_smu->toc_entry_used_count;

	for (i = 0; i < cz_smu->driver_buffer_length; i++)
		if (cz_smu->driver_buffer[i].firmware_ID == fw_enum)
			break;

	if (i >= cz_smu->driver_buffer_length) {
		pr_err("Invalid Firmware Type\n");
		return -EINVAL;
	}

	task->addr.low = smu_lower_32_bits(cz_smu->driver_buffer[i].mc_addr);
	task->addr.high = smu_upper_32_bits(cz_smu->driver_buffer[i].mc_addr);
	task->size_bytes = cz_smu->driver_buffer[i].data_size;

	return 0;
}

static int cz_smu_construct_toc_for_rlc_aram_save(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;

	cz_smu->toc_entry_aram = cz_smu->toc_entry_used_count;
	cz_smu_populate_single_scratch_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_ARAM,
				TASK_TYPE_UCODE_SAVE, true);

	return 0;
}

static int cz_smu_initialize_toc_empty_job_list(struct pp_hwmgr *hwmgr)
{
	int i;
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	struct TOC *toc = (struct TOC *)cz_smu->toc_buffer.kaddr;

	for (i = 0; i < NUM_JOBLIST_ENTRIES; i++)
		toc->JobList[i] = (uint8_t)IGNORE_JOB;

	return 0;
}

static int cz_smu_construct_toc_for_vddgfx_enter(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	struct TOC *toc = (struct TOC *)cz_smu->toc_buffer.kaddr;

	toc->JobList[JOB_GFX_SAVE] = (uint8_t)cz_smu->toc_entry_used_count;
	cz_smu_populate_single_scratch_task(hwmgr,
				    CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SCRATCH,
				    TASK_TYPE_UCODE_SAVE, false);

	cz_smu_populate_single_scratch_task(hwmgr,
				    CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_DRAM,
				    TASK_TYPE_UCODE_SAVE, true);

	return 0;
}


static int cz_smu_construct_toc_for_vddgfx_exit(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	struct TOC *toc = (struct TOC *)cz_smu->toc_buffer.kaddr;

	toc->JobList[JOB_GFX_RESTORE] = (uint8_t)cz_smu->toc_entry_used_count;

	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_CE, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_PFP, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_ME, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT1, false);

	if (hwmgr->chip_id == CHIP_STONEY)
		cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT1, false);
	else
		cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT2, false);

	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_RLC_G, false);

	/* populate scratch */
	cz_smu_populate_single_scratch_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SCRATCH,
				TASK_TYPE_UCODE_LOAD, false);

	cz_smu_populate_single_scratch_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_ARAM,
				TASK_TYPE_UCODE_LOAD, false);

	cz_smu_populate_single_scratch_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_DRAM,
				TASK_TYPE_UCODE_LOAD, true);

	return 0;
}

static int cz_smu_construct_toc_for_power_profiling(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;

	cz_smu->toc_entry_power_profiling_index = cz_smu->toc_entry_used_count;

	cz_smu_populate_single_scratch_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_POWER_PROFILING,
				TASK_TYPE_INITIALIZE, true);
	return 0;
}

static int cz_smu_construct_toc_for_bootup(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;

	cz_smu->toc_entry_initialize_index = cz_smu->toc_entry_used_count;

	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_SDMA0, false);
	if (hwmgr->chip_id != CHIP_STONEY)
		cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_SDMA1, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_CE, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_PFP, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_ME, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT1, false);
	if (hwmgr->chip_id != CHIP_STONEY)
		cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_CP_MEC_JT2, false);
	cz_smu_populate_single_ucode_load_task(hwmgr,
				CZ_SCRATCH_ENTRY_UCODE_ID_RLC_G, true);

	return 0;
}

static int cz_smu_construct_toc_for_clock_table(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;

	cz_smu->toc_entry_clock_table = cz_smu->toc_entry_used_count;

	cz_smu_populate_single_scratch_task(hwmgr,
				CZ_SCRATCH_ENTRY_SMU8_FUSION_CLKTABLE,
				TASK_TYPE_INITIALIZE, true);

	return 0;
}

static int cz_smu_construct_toc(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;

	cz_smu->toc_entry_used_count = 0;
	cz_smu_initialize_toc_empty_job_list(hwmgr);
	cz_smu_construct_toc_for_rlc_aram_save(hwmgr);
	cz_smu_construct_toc_for_vddgfx_enter(hwmgr);
	cz_smu_construct_toc_for_vddgfx_exit(hwmgr);
	cz_smu_construct_toc_for_power_profiling(hwmgr);
	cz_smu_construct_toc_for_bootup(hwmgr);
	cz_smu_construct_toc_for_clock_table(hwmgr);

	return 0;
}

static int cz_smu_populate_firmware_entries(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	uint32_t firmware_type;
	uint32_t i;
	int ret;
	enum cgs_ucode_id ucode_id;
	struct cgs_firmware_info info = {0};

	cz_smu->driver_buffer_length = 0;

	for (i = 0; i < ARRAY_SIZE(firmware_list); i++) {

		firmware_type = cz_translate_firmware_enum_to_arg(hwmgr,
					firmware_list[i]);

		ucode_id = cz_convert_fw_type_to_cgs(firmware_type);

		ret = cgs_get_firmware_info(hwmgr->device,
							ucode_id, &info);

		if (ret == 0) {
			cz_smu->driver_buffer[i].mc_addr = info.mc_addr;

			cz_smu->driver_buffer[i].data_size = info.image_size;

			cz_smu->driver_buffer[i].firmware_ID = firmware_list[i];
			cz_smu->driver_buffer_length++;
		}
	}

	return 0;
}

static int cz_smu_populate_single_scratch_entry(
				struct pp_hwmgr *hwmgr,
				enum cz_scratch_entry scratch_type,
				uint32_t ulsize_byte,
				struct cz_buffer_entry *entry)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	uint32_t ulsize_aligned = SIZE_ALIGN_32(ulsize_byte);

	entry->data_size = ulsize_byte;
	entry->kaddr = (char *) cz_smu->smu_buffer.kaddr +
				cz_smu->smu_buffer_used_bytes;
	entry->mc_addr = cz_smu->smu_buffer.mc_addr + cz_smu->smu_buffer_used_bytes;
	entry->firmware_ID = scratch_type;

	cz_smu->smu_buffer_used_bytes += ulsize_aligned;

	return 0;
}

static int cz_download_pptable_settings(struct pp_hwmgr *hwmgr, void **table)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	unsigned long i;

	for (i = 0; i < cz_smu->scratch_buffer_length; i++) {
		if (cz_smu->scratch_buffer[i].firmware_ID
			== CZ_SCRATCH_ENTRY_SMU8_FUSION_CLKTABLE)
			break;
	}

	*table = (struct SMU8_Fusion_ClkTable *)cz_smu->scratch_buffer[i].kaddr;

	cz_send_msg_to_smc_with_parameter(hwmgr,
				PPSMC_MSG_SetClkTableAddrHi,
				smu_upper_32_bits(cz_smu->scratch_buffer[i].mc_addr));

	cz_send_msg_to_smc_with_parameter(hwmgr,
				PPSMC_MSG_SetClkTableAddrLo,
				smu_lower_32_bits(cz_smu->scratch_buffer[i].mc_addr));

	cz_send_msg_to_smc_with_parameter(hwmgr, PPSMC_MSG_ExecuteJob,
				cz_smu->toc_entry_clock_table);

	cz_send_msg_to_smc(hwmgr, PPSMC_MSG_ClkTableXferToDram);

	return 0;
}

static int cz_upload_pptable_settings(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	unsigned long i;

	for (i = 0; i < cz_smu->scratch_buffer_length; i++) {
		if (cz_smu->scratch_buffer[i].firmware_ID
				== CZ_SCRATCH_ENTRY_SMU8_FUSION_CLKTABLE)
			break;
	}

	cz_send_msg_to_smc_with_parameter(hwmgr,
				PPSMC_MSG_SetClkTableAddrHi,
				smu_upper_32_bits(cz_smu->scratch_buffer[i].mc_addr));

	cz_send_msg_to_smc_with_parameter(hwmgr,
				PPSMC_MSG_SetClkTableAddrLo,
				smu_lower_32_bits(cz_smu->scratch_buffer[i].mc_addr));

	cz_send_msg_to_smc_with_parameter(hwmgr, PPSMC_MSG_ExecuteJob,
				cz_smu->toc_entry_clock_table);

	cz_send_msg_to_smc(hwmgr, PPSMC_MSG_ClkTableXferToSmu);

	return 0;
}

static int cz_request_smu_load_fw(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu = (struct cz_smumgr *)(hwmgr->smu_backend);
	uint32_t smc_address;

	if (!hwmgr->reload_fw) {
		pr_info("skip reloading...\n");
		return 0;
	}

	cz_smu_populate_firmware_entries(hwmgr);

	cz_smu_construct_toc(hwmgr);

	smc_address = SMU8_FIRMWARE_HEADER_LOCATION +
		offsetof(struct SMU8_Firmware_Header, UcodeLoadStatus);

	cz_write_smc_sram_dword(hwmgr, smc_address, 0, smc_address+4);

	cz_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_DriverDramAddrHi,
					smu_upper_32_bits(cz_smu->toc_buffer.mc_addr));

	cz_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_DriverDramAddrLo,
					smu_lower_32_bits(cz_smu->toc_buffer.mc_addr));

	cz_send_msg_to_smc(hwmgr, PPSMC_MSG_InitJobs);

	cz_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_ExecuteJob,
					cz_smu->toc_entry_aram);
	cz_send_msg_to_smc_with_parameter(hwmgr, PPSMC_MSG_ExecuteJob,
				cz_smu->toc_entry_power_profiling_index);

	return cz_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_ExecuteJob,
					cz_smu->toc_entry_initialize_index);
}

static int cz_start_smu(struct pp_hwmgr *hwmgr)
{
	int ret = 0;
	uint32_t fw_to_check = 0;
	struct cgs_firmware_info info = {0};
	uint32_t index = SMN_MP1_SRAM_START_ADDR +
			 SMU8_FIRMWARE_HEADER_LOCATION +
			 offsetof(struct SMU8_Firmware_Header, Version);


	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	cgs_write_register(hwmgr->device, mmMP0PUB_IND_INDEX, index);
	hwmgr->smu_version = cgs_read_register(hwmgr->device, mmMP0PUB_IND_DATA);
	info.version = hwmgr->smu_version >> 8;
	cgs_get_firmware_info(hwmgr->device, CGS_UCODE_ID_SMU, &info);

	fw_to_check = UCODE_ID_RLC_G_MASK |
			UCODE_ID_SDMA0_MASK |
			UCODE_ID_SDMA1_MASK |
			UCODE_ID_CP_CE_MASK |
			UCODE_ID_CP_ME_MASK |
			UCODE_ID_CP_PFP_MASK |
			UCODE_ID_CP_MEC_JT1_MASK |
			UCODE_ID_CP_MEC_JT2_MASK;

	if (hwmgr->chip_id == CHIP_STONEY)
		fw_to_check &= ~(UCODE_ID_SDMA1_MASK | UCODE_ID_CP_MEC_JT2_MASK);

	ret = cz_request_smu_load_fw(hwmgr);
	if (ret)
		pr_err("SMU firmware load failed\n");

	cz_check_fw_load_finish(hwmgr, fw_to_check);

	ret = cz_load_mec_firmware(hwmgr);
	if (ret)
		pr_err("Mec Firmware load failed\n");

	return ret;
}

static int cz_smu_init(struct pp_hwmgr *hwmgr)
{
	int ret = 0;
	struct cz_smumgr *cz_smu;

	cz_smu = kzalloc(sizeof(struct cz_smumgr), GFP_KERNEL);
	if (cz_smu == NULL)
		return -ENOMEM;

	hwmgr->smu_backend = cz_smu;

	cz_smu->toc_buffer.data_size = 4096;
	cz_smu->smu_buffer.data_size =
		ALIGN(UCODE_ID_RLC_SCRATCH_SIZE_BYTE, 32) +
		ALIGN(UCODE_ID_RLC_SRM_ARAM_SIZE_BYTE, 32) +
		ALIGN(UCODE_ID_RLC_SRM_DRAM_SIZE_BYTE, 32) +
		ALIGN(sizeof(struct SMU8_MultimediaPowerLogData), 32) +
		ALIGN(sizeof(struct SMU8_Fusion_ClkTable), 32);

	ret = amdgpu_bo_create_kernel((struct amdgpu_device *)hwmgr->adev,
				cz_smu->toc_buffer.data_size,
				PAGE_SIZE,
				AMDGPU_GEM_DOMAIN_VRAM,
				&cz_smu->toc_buffer.handle,
				&cz_smu->toc_buffer.mc_addr,
				&cz_smu->toc_buffer.kaddr);
	if (ret)
		return -EINVAL;

	ret = amdgpu_bo_create_kernel((struct amdgpu_device *)hwmgr->adev,
				cz_smu->smu_buffer.data_size,
				PAGE_SIZE,
				AMDGPU_GEM_DOMAIN_VRAM,
				&cz_smu->smu_buffer.handle,
				&cz_smu->smu_buffer.mc_addr,
				&cz_smu->smu_buffer.kaddr);
	if (ret) {
		amdgpu_bo_free_kernel(&cz_smu->toc_buffer.handle,
					&cz_smu->toc_buffer.mc_addr,
					&cz_smu->toc_buffer.kaddr);
		return -EINVAL;
	}

	if (0 != cz_smu_populate_single_scratch_entry(hwmgr,
		CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SCRATCH,
		UCODE_ID_RLC_SCRATCH_SIZE_BYTE,
		&cz_smu->scratch_buffer[cz_smu->scratch_buffer_length++])) {
		pr_err("Error when Populate Firmware Entry.\n");
		return -1;
	}

	if (0 != cz_smu_populate_single_scratch_entry(hwmgr,
		CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_ARAM,
		UCODE_ID_RLC_SRM_ARAM_SIZE_BYTE,
		&cz_smu->scratch_buffer[cz_smu->scratch_buffer_length++])) {
		pr_err("Error when Populate Firmware Entry.\n");
		return -1;
	}
	if (0 != cz_smu_populate_single_scratch_entry(hwmgr,
		CZ_SCRATCH_ENTRY_UCODE_ID_RLC_SRM_DRAM,
		UCODE_ID_RLC_SRM_DRAM_SIZE_BYTE,
		&cz_smu->scratch_buffer[cz_smu->scratch_buffer_length++])) {
		pr_err("Error when Populate Firmware Entry.\n");
		return -1;
	}

	if (0 != cz_smu_populate_single_scratch_entry(hwmgr,
		CZ_SCRATCH_ENTRY_UCODE_ID_POWER_PROFILING,
		sizeof(struct SMU8_MultimediaPowerLogData),
		&cz_smu->scratch_buffer[cz_smu->scratch_buffer_length++])) {
		pr_err("Error when Populate Firmware Entry.\n");
		return -1;
	}

	if (0 != cz_smu_populate_single_scratch_entry(hwmgr,
		CZ_SCRATCH_ENTRY_SMU8_FUSION_CLKTABLE,
		sizeof(struct SMU8_Fusion_ClkTable),
		&cz_smu->scratch_buffer[cz_smu->scratch_buffer_length++])) {
		pr_err("Error when Populate Firmware Entry.\n");
		return -1;
	}

	return 0;
}

static int cz_smu_fini(struct pp_hwmgr *hwmgr)
{
	struct cz_smumgr *cz_smu;

	if (hwmgr == NULL || hwmgr->device == NULL)
		return -EINVAL;

	cz_smu = (struct cz_smumgr *)hwmgr->smu_backend;
	if (cz_smu) {
		amdgpu_bo_free_kernel(&cz_smu->toc_buffer.handle,
					&cz_smu->toc_buffer.mc_addr,
					&cz_smu->toc_buffer.kaddr);
		amdgpu_bo_free_kernel(&cz_smu->smu_buffer.handle,
					&cz_smu->smu_buffer.mc_addr,
					&cz_smu->smu_buffer.kaddr);
		kfree(cz_smu);
	}

	return 0;
}

static bool cz_dpm_check_smu_features(struct pp_hwmgr *hwmgr,
				unsigned long check_feature)
{
	int result;
	unsigned long features;

	result = cz_send_msg_to_smc_with_parameter(hwmgr, PPSMC_MSG_GetFeatureStatus, 0);
	if (result == 0) {
		features = smum_get_argument(hwmgr);
		if (features & check_feature)
			return true;
	}

	return false;
}

static bool cz_is_dpm_running(struct pp_hwmgr *hwmgr)
{
	if (cz_dpm_check_smu_features(hwmgr, SMU_EnabledFeatureScoreboard_SclkDpmOn))
		return true;
	return false;
}

const struct pp_smumgr_func cz_smu_funcs = {
	.smu_init = cz_smu_init,
	.smu_fini = cz_smu_fini,
	.start_smu = cz_start_smu,
	.check_fw_load_finish = cz_check_fw_load_finish,
	.request_smu_load_fw = NULL,
	.request_smu_load_specific_fw = NULL,
	.get_argument = cz_smum_get_argument,
	.send_msg_to_smc = cz_send_msg_to_smc,
	.send_msg_to_smc_with_parameter = cz_send_msg_to_smc_with_parameter,
	.download_pptable_settings = cz_download_pptable_settings,
	.upload_pptable_settings = cz_upload_pptable_settings,
	.is_dpm_running = cz_is_dpm_running,
};

