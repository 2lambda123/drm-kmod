/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2017-2018 Intel Corporation
 */

#include "intel_wopcm.h"
#include "i915_drv.h"

/**
 * DOC: WOPCM Layout
 *
 * The layout of the WOPCM will be fixed after writing to GuC WOPCM size and
 * offset registers whose values are calculated and determined by HuC/GuC
 * firmware size and set of hardware requirements/restrictions as shown below:
 *
 * ::
 *
 *    +=========> +====================+ <== WOPCM Top
 *    ^           |  HW contexts RSVD  |
 *    |     +===> +====================+ <== GuC WOPCM Top
 *    |     ^     |                    |
 *    |     |     |                    |
 *    |     |     |                    |
 *    |    GuC    |                    |
 *    |   WOPCM   |                    |
 *    |    Size   +--------------------+
 *  WOPCM   |     |    GuC FW RSVD     |
 *    |     |     +--------------------+
 *    |     |     |   GuC Stack RSVD   |
 *    |     |     +------------------- +
 *    |     v     |   GuC WOPCM RSVD   |
 *    |     +===> +====================+ <== GuC WOPCM base
 *    |           |     WOPCM RSVD     |
 *    |           +------------------- + <== HuC Firmware Top
 *    v           |      HuC FW        |
 *    +=========> +====================+ <== WOPCM Base
 *
 * GuC accessible WOPCM starts at GuC WOPCM base and ends at GuC WOPCM top.
 * The top part of the WOPCM is reserved for hardware contexts (e.g. RC6
 * context).
 */

/* Default WOPCM size is 2MB from Gen11, 1MB on previous platforms */
#define GEN11_WOPCM_SIZE		SZ_2M
#define GEN9_WOPCM_SIZE			SZ_1M
/* 16KB WOPCM (RSVD WOPCM) is reserved from HuC firmware top. */
#define WOPCM_RESERVED_SIZE		SZ_16K

/* 16KB reserved at the beginning of GuC WOPCM. */
#define GUC_WOPCM_RESERVED		SZ_16K
/* 8KB from GUC_WOPCM_RESERVED is reserved for GuC stack. */
#define GUC_WOPCM_STACK_RESERVED	SZ_8K

/* GuC WOPCM Offset value needs to be aligned to 16KB. */
#define GUC_WOPCM_OFFSET_ALIGNMENT	(1UL << GUC_WOPCM_OFFSET_SHIFT)

/* 24KB at the end of WOPCM is reserved for RC6 CTX on BXT. */
#define BXT_WOPCM_RC6_CTX_RESERVED	(SZ_16K + SZ_8K)
/* 36KB WOPCM reserved at the end of WOPCM on CNL. */
#define CNL_WOPCM_HW_CTX_RESERVED	(SZ_32K + SZ_4K)

/* 128KB from GUC_WOPCM_RESERVED is reserved for FW on Gen9. */
#define GEN9_GUC_FW_RESERVED	SZ_128K
#define GEN9_GUC_WOPCM_OFFSET	(GUC_WOPCM_RESERVED + GEN9_GUC_FW_RESERVED)

static inline struct drm_i915_private *wopcm_to_i915(struct intel_wopcm *wopcm)
{
	return container_of(wopcm, struct drm_i915_private, wopcm);
}

/**
 * intel_wopcm_init_early() - Early initialization of the WOPCM.
 * @wopcm: pointer to intel_wopcm.
 *
 * Setup the size of WOPCM which will be used by later on WOPCM partitioning.
 */
void intel_wopcm_init_early(struct intel_wopcm *wopcm)
{
	struct drm_i915_private *i915 = wopcm_to_i915(wopcm);

	if (!HAS_GT_UC(i915))
		return;

	if (INTEL_GEN(i915) >= 11)
		wopcm->size = GEN11_WOPCM_SIZE;
	else
		wopcm->size = GEN9_WOPCM_SIZE;

	DRM_DEV_DEBUG_DRIVER(i915->drm.dev, "WOPCM: %uK\n", wopcm->size / 1024);
}

