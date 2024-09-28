// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG Fast Entropy Source: Jitter RNG
 *
 * Copyright (C) 2022 - 2023, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/rng.h>
#include <linux/fips.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/types.h>

#include "lrng_definitions.h"
#include "lrng_es_aux.h"
#include "lrng_es_jent.h"
#include "lrng_es_mgr.h"

/*
 * Estimated entropy of data is a 16th of LRNG_DRNG_SECURITY_STRENGTH_BITS.
 * Albeit a full entropy assessment is provided for the noise source indicating
 * that it provides high entropy rates and considering that it deactivates
 * when it detects insufficient hardware, the chosen under estimation of
 * entropy is considered to be acceptable to all reviewers.
 */
static u32 jent_entropy = CONFIG_LRNG_JENT_ENTROPY_RATE;
#ifdef CONFIG_LRNG_RUNTIME_ES_CONFIG
module_param(jent_entropy, uint, 0644);
MODULE_PARM_DESC(jent_entropy,
		 "Entropy in bits of 256 data bits from Jitter RNG noise source");
#endif

static bool lrng_jent_initialized = false;
static struct crypto_rng *jent;

#if (CONFIG_LRNG_JENT_ENTROPY_BLOCKS != 0)

/* Entropy buffer filled by Jitter RNG thread - must be power of 2 */
#define LRNG_JENT_ENTROPY_BLOCKS_MASK (CONFIG_LRNG_JENT_ENTROPY_BLOCKS - 1)

struct jent_entropy_es {
	uint8_t e[LRNG_DRNG_INIT_SEED_SIZE_BYTES];
	uint32_t e_bits;
};

/* Buffer that is filled with Jitter RNG data by a thread. */
static struct jent_entropy_es
	lrng_jent_async[CONFIG_LRNG_JENT_ENTROPY_BLOCKS] __aligned(sizeof(u64));

/* State of each Jitter RNG buffer entry to ensure atomic access. */
enum lrng_jent_async_state {
	buffer_empty,
	buffer_filling,
	buffer_filled,
	buffer_reading,
};
static atomic_t lrng_jent_async_set[CONFIG_LRNG_JENT_ENTROPY_BLOCKS];

/* Jitter RNG buffer work handler. */
static struct work_struct lrng_jent_async_work;

/* Is the asynchronous operation enabled? */
static bool lrng_es_jent_async_enabled = true;

#else /* CONFIG_LRNG_JENT_ENTROPY_BLOCKS */

/* The asynchronous operation is disabled by compile time option. */
static bool lrng_es_jent_async_enabled = false;

#endif /* CONFIG_LRNG_JENT_ENTROPY_BLOCKS */

static u32 lrng_jent_entropylevel(u32 requested_bits)
{
	return lrng_fast_noise_entropylevel(lrng_jent_initialized ?
					    jent_entropy : 0, requested_bits);
}

static u32 lrng_jent_poolsize(void)
{
	return lrng_jent_entropylevel(lrng_security_strength());
}

static void __lrng_jent_get(u8 *e, u32 *e_bits, u32 requested_bits)
{
	int ret;
	u32 ent_bits = lrng_jent_entropylevel(requested_bits);
	unsigned long flags;
	static DEFINE_SPINLOCK(lrng_jent_lock);

	if (!lrng_jent_initialized)
		goto err;

	spin_lock_irqsave(&lrng_jent_lock, flags);
	ret = crypto_rng_get_bytes(jent, e, requested_bits >> 3);
	spin_unlock_irqrestore(&lrng_jent_lock, flags);

	if (ret) {
		pr_debug("Jitter RNG failed with %d\n", ret);
		goto err;
	}

	pr_debug("obtained %u bits of entropy from Jitter RNG noise source\n",
		 ent_bits);

	*e_bits = ent_bits;
	return;

err:
	*e_bits = 0;
}

/*
 * lrng_get_jent() - Get Jitter RNG entropy
 *
 * @eb: entropy buffer to store entropy
 * @requested_bits: requested entropy in bits
 */
static void lrng_jent_get(struct entropy_buf *eb, u32 requested_bits,
			  bool __unused)
{
	__lrng_jent_get(eb->e[lrng_ext_es_jitter],
			&eb->e_bits[lrng_ext_es_jitter], requested_bits);
}

