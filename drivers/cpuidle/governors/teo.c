// SPDX-License-Identifier: GPL-2.0
/*
 * Timer events oriented CPU idle governor
 *
 * Copyright (C) 2018 - 2021 Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * The idea of this governor is based on the observation that on many systems
 * timer events are two or more orders of magnitude more frequent than any
 * other interrupts, so they are likely to be the most significant cause of CPU
 * wakeups from idle states.  Moreover, information about what happened in the
 * (relatively recent) past can be used to estimate whether or not the deepest
 * idle state with target residency within the (known) time till the closest
 * timer event, referred to as the sleep length, is likely to be suitable for
 * the upcoming CPU idle period and, if not, then which of the shallower idle
 * states to choose instead of it.
 *
 * Of course, non-timer wakeup sources are more important in some use cases
 * which can be covered by taking a few most recent idle time intervals of the
 * CPU into account.  However, even in that context it is not necessary to
 * consider idle duration values greater than the sleep length, because the
 * closest timer will ultimately wake up the CPU anyway unless it is woken up
 * earlier.
 *
 * Thus this governor estimates whether or not the prospective idle duration of
 * a CPU is likely to be significantly shorter than the sleep length and selects
 * an idle state for it accordingly.
 *
 * The computations carried out by this governor are based on using bins whose
 * boundaries are aligned with the target residency parameter values of the CPU
 * idle states provided by the cpuidle driver in the ascending order.  That is,
 * the first bin spans from 0 up to, but not including, the target residency of
 * the second idle state (idle state 1), the second bin spans from the target
 * residency of idle state 1 up to, but not including, the target residency of
 * idle state 2, the third bin spans from the target residency of idle state 2
 * up to, but not including, the target residency of idle state 3 and so on.
 * The last bin spans from the target residency of the deepest idle state
 * supplied by the driver to infinity.
 *
 * Two metrics called "hits" and "intercepts" are associated with each bin.
 * They are updated every time before selecting an idle state for the given CPU
 * in accordance with what happened last time.
 *
 * The "hits" metric reflects the relative frequency of situations in which the
 * sleep length and the idle duration measured after CPU wakeup fall into the
 * same bin (that is, the CPU appears to wake up "on time" relative to the sleep
 * length).  In turn, the "intercepts" metric reflects the relative frequency of
 * situations in which the measured idle duration is so much shorter than the
 * sleep length that the bin it falls into corresponds to an idle state
 * shallower than the one whose bin is fallen into by the sleep length (these
 * situations are referred to as "intercepts" below).
 *
 * In order to select an idle state for a CPU, the governor takes the following
 * steps (modulo the possible latency constraint that must be taken into account
 * too):
 *
 * 1. Find the deepest CPU idle state whose target residency does not exceed
 *    the current sleep length (the candidate idle state) and compute 2 sums as
 *    follows:
 *
 *    - The sum of the "hits" and "intercepts" metrics for the candidate state
 *      and all of the deeper idle states (it represents the cases in which the
 *      CPU was idle long enough to avoid being intercepted if the sleep length
 *      had been equal to the current one).
 *
 *    - The sum of the "intercepts" metrics for all of the idle states shallower
 *      than the candidate one (it represents the cases in which the CPU was not
 *      idle long enough to avoid being intercepted if the sleep length had been
 *      equal to the current one).
 *
 * 2. If the second sum is greater than the first one the CPU is likely to wake
 *    up early, so look for an alternative idle state to select.
 *
 *    - Traverse the idle states shallower than the candidate one in the
 *      descending order.
 *
 *    - For each of them compute the sum of the "intercepts" metrics over all
 *      of the idle states between it and the candidate one (including the
 *      former and excluding the latter).
 *
 *    - If each of these sums that needs to be taken into account (because the
 *      check related to it has indicated that the CPU is likely to wake up
 *      early) is greater than a half of the corresponding sum computed in step
 *      1 (which means that the target residency of the state in question had
 *      not exceeded the idle duration in over a half of the relevant cases),
 *      select the given idle state instead of the candidate one.
 *
 * 3. By default, select the candidate state.
 */

#include <linux/cpuidle.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <linux/tick.h>

/*
 * The PULSE value is added to metrics when they grow and the DECAY_SHIFT value
 * is used for decreasing metrics on a regular basis.
 */