static inline u32 context_reserved_size(struct drm_i915_private *i915)
{
	if (IS_GEN9_LP(i915))
		return BXT_WOPCM_RC6_CTX_RESERVED;
	else if (INTEL_GEN(i915) >= 10)
		return CNL_WOPCM_HW_CTX_RESERVED;
	else
		return 0;
}

static inline int gen9_check_dword_gap(u32 guc_wopcm_base, u32 guc_wopcm_size)
{
	u32 offset;

	/*
	 * GuC WOPCM size shall be at least a dword larger than the offset from
	 * WOPCM base (GuC WOPCM offset from WOPCM base + GEN9_GUC_WOPCM_OFFSET)
	 * due to hardware limitation on Gen9.
	 */
	offset = guc_wopcm_base + GEN9_GUC_WOPCM_OFFSET;
	if (offset > guc_wopcm_size ||
	    (guc_wopcm_size - offset) < sizeof(u32)) {
		DRM_ERROR("GuC WOPCM size %uKiB is too small. %uKiB needed.\n",
			  guc_wopcm_size / 1024,
			  (u32)(offset + sizeof(u32)) / 1024);
		return -E2BIG;
	}

	return 0;
}

static inline int gen9_check_huc_fw_fits(u32 guc_wopcm_size, u32 huc_fw_size)
{
	/*
	 * On Gen9 & CNL A0, hardware requires the total available GuC WOPCM
	 * size to be larger than or equal to HuC firmware size. Otherwise,
	 * firmware uploading would fail.
	 */
	if (huc_fw_size > guc_wopcm_size - GUC_WOPCM_RESERVED) {
		DRM_ERROR("HuC FW (%uKiB) won't fit in GuC WOPCM (%uKiB).\n",
			  huc_fw_size / 1024,
			  (guc_wopcm_size - GUC_WOPCM_RESERVED) / 1024);
		return -E2BIG;
	}

	return 0;
}

static inline bool check_hw_restrictions(struct drm_i915_private *i915,
					 u32 guc_wopcm_base, u32 guc_wopcm_size,
					 u32 huc_fw_size)
{
	int err = 0;

	if (IS_GEN(i915, 9))
		err = gen9_check_dword_gap(guc_wopcm_base, guc_wopcm_size);

	if (!err &&
	    (IS_GEN(i915, 9) || IS_CNL_REVID(i915, CNL_REVID_A0, CNL_REVID_A0)))
		err = gen9_check_huc_fw_fits(guc_wopcm_size, huc_fw_size);

	return !err;
}

static inline bool __check_layout(struct drm_i915_private *i915, u32 wopcm_size,
				  u32 guc_wopcm_base, u32 guc_wopcm_size,
				  u32 guc_fw_size, u32 huc_fw_size)
{
	const u32 ctx_rsvd = context_reserved_size(i915);
	u32 size;

	size = wopcm_size - ctx_rsvd;
	if (unlikely(range_overflows(guc_wopcm_base, guc_wopcm_size, size))) {
		dev_err(i915->drm.dev,
			"WOPCM: invalid GuC region layout: %uK + %uK > %uK\n",
			guc_wopcm_base / SZ_1K, guc_wopcm_size / SZ_1K,
			size / SZ_1K);
		return false;
	}

	size = guc_fw_size + GUC_WOPCM_RESERVED + GUC_WOPCM_STACK_RESERVED;
	if (unlikely(guc_wopcm_size < size)) {
		dev_err(i915->drm.dev, "WOPCM: no space for %s: %uK < %uK\n",
			intel_uc_fw_type_repr(INTEL_UC_FW_TYPE_GUC),
			guc_wopcm_size / SZ_1K, size / SZ_1K);
		return false;
	}

	size = huc_fw_size + WOPCM_RESERVED_SIZE;
	if (unlikely(guc_wopcm_base < size)) {
		dev_err(i915->drm.dev, "WOPCM: no space for %s: %uK < %uK\n",
			intel_uc_fw_type_repr(INTEL_UC_FW_TYPE_HUC),
			guc_wopcm_base / SZ_1K, size / SZ_1K);
		return false;
	}

	return check_hw_restrictions(i915, guc_wopcm_base, guc_wopcm_size,
				     huc_fw_size);
}