#if (CONFIG_LRNG_JENT_ENTROPY_BLOCKS != 0)

/* Fill the Jitter RNG buffer with random data. */
static void lrng_jent_async_monitor(struct work_struct *__unused)
{
	unsigned int i, requested_bits = lrng_get_seed_entropy_osr(true);

	pr_debug("Jitter RNG block filling started\n");

	for (i = 0; i < CONFIG_LRNG_JENT_ENTROPY_BLOCKS; i++) {
		/* Ensure atomic access to the Jitter RNG buffer slot. */
		if (atomic_cmpxchg(&lrng_jent_async_set[i],
				   buffer_empty, buffer_filling) !=
		    buffer_empty)
			continue;

		/*
		 * Always gather entropy data including
		 * potential oversampling factor.
		 */
		__lrng_jent_get(lrng_jent_async[i].e,
				&lrng_jent_async[i].e_bits, requested_bits);

		atomic_set(&lrng_jent_async_set[i], buffer_filled);

		pr_debug("Jitter RNG ES monitor: filled slot %u with %u bits of entropy\n",
			 i, requested_bits);

		schedule();
	}

	pr_debug("Jitter RNG block filling completed\n");
}

static void lrng_jent_async_monitor_schedule(void)
{
	if (lrng_es_jent_async_enabled)
		schedule_work(&lrng_jent_async_work);
}

static void lrng_jent_async_fini(void)
{
	/* Reset state */
	memzero_explicit(lrng_jent_async, sizeof(lrng_jent_async));
}

/* Get Jitter RNG data from the buffer */
static void lrng_jent_async_get(struct entropy_buf *eb, uint32_t requested_bits,
				bool __unused)
{
	static atomic_t idx = ATOMIC_INIT(-1);
	unsigned int slot;

	(void)requested_bits;

	if (!lrng_jent_initialized) {
		eb->e_bits[lrng_ext_es_jitter] = 0;
		return;
	}

	/* CONFIG_LRNG_JENT_ENTROPY_BLOCKS must be a power of 2 */
	BUILD_BUG_ON((CONFIG_LRNG_JENT_ENTROPY_BLOCKS &
		      LRNG_JENT_ENTROPY_BLOCKS_MASK) != 0);

	slot = ((unsigned int)atomic_inc_return(&idx)) &
		LRNG_JENT_ENTROPY_BLOCKS_MASK;

	/* Ensure atomic access to the Jitter RNG buffer slot. */
	if (atomic_cmpxchg(&lrng_jent_async_set[slot],
			   buffer_filled, buffer_reading) != buffer_filled) {
		pr_debug("Jitter RNG ES monitor: buffer slot %u exhausted\n",
			 slot);
		lrng_jent_get(eb, requested_bits, __unused);
		lrng_jent_async_monitor_schedule();
		return;
	}

	pr_debug("Jitter RNG ES monitor: used slot %u\n", slot);
	memcpy(eb->e[lrng_ext_es_jitter], lrng_jent_async[slot].e,
	       LRNG_DRNG_INIT_SEED_SIZE_BYTES);
	eb->e_bits[lrng_ext_es_jitter] = lrng_jent_async[slot].e_bits;

	pr_debug("obtained %u bits of entropy from Jitter RNG noise source\n",
		 eb->e_bits[lrng_ext_es_jitter]);

	memzero_explicit(&lrng_jent_async[slot],
			 sizeof(struct jent_entropy_es));

	atomic_set(&lrng_jent_async_set[slot], buffer_empty);

	/* Ensure division in the following check works */
	BUILD_BUG_ON(CONFIG_LRNG_JENT_ENTROPY_BLOCKS < 4);
	if (!(slot % (CONFIG_LRNG_JENT_ENTROPY_BLOCKS / 4)) && slot)
		lrng_jent_async_monitor_schedule();
}

static void lrng_jent_get_check(struct entropy_buf *eb,
				uint32_t requested_bits, bool __unused)
{
	if (lrng_es_jent_async_enabled &&
	    (requested_bits == lrng_get_seed_entropy_osr(true))) {
		lrng_jent_async_get(eb, requested_bits, __unused);
	} else {
		lrng_jent_get(eb, requested_bits, __unused);
	}
}