#define PULSE		1024
#define DECAY_SHIFT	3

/**
 * struct teo_bin - Metrics used by the TEO cpuidle governor.
 * @intercepts: The "intercepts" metric.
 * @hits: The "hits" metric.
 */
struct teo_bin {
	unsigned int intercepts;
	unsigned int hits;
};

/**
 * struct teo_cpu - CPU data used by the TEO cpuidle governor.
 * @time_span_ns: Time between idle state selection and post-wakeup update.
 * @sleep_length_ns: Time till the closest timer event (at the selection time).
 * @state_bins: Idle state data bins for this CPU.
 * @total: Grand total of the "intercepts" and "hits" mertics for all bins.
 * @last_state: Idle state entered by the CPU last time.
 */
struct teo_cpu {
	s64 time_span_ns;
	s64 sleep_length_ns;
	struct teo_bin state_bins[CPUIDLE_STATE_MAX];
	unsigned int total;
	int last_state;
	s64 wfi_timeout_us;
};

static DEFINE_PER_CPU(struct teo_cpu, teo_cpus);

/**
 * teo_update - Update CPU metrics after wakeup.
 * @drv: cpuidle driver containing state data.
 * @dev: Target CPU.
 */
static void teo_update(struct cpuidle_driver *drv, struct cpuidle_device *dev)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);
	unsigned int sleep_length_us = ktime_to_us(cpu_data->sleep_length_ns);
	int i, idx_timer = 0, idx_duration = 0;
	unsigned int measured_us;

	if (cpu_data->time_span_ns >= cpu_data->sleep_length_ns) {
		/*
		 * One of the safety nets has triggered or the wakeup was close
		 * enough to the closest timer event expected at the idle state
		 * selection time to be discarded.
		 */
		measured_us = UINT_MAX;
	} else {
		unsigned int lat = drv->states[cpu_data->last_state].exit_latency;

		/*
		 * The computations below are to determine whether or not the
		 * (saved) time till the next timer event and the measured idle
		 * duration fall into the same "bin", so use last_residency_ns
		 * for that instead of time_span_ns which includes the cpuidle
		 * overhead.
		 */
		measured_us = dev->last_residency;

		/*
		 * The delay between the wakeup and the first instruction
		 * executed by the CPU is not likely to be worst-case every
		 * time, so take 1/2 of the exit latency as a very rough
		 * approximation of the average of it.
		 */
		if (measured_us >= lat)
			measured_us -= lat / 2;
		else
			measured_us /= 2;
	}

	cpu_data->total = 0;

	/*
	 * Decay the "hits" and "intercepts" metrics for all of the bins and
	 * find the bins that the sleep length and the measured idle duration
	 * fall into.
	 */
	for (i = 0; i < drv->state_count; i++) {
		int target_residency = drv->states[i].target_residency;
		struct teo_bin *bin = &cpu_data->state_bins[i];

		bin->hits -= bin->hits >> DECAY_SHIFT;
		bin->intercepts -= bin->intercepts >> DECAY_SHIFT;

		cpu_data->total += bin->hits + bin->intercepts;

		if (target_residency <= sleep_length_us) {
			idx_timer = i;
			if (target_residency <= measured_us)
				idx_duration = i;
		}
	}

	/*
	 * If the measured idle duration falls into the same bin as the sleep
	 * length, this is a "hit", so update the "hits" metric for that bin.
	 * Otherwise, update the "intercepts" metric for the bin fallen into by
	 * the measured idle duration.
	 */
	if (idx_timer == idx_duration)
		cpu_data->state_bins[idx_timer].hits += PULSE;
	else
		cpu_data->state_bins[idx_duration].intercepts += PULSE;

	cpu_data->total += PULSE;
}

static bool teo_time_ok(unsigned int interval_us)
{
	return !tick_nohz_tick_stopped() || interval_us >= TICK_USEC;
}

static unsigned int teo_middle_of_bin(int idx, struct cpuidle_driver *drv)
{
	return (drv->states[idx].target_residency +
		drv->states[idx+1].target_residency) / 2;
}

/**
 * teo_find_shallower_state - Find shallower idle state matching given duration.
 * @drv: cpuidle driver containing state data.
 * @dev: Target CPU.
 * @state_idx: Index of the capping idle state.
 * @duration_us: Idle duration value to match.
 */
