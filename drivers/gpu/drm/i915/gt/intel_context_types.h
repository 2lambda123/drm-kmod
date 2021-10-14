/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_CONTEXT_TYPES__
#define __INTEL_CONTEXT_TYPES__

#include <linux/average.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "i915_active_types.h"
#include "i915_sw_fence.h"
#include "i915_utils.h"
#include "intel_engine_types.h"
#include "intel_sseu.h"

#ifdef __FreeBSD__
#include <linux/poison.h>
#endif

#include "uc/intel_guc_fwif.h"

#define CONTEXT_REDZONE POISON_INUSE
DECLARE_EWMA(runtime, 3, 8);

struct i915_gem_context;
struct i915_gem_ww_ctx;
struct i915_vma;
struct intel_breadcrumbs;
struct intel_context;
struct intel_ring;

struct intel_context_ops {
	unsigned long flags;
#define COPS_HAS_INFLIGHT_BIT 0
#define COPS_HAS_INFLIGHT BIT(COPS_HAS_INFLIGHT_BIT)

	int (*alloc)(struct intel_context *ce);

	void (*ban)(struct intel_context *ce, struct i915_request *rq);

	int (*pre_pin)(struct intel_context *ce, struct i915_gem_ww_ctx *ww, void **vaddr);
	int (*pin)(struct intel_context *ce, void *vaddr);
	void (*unpin)(struct intel_context *ce);
	void (*post_unpin)(struct intel_context *ce);

	void (*cancel_request)(struct intel_context *ce,
			       struct i915_request *rq);

	void (*enter)(struct intel_context *ce);
	void (*exit)(struct intel_context *ce);

	void (*sched_disable)(struct intel_context *ce);

	void (*reset)(struct intel_context *ce);
	void (*destroy)(struct kref *kref);

	/* virtual engine/context interface */
	struct intel_context *(*create_virtual)(struct intel_engine_cs **engine,
						unsigned int count);
	struct intel_engine_cs *(*get_sibling)(struct intel_engine_cs *engine,
					       unsigned int sibling);
};

struct intel_context {
	/*
	 * Note: Some fields may be accessed under RCU.
	 *
	 * Unless otherwise noted a field can safely be assumed to be protected
	 * by strong reference counting.
	 */
	union {
		struct kref ref; /* no kref_get_unless_zero()! */
		struct rcu_head rcu;
	};

	struct intel_engine_cs *engine;
	struct intel_engine_cs *inflight;
#define __intel_context_inflight(engine) ptr_mask_bits(engine, 3)
#define __intel_context_inflight_count(engine) ptr_unmask_bits(engine, 3)
#define intel_context_inflight(ce) \
	__intel_context_inflight(READ_ONCE((ce)->inflight))
#define intel_context_inflight_count(ce) \
	__intel_context_inflight_count(READ_ONCE((ce)->inflight))

	struct i915_address_space *vm;
	struct i915_gem_context __rcu *gem_context;

	/*
	 * @signal_lock protects the list of requests that need signaling,
	 * @signals. While there are any requests that need signaling,
	 * we add the context to the breadcrumbs worker, and remove it
	 * upon completion/cancellation of the last request.
	 */
	struct list_head signal_link; /* Accessed under RCU */
	struct list_head signals; /* Guarded by signal_lock */
	spinlock_t signal_lock; /* protects signals, the list of requests */

	struct i915_vma *state;
	u32 ring_size;
	struct intel_ring *ring;
	struct intel_timeline *timeline;

	unsigned long flags;
#define CONTEXT_BARRIER_BIT		0
#define CONTEXT_ALLOC_BIT		1
#define CONTEXT_INIT_BIT		2
#define CONTEXT_VALID_BIT		3
#define CONTEXT_CLOSED_BIT		4
#define CONTEXT_USE_SEMAPHORES		5
#define CONTEXT_BANNED			6
#define CONTEXT_FORCE_SINGLE_SUBMISSION	7
#define CONTEXT_NOPREEMPT		8
#define CONTEXT_LRCA_DIRTY		9
#define CONTEXT_GUC_INIT		10

	struct {
		u64 timeout_us;
	} watchdog;

	u32 *lrc_reg_state;
	union {
		struct {
			u32 lrca;
			u32 ccid;
		};
		u64 desc;
	} lrc;
	u32 tag; /* cookie passed to HW to track this context on submission */

	/* Time on GPU as tracked by the hw. */
	struct {
		struct ewma_runtime avg;
		u64 total;
		u32 last;
		I915_SELFTEST_DECLARE(u32 num_underflow);
		I915_SELFTEST_DECLARE(u32 max_underflow);
	} runtime;

	unsigned int active_count; /* protected by timeline->mutex */

	atomic_t pin_count;
	struct mutex pin_mutex; /* guards pinning and associated on-gpuing */

	/**
	 * active: Active tracker for the rq activity (inc. external) on this
	 * intel_context object.
	 */
	struct i915_active active;

	const struct intel_context_ops *ops;

	/** sseu: Control eu/slice partitioning */
	struct intel_sseu sseu;

	/**
	 * pinned_contexts_link: List link for the engine's pinned contexts.
	 * This is only used if this is a perma-pinned kernel context and
	 * the list is assumed to only be manipulated during driver load
	 * or unload time so no mutex protection currently.
	 */
	struct list_head pinned_contexts_link;

	u8 wa_bb_page; /* if set, page num reserved for context workarounds */

	struct {
		/** @lock: protects everything in guc_state */
		spinlock_t lock;
		/**
		 * @sched_state: scheduling state of this context using GuC
		 * submission
		 */
		u32 sched_state;
		/*
		 * @fences: maintains a list of requests that are currently
		 * being fenced until a GuC operation completes
		 */
		struct list_head fences;
		/**
		 * @blocked: fence used to signal when the blocking of a
		 * context's submissions is complete.
		 */
		struct i915_sw_fence blocked;
		/** @number_committed_requests: number of committed requests */
		int number_committed_requests;
		/** @requests: list of active requests on this context */
		struct list_head requests;
		/** @prio: the context's current guc priority */
		u8 prio;
		/**
		 * @prio_count: a counter of the number requests in flight in
		 * each priority bucket
		 */
		u32 prio_count[GUC_CLIENT_PRIORITY_NUM];
	} guc_state;

	struct {
		/**
		 * @id: handle which is used to uniquely identify this context
		 * with the GuC, protected by guc->submission_state.lock
		 */
		u16 id;
		/**
		 * @ref: the number of references to the guc_id, when
		 * transitioning in and out of zero protected by
		 * guc->submission_state.lock
		 */
		atomic_t ref;
		/**
		 * @link: in guc->guc_id_list when the guc_id has no refs but is
		 * still valid, protected by guc->submission_state.lock
		 */
		struct list_head link;
	} guc_id;

#ifdef CONFIG_DRM_I915_SELFTEST
	/**
	 * @drop_schedule_enable: Force drop of schedule enable G2H for selftest
	 */
	bool drop_schedule_enable;

	/**
	 * @drop_schedule_disable: Force drop of schedule disable G2H for
	 * selftest
	 */
	bool drop_schedule_disable;

	/**
	 * @drop_deregister: Force drop of deregister G2H for selftest
	 */
	bool drop_deregister;
#endif
};

#endif /* __INTEL_CONTEXT_TYPES__ */