static void lrng_jent_async_init(void)
{
	unsigned int i;

	if (!lrng_es_jent_async_enabled)
		return;

	for (i = 0; i < CONFIG_LRNG_JENT_ENTROPY_BLOCKS; i++)
		atomic_set(&lrng_jent_async_set[i], buffer_empty);
}

static void lrng_jent_async_init_complete(void)
{
	lrng_jent_async_init();
	INIT_WORK(&lrng_jent_async_work, lrng_jent_async_monitor);
}

#if (defined(CONFIG_SYSFS) && defined(CONFIG_LRNG_RUNTIME_ES_CONFIG))
/* Initialize or deinitialize the Jitter RNG async collection */
static int lrng_jent_async_sysfs_set(const char *val,
				     const struct kernel_param *kp)
{
	static const char val_dflt[] = "1";
	int ret;
	bool setting;

	if (!val)
		val = val_dflt;

	ret = kstrtobool(val, &setting);
	if (ret)
		return ret;

	if (setting) {
		if (!lrng_es_jent_async_enabled) {
			lrng_es_jent_async_enabled = 1;
			lrng_jent_async_init();
			pr_devel("Jitter RNG async data collection enabled\n");
			lrng_jent_async_monitor_schedule();
		}
	} else {
		if (lrng_es_jent_async_enabled) {
			lrng_es_jent_async_enabled = 0;
			lrng_jent_async_fini();
			pr_devel("Jitter RNG async data collection disabled\n");
		}
	}

	return 0;
}

static const struct kernel_param_ops lrng_jent_async_sysfs = {
	.set = lrng_jent_async_sysfs_set,
	.get = param_get_bool,
};
module_param_cb(jent_async_enabled, &lrng_jent_async_sysfs,
		&lrng_es_jent_async_enabled, 0644);
MODULE_PARM_DESC(lrng_es_jent_async_enabled,
		 "Enable Jitter RNG entropy buffer asynchronous collection");
#endif /* CONFIG_SYSFS && CONFIG_LRNG_RUNTIME_ES_CONFIG */

#else /* CONFIG_LRNG_JENT_ENTROPY_BLOCKS */

static void lrng_jent_get_check(struct entropy_buf *eb,
				uint32_t requested_bits, bool __unused)
{
	lrng_jent_get(eb, requested_bits, __unused);
}

static inline void __init lrng_jent_async_init_complete(void) { }

#endif /* CONFIG_LRNG_JENT_ENTROPY_BLOCKS */

static void lrng_jent_es_state(unsigned char *buf, size_t buflen)
{
	snprintf(buf, buflen,
		 " Available entropy: %u\n"
		 " Enabled: %s\n"
		 " Jitter RNG async collection %s\n",
		 lrng_jent_poolsize(),
		 lrng_jent_initialized ? "true" : "false",
		 lrng_es_jent_async_enabled ? "true" : "false");
}

static int __init lrng_jent_initialize(void)
{
	jent = crypto_alloc_rng("jitterentropy_rng", 0, 0);
	if (IS_ERR(jent)) {
		pr_err("Cannot allocate Jitter RNG\n");
		return PTR_ERR(jent);
	}

	lrng_jent_async_init_complete();

	lrng_jent_initialized = true;
	pr_debug("Jitter RNG working on current system\n");

	/*
	 * In FIPS mode, the Jitter RNG is defined to have full of entropy
	 * unless a different value has been specified at the command line
	 * (i.e. the user overrides the default), and the default value is
	 * larger than zero (if it is zero, it is assumed that an RBG2(P) or
	 * RBG2(NP) construction is attempted that intends to exclude the
	 * Jitter RNG).
	 */
	if (fips_enabled && CONFIG_LRNG_JENT_ENTROPY_RATE > 0 &&
	    jent_entropy == CONFIG_LRNG_JENT_ENTROPY_RATE)
		jent_entropy = LRNG_DRNG_SECURITY_STRENGTH_BITS;

	if (jent_entropy)
		lrng_force_fully_seeded();

	return 0;
}
device_initcall(lrng_jent_initialize);

struct lrng_es_cb lrng_es_jent = {
	.name			= "JitterRNG",
	.get_ent		= lrng_jent_get_check,
	.curr_entropy		= lrng_jent_entropylevel,
	.max_entropy		= lrng_jent_poolsize,
	.state			= lrng_jent_es_state,
	.reset			= NULL,
	.switch_hash		= NULL,
};