static bool __wopcm_regs_locked(struct intel_uncore *uncore,
				u32 *guc_wopcm_base, u32 *guc_wopcm_size)
{
	u32 reg_base = intel_uncore_read(uncore, DMA_GUC_WOPCM_OFFSET);
	u32 reg_size = intel_uncore_read(uncore, GUC_WOPCM_SIZE);

	if (!(reg_size & GUC_WOPCM_SIZE_LOCKED) ||
	    !(reg_base & GUC_WOPCM_OFFSET_VALID))
		return false;

	*guc_wopcm_base = reg_base & GUC_WOPCM_OFFSET_MASK;
	*guc_wopcm_size = reg_size & GUC_WOPCM_SIZE_MASK;
	return true;
}

/**
 * intel_wopcm_init() - Initialize the WOPCM structure.
 * @wopcm: pointer to intel_wopcm.
 *
 * This function will partition WOPCM space based on GuC and HuC firmware sizes
 * and will allocate max remaining for use by GuC. This function will also
 * enforce platform dependent hardware restrictions on GuC WOPCM offset and
 * size. It will fail the WOPCM init if any of these checks fail, so that the
 * following WOPCM registers setup and GuC firmware uploading would be aborted.
 */
void intel_wopcm_init(struct intel_wopcm *wopcm)
{
	struct drm_i915_private *i915 = wopcm_to_i915(wopcm);
	struct intel_gt *gt = &i915->gt;
	u32 guc_fw_size = intel_uc_fw_get_upload_size(&gt->uc.guc.fw);
	u32 huc_fw_size = intel_uc_fw_get_upload_size(&gt->uc.huc.fw);
	u32 ctx_rsvd = context_reserved_size(i915);
	u32 guc_wopcm_base;
	u32 guc_wopcm_size;

	if (!guc_fw_size)
		return;

	GEM_BUG_ON(!wopcm->size);
	GEM_BUG_ON(wopcm->guc.base);
	GEM_BUG_ON(wopcm->guc.size);
	GEM_BUG_ON(guc_fw_size >= wopcm->size);
	GEM_BUG_ON(huc_fw_size >= wopcm->size);
	GEM_BUG_ON(ctx_rsvd + WOPCM_RESERVED_SIZE >= wopcm->size);

	if (i915_inject_probe_failure(i915))
		return;

	if (__wopcm_regs_locked(gt->uncore, &guc_wopcm_base, &guc_wopcm_size)) {
		DRM_DEV_DEBUG_DRIVER(i915->drm.dev,
				     "GuC WOPCM is already locked [%uK, %uK)\n",
				     guc_wopcm_base / SZ_1K,
				     guc_wopcm_size / SZ_1K);
		goto check;
	}

	/*
	 * Aligned value of guc_wopcm_base will determine available WOPCM space
	 * for HuC firmware and mandatory reserved area.
	 */
	guc_wopcm_base = huc_fw_size + WOPCM_RESERVED_SIZE;
	guc_wopcm_base = ALIGN(guc_wopcm_base, GUC_WOPCM_OFFSET_ALIGNMENT);

	/*
	 * Need to clamp guc_wopcm_base now to make sure the following math is
	 * correct. Formal check of whole WOPCM layout will be done below.
	 */
	guc_wopcm_base = min(guc_wopcm_base, wopcm->size - ctx_rsvd);

	/* Aligned remainings of usable WOPCM space can be assigned to GuC. */
	guc_wopcm_size = wopcm->size - ctx_rsvd - guc_wopcm_base;
	guc_wopcm_size &= GUC_WOPCM_SIZE_MASK;

	DRM_DEV_DEBUG_DRIVER(i915->drm.dev, "Calculated GuC WOPCM [%uK, %uK)\n",
			     guc_wopcm_base / SZ_1K, guc_wopcm_size / SZ_1K);

check:
	if (__check_layout(i915, wopcm->size, guc_wopcm_base, guc_wopcm_size,
			   guc_fw_size, huc_fw_size)) {
		wopcm->guc.base = guc_wopcm_base;
		wopcm->guc.size = guc_wopcm_size;
		GEM_BUG_ON(!wopcm->guc.base);
		GEM_BUG_ON(!wopcm->guc.size);
	}
}