static int teo_find_shallower_state(struct cpuidle_driver *drv,
				    struct cpuidle_device *dev, int state_idx,
				    int duration_us)
{
	int i;

	for (i = state_idx - 1; i >= 0; i--) {
		if (dev->states_usage[i].disable)
			continue;

		state_idx = i;
		if (drv->states[i].target_residency <= duration_us)
			break;
	}
	return state_idx;
}

/**
 * teo_select - Selects the next idle state to enter.
 * @drv: cpuidle driver containing state data.
 * @dev: Target CPU.
 * @stop_tick: Indication on whether or not to stop the scheduler tick.
 */
static int teo_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		      bool *stop_tick)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);
	int latency_req = cpuidle_governor_latency_req(dev->cpu);
	unsigned int idx_intercept_sum = 0;
	unsigned int intercept_sum = 0;
	unsigned int idx_hit_sum = 0;
	unsigned int hit_sum = 0;
	int constraint_idx = 0;
	int idx0 = 0, idx = -1;
	int i;
	int duration_us;
	ktime_t delta_tick;

	if (cpu_data->last_state >= 0) {
		teo_update(drv, dev);
		cpu_data->last_state = -1;
	}

	cpu_data->time_span_ns = local_clock();

	cpu_data->sleep_length_ns = tick_nohz_get_sleep_length(&delta_tick);
	if (cpu_data->sleep_length_ns <= 0)
		cpu_data->sleep_length_ns = S64_MAX;
	duration_us = ktime_to_us(cpu_data->sleep_length_ns);

	/* Check if there is any choice in the first place. */
	if (drv->state_count < 2) {
		idx = 0;
		goto end;
	}
	if (!dev->states_usage[0].disable) {
		idx = 0;
		if (drv->states[1].target_residency > duration_us)
			goto end;
	}

	/*
	 * Find the deepest idle state whose target residency does not exceed
	 * the current sleep length and the deepest idle state not deeper than
	 * the former whose exit latency does not exceed the current latency
	 * constraint.  Compute the sums of metrics for early wakeup pattern
	 * detection.
	 */
	for (i = 1; i < drv->state_count; i++) {
		struct teo_bin *prev_bin = &cpu_data->state_bins[i-1];
		struct cpuidle_state *s = &drv->states[i];

		/*
		 * Update the sums of idle state mertics for all of the states
		 * shallower than the current one.
		 */
		intercept_sum += prev_bin->intercepts;
		hit_sum += prev_bin->hits;

		if (dev->states_usage[i].disable)
			continue;

		if (idx < 0) {
			idx = i; /* first enabled state */
			idx0 = i;
		}

		if (s->target_residency > duration_us)
			break;

		idx = i;

		if (s->exit_latency <= latency_req)
			constraint_idx = i;

		idx_intercept_sum = intercept_sum;
		idx_hit_sum = hit_sum;
	}

	/* Avoid unnecessary overhead. */
	if (idx < 0) {
		idx = 0; /* No states enabled, must use 0. */
		goto end;
	} else if (idx == idx0) {
		goto end;
	}

	/*
	 * If the sum of the intercepts metric for all of the idle states
	 * shallower than the current candidate one (idx) is greater than the
	 * sum of the intercepts and hits metrics for the candidate state and
	 * all of the deeper states a shallower idle state is likely to be a
	 * better choice.
	 */
        if (2 * idx_intercept_sum > cpu_data->total - idx_hit_sum) {
                s64 first_suitable_span_us = duration_us;
		int first_suitable_idx = idx;

		/*
		 * Look for the deepest idle state whose target residency had
		 * not exceeded the idle duration in over a half of the relevant
		 * cases in the past.
		 *
		 * Take the possible latency constraint and duration limitation
		 * present if the tick has been stopped already into account.
		 */
		intercept_sum = 0;

		for (i = idx - 1; i >= 0; i--) {
			struct teo_bin *bin = &cpu_data->state_bins[i];
			s64 span_us;

			intercept_sum += bin->intercepts;

			span_us = teo_middle_of_bin(i, drv);

                        if (2 * intercept_sum > idx_intercept_sum) {
				if (teo_time_ok(span_us) &&
				    !dev->states_usage[i].disable) {
					idx = i;
					duration_us = span_us;
				} else {
					/*
					 * The current state is too shallow or
					 * disabled, so take the first enabled
					 * deeper state with suitable time span.
					 */
					idx = first_suitable_idx;
					duration_us = first_suitable_span_us;
				}
				break;
			}

			if (dev->states_usage[i].disable)
				continue;

			if (!teo_time_ok(span_us)) {
				/*
				 * The current state is too shallow, but if an
				 * alternative candidate state has been found,
				 * it may still turn out to be a better choice.
				 */
				if (first_suitable_idx != idx)
					continue;

				break;
			}

			first_suitable_span_us = span_us;
			first_suitable_idx = i;
		}
	}

	/*
	 * If there is a latency constraint, it may be necessary to select an
	 * idle state shallower than the current candidate one.
	 */
	if (idx > constraint_idx)
		idx = constraint_idx;

end:
	/*
	 * Don't stop the tick if the selected state is a polling one or if the
	 * expected idle duration is shorter than the tick period length.
	 */
	if (((drv->states[idx].flags & CPUIDLE_FLAG_POLLING) ||
	    duration_us < TICK_USEC) && !tick_nohz_tick_stopped()) {
		unsigned int delta_tick_us = ktime_to_us(delta_tick);

		*stop_tick = false;

		/*
		 * The tick is not going to be stopped, so if the target
		 * residency of the state to be returned is not within the time
		 * till the closest timer including the tick, try to correct
		 * that.
		 */
		if (idx > idx0 &&
		    drv->states[idx].target_residency > delta_tick_us)
			idx = teo_find_shallower_state(drv, dev, idx, delta_tick_us);
	}

	/*
	 * Set a limit to how long the CPU can remain in WFI in case of a
	 * misprediction that results in too much time spent in WFI. This way,
	 * the CPU can be kicked out of WFI and enter a deeper idle state if a
	 * deeper state fits within the residency requirement.
	 */
#define WFI_TIMEOUT_US (1 * USEC_PER_MSEC)
	cpu_data->wfi_timeout_us = 0;
	if (drv->state_count > 1 && !idx && constraint_idx) {
		if (*stop_tick)
			delta_tick = ktime_to_us(cpu_data->sleep_length_ns);

		if (delta_tick > duration_us &&
		    (delta_tick - duration_us - WFI_TIMEOUT_US) >
		    drv->states[1].target_residency)
			cpu_data->wfi_timeout_us = duration_us + WFI_TIMEOUT_US;
	}

	return idx;
}

s64 teo_wfi_timeout_us(void)
{
	struct teo_cpu *cpu_data = this_cpu_ptr(&teo_cpus);

	return cpu_data->wfi_timeout_us;
}

/**
 * teo_reflect - Note that governor data for the CPU need to be updated.
 * @dev: Target CPU.
 * @state: Entered state.
 */
static void teo_reflect(struct cpuidle_device *dev, int state)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);

	cpu_data->last_state = state;
	/*
	 * If the wakeup was not "natural", but triggered by one of the safety
	 * nets, assume that the CPU might have been idle for the entire sleep
	 * length time.
	 */
	if (dev->poll_time_limit ||
	    (tick_nohz_idle_got_tick() && cpu_data->sleep_length_ns > TICK_NSEC)) {
		dev->poll_time_limit = false;
		cpu_data->time_span_ns = cpu_data->sleep_length_ns;
	} else {
		cpu_data->time_span_ns = local_clock() - cpu_data->time_span_ns;
	}
}

/**
 * teo_enable_device - Initialize the governor's data for the target CPU.
 * @drv: cpuidle driver (not used).
 * @dev: Target CPU.
 */
static int teo_enable_device(struct cpuidle_driver *drv,
			     struct cpuidle_device *dev)
{
	struct teo_cpu *cpu_data = per_cpu_ptr(&teo_cpus, dev->cpu);

	memset(cpu_data, 0, sizeof(*cpu_data));

	return 0;
}

static struct cpuidle_governor teo_governor = {
	.name =		"teo",
	.rating =	50,
	.enable =	teo_enable_device,
	.select =	teo_select,
	.reflect =	teo_reflect,
};

static int __init teo_governor_init(void)
{
	return cpuidle_register_governor(&teo_governor);
}

postcore_initcall(teo_governor_init);
