/* Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/reboot.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/hrtimer.h>
#include <mach/cpufreq.h>

#define DEFAULT_POLLING_MS	150
#define CPU_FREQ_MAX	1890000

static DEFINE_MUTEX(emergency_shutdown_mutex);

static int mako_enabled;

//Throttling indicator, 0=not throttled, 1=low, 2=mid, 3=max
int bricked_thermal_throttled = 0;
EXPORT_SYMBOL_GPL(bricked_thermal_throttled);

//Save the cpu max freq before throttling
static int pre_throttled_max = 0;

static struct msm_thermal_data msm_thermal_info = {
	.sensor_id = 0,
	.poll_ms = DEFAULT_POLLING_MS,
	.shutdown_temp = 88,
	.allowed_max_high = 84,
	.allowed_max_low = 80,
	.allowed_max_freq = 384000,
	.allowed_mid_high = 81,
	.allowed_mid_low = 76,
	.allowed_mid_freq = 810000,
	.allowed_low_high = 79,
	.allowed_low_low = 73,
	.allowed_low_freq = 1350000,
};

#ifdef CONFIG_MAKO_THERMAL_STATS
static struct msm_thermal_stat msm_thermal_stats = {
    .time_low_start = 0,
    .time_mid_start = 0,
    .time_max_start = 0,
    .time_low = 0,
    .time_mid = 0,
    .time_max = 0,
};
#endif

static struct delayed_work check_temp_work;
static struct workqueue_struct *check_temp_workq;

#ifdef CONFIG_MAKO_THERMAL_STATS
static void update_stats(void)
{
    if (msm_thermal_stats.time_low_start > 0) {
        msm_thermal_stats.time_low += (ktime_to_ms(ktime_get()) - msm_thermal_stats.time_low_start);
        msm_thermal_stats.time_low_start = 0;
    }
    if (msm_thermal_stats.time_mid_start > 0) {
        msm_thermal_stats.time_mid += (ktime_to_ms(ktime_get()) - msm_thermal_stats.time_mid_start);
        msm_thermal_stats.time_mid_start = 0;
    }
    if (msm_thermal_stats.time_max_start > 0) {
        msm_thermal_stats.time_max += (ktime_to_ms(ktime_get()) - msm_thermal_stats.time_max_start);
        msm_thermal_stats.time_max_start = 0;
    }
}
#endif

#ifdef CONFIG_MAKO_THERMAL_STATS
static void start_stats(int status)
{
    switch (bricked_thermal_throttled) {
        case 1:
            msm_thermal_stats.time_low_start = ktime_to_ms(ktime_get());
            break;
        case 2:
            msm_thermal_stats.time_mid_start = ktime_to_ms(ktime_get());
            break;
        case 3:
            msm_thermal_stats.time_max_start = ktime_to_ms(ktime_get());
            break;
    }
}
#endif

static int update_cpu_max_freq(struct cpufreq_policy *cpu_policy,
                   int cpu, int max_freq)
{
    int ret = 0;

    if (!cpu_policy)
        return -EINVAL;

    cpufreq_verify_within_limits(cpu_policy, cpu_policy->min, max_freq);
    cpu_policy->user_policy.max = max_freq;

    ret = cpufreq_update_policy(cpu);
    if (!ret)
        pr_debug("msm_thermal: Setting CPU%d max frequency to %d\n",
                 cpu, max_freq);
    return ret;
}

static __ref void check_temp(struct work_struct *work)
{
    struct cpufreq_policy *cpu_policy = NULL;
    struct tsens_device tsens_dev;
    unsigned long temp = 0;
    uint32_t max_freq = 0;
    bool update_policy = false;
    int i = 0, cpu = 0, ret = 0;

    tsens_dev.sensor_num = msm_thermal_info.sensor_id;
    ret = tsens_get_temp(&tsens_dev, &temp);
    if (ret) {
        pr_err("msm_thermal: FATAL: Unable to read TSENS sensor %d\n",
               tsens_dev.sensor_num);
        goto reschedule;
    }

    if (temp >= msm_thermal_info.shutdown_temp) {
        mutex_lock(&emergency_shutdown_mutex);
        pr_warn("################################\n");
        pr_warn("################################\n");
        pr_warn("- %u OVERTEMP! SHUTTING DOWN! -\n", msm_thermal_info.shutdown_temp);
        pr_warn("- cur temp:%lu measured by:%u -\n", temp, msm_thermal_info.sensor_id);
        pr_warn("################################\n");
        pr_warn("################################\n");
        /* orderly poweroff tries to power down gracefully
           if it fails it will force it. */
        orderly_poweroff(true);
        for_each_possible_cpu(cpu) {
            update_policy = true;
            max_freq = msm_thermal_info.allowed_max_freq;
            bricked_thermal_throttled = 3;
            pr_warn("msm_thermal: Emergency throttled CPU%i to %u! temp:%lu\n",
                    cpu, msm_thermal_info.allowed_max_freq, temp);
        }
        mutex_unlock(&emergency_shutdown_mutex);
    }

    for_each_possible_cpu(cpu) {
        update_policy = false;
        cpu_policy = cpufreq_cpu_get(cpu);
        if (!cpu_policy) {
            pr_debug("msm_thermal: NULL policy on cpu %d\n", cpu);
            continue;
        }

        /* save pre-throttled max freq value */
        if ((bricked_thermal_throttled == 0) && (cpu == 0))
            pre_throttled_max = cpu_policy->max;

        //low trip point
        if ((temp >= msm_thermal_info.allowed_low_high) &&
            (temp < msm_thermal_info.allowed_mid_high) &&
            (bricked_thermal_throttled < 1)) {
            update_policy = true;
            max_freq = msm_thermal_info.allowed_low_freq;
            if (cpu == (CONFIG_NR_CPUS-1)) {
                bricked_thermal_throttled = 1;
                pr_warn("msm_thermal: Thermal Throttled (low)! temp:%lu by:%u\n",
                        temp, msm_thermal_info.sensor_id);
            }
        //low clr point
        } else if ((temp < msm_thermal_info.allowed_low_low) &&
               (bricked_thermal_throttled > 0)) {
            if (pre_throttled_max != 0)
                max_freq = pre_throttled_max;
            else {
                max_freq = CPU_FREQ_MAX;
                pr_warn("msm_thermal: ERROR! pre_throttled_max=0, falling back to %u\n", max_freq);
            }
            update_policy = true;
            for (i = 1; i < CONFIG_NR_CPUS; i++) {
                if (cpu_online(i))
                        continue;
                cpu_up(i);
            }
            if (cpu == (CONFIG_NR_CPUS-1)) {
                bricked_thermal_throttled = 0;
                pr_warn("msm_thermal: Low thermal throttle ended! temp:%lu by:%u\n",
                        temp, msm_thermal_info.sensor_id);
            }


        //mid trip point
        } else if ((temp >= msm_thermal_info.allowed_mid_high) &&
               (temp < msm_thermal_info.allowed_max_high) &&
               (bricked_thermal_throttled < 2)) {
            update_policy = true;
            max_freq = msm_thermal_info.allowed_mid_freq;
            if (cpu == (CONFIG_NR_CPUS-1)) {
                bricked_thermal_throttled = 2;
                pr_warn("msm_thermal: Thermal Throttled (mid)! temp:%lu by:%u\n",
                        temp, msm_thermal_info.sensor_id);
            }
        //mid clr point
        } else if ((temp < msm_thermal_info.allowed_mid_low) &&
               (bricked_thermal_throttled > 1)) {
            max_freq = msm_thermal_info.allowed_low_freq;
            update_policy = true;
            if (cpu == (CONFIG_NR_CPUS-1)) {
                bricked_thermal_throttled = 1;
                pr_warn("msm_thermal: Mid thermal throttle ended! temp:%lu by:%u\n",
                        temp, msm_thermal_info.sensor_id);
            }
        //max trip point
        } else if (temp >= msm_thermal_info.allowed_max_high) {
            update_policy = true;
            max_freq = msm_thermal_info.allowed_max_freq;
            if (cpu == (CONFIG_NR_CPUS-1)) {
                bricked_thermal_throttled = 3;
                pr_warn("msm_thermal: Thermal Throttled (max)! temp:%lu by:%u\n",
                        temp, msm_thermal_info.sensor_id);
            }
        //max clr point
        } else if ((temp < msm_thermal_info.allowed_max_low) &&
               (bricked_thermal_throttled > 2)) {
            max_freq = msm_thermal_info.allowed_mid_freq;
            update_policy = true;
            if (cpu == (CONFIG_NR_CPUS-1)) {
                bricked_thermal_throttled = 2;
                pr_warn("msm_thermal: Max thermal throttle ended! temp:%lu by:%u\n",
                        temp, msm_thermal_info.sensor_id);
            }
        }
#ifdef CONFIG_MAKO_THERMAL_STATS
        update_stats();
        start_stats(bricked_thermal_throttled);
#endif
        if (update_policy)
            update_cpu_max_freq(cpu_policy, cpu, max_freq);

        cpufreq_cpu_put(cpu_policy);
    }

reschedule:
    if (mako_enabled)
        queue_delayed_work(check_temp_workq, &check_temp_work,
                           msecs_to_jiffies(msm_thermal_info.poll_ms));

    return;
}

static void disable_msm_thermal(void)
{
    int cpu = 0;
    struct cpufreq_policy *cpu_policy = NULL;

     mako_enabled = 0;
    /* make sure check_temp is no longer running */
    cancel_delayed_work_sync(&check_temp_work);
 
    if (pre_throttled_max != 0) {
        for_each_possible_cpu(cpu) {
            cpu_policy = cpufreq_cpu_get(cpu);
            if (cpu_policy) {
                if (cpu_policy->max < cpu_policy->cpuinfo.max_freq)
                    update_cpu_max_freq(cpu_policy, cpu, pre_throttled_max);
                cpufreq_cpu_put(cpu_policy);
            }
        }
    }

   pr_warn("msm_thermal: Warning! Thermal guard disabled!");
}

static void enable_msm_thermal(void)
{
    mako_enabled = 1;
    /* make sure check_temp is running */
    queue_delayed_work(check_temp_workq, &check_temp_work,
                       msecs_to_jiffies(msm_thermal_info.poll_ms));

    pr_info("msm_thermal: Thermal guard enabled.");
}

static int set_enabled(const char *val, const struct kernel_param *kp)
{
    int ret = 0;

    ret = param_set_bool(val, kp);
    if (!mako_enabled)
        disable_msm_thermal();
    else if (mako_enabled == 1)
        enable_msm_thermal();
    else
        pr_info("msm_thermal: no action for enabled = %d\n", mako_enabled);

    pr_info("msm_thermal: enabled = %d\n", mako_enabled);

    return ret;
}

static struct kernel_param_ops module_ops = {
    .set = set_enabled,
    .get = param_get_bool,
};

module_param_cb(mako_enabled, &module_ops, &mako_enabled, 0644);
MODULE_PARM_DESC(mako_enabled, "enforce thermal limit on cpu");

/**************************** SYSFS START ****************************/
struct kobject *msm_thermal_kobject;

#define show_one(file_name, object)                             \
static ssize_t show_##file_name                                 \
(struct kobject *kobj, struct attribute *attr, char *buf)       \
{                                                               \
    return sprintf(buf, "%u\n", msm_thermal_info.object);       \
}

show_one(shutdown_temp, shutdown_temp);
show_one(allowed_max_high, allowed_max_high);
show_one(allowed_max_low, allowed_max_low);
show_one(allowed_max_freq, allowed_max_freq);
show_one(allowed_mid_high, allowed_mid_high);
show_one(allowed_mid_low, allowed_mid_low);
show_one(allowed_mid_freq, allowed_mid_freq);
show_one(allowed_low_high, allowed_low_high);
show_one(allowed_low_low, allowed_low_low);
show_one(allowed_low_freq, allowed_low_freq);
show_one(poll_ms, poll_ms);

static ssize_t store_shutdown_temp(struct kobject *a, struct attribute *b,
                                   const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.shutdown_temp = input;

    return count;
}

static ssize_t store_allowed_max_high(struct kobject *a, struct attribute *b,
                                      const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_max_high = input;

    return count;
}

static ssize_t store_allowed_max_low(struct kobject *a, struct attribute *b,
                                     const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_max_low = input;

    return count;
}

static ssize_t store_allowed_max_freq(struct kobject *a, struct attribute *b,
                                      const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_max_freq = input;

    return count;
}

static ssize_t store_allowed_mid_high(struct kobject *a, struct attribute *b,
                                      const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_mid_high = input;

    return count;
}

static ssize_t store_allowed_mid_low(struct kobject *a, struct attribute *b,
                                     const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_mid_low = input;

    return count;
}

static ssize_t store_allowed_mid_freq(struct kobject *a, struct attribute *b,
                                      const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_mid_freq = input;

    return count;
}

static ssize_t store_allowed_low_high(struct kobject *a, struct attribute *b,
                                      const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_low_high = input;

    return count;
}

static ssize_t store_allowed_low_low(struct kobject *a, struct attribute *b,
                                     const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_low_low = input;

    return count;
}

static ssize_t store_allowed_low_freq(struct kobject *a, struct attribute *b,
                                      const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.allowed_low_freq = input;

    return count;
}

static ssize_t store_poll_ms(struct kobject *a, struct attribute *b,
                             const char *buf, size_t count)
{
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);
    if (ret != 1)
        return -EINVAL;

    msm_thermal_info.poll_ms = input;

    return count;
}

define_one_global_rw(shutdown_temp);
define_one_global_rw(allowed_max_high);
define_one_global_rw(allowed_max_low);
define_one_global_rw(allowed_max_freq);
define_one_global_rw(allowed_mid_high);
define_one_global_rw(allowed_mid_low);
define_one_global_rw(allowed_mid_freq);
define_one_global_rw(allowed_low_high);
define_one_global_rw(allowed_low_low);
define_one_global_rw(allowed_low_freq);
define_one_global_rw(poll_ms);

static struct attribute *msm_thermal_attributes[] = {
    &shutdown_temp.attr,
    &allowed_max_high.attr,
    &allowed_max_low.attr,
    &allowed_max_freq.attr,
    &allowed_mid_high.attr,
    &allowed_mid_low.attr,
    &allowed_mid_freq.attr,
    &allowed_low_high.attr,
    &allowed_low_low.attr,
    &allowed_low_freq.attr,
    &poll_ms.attr,
    NULL
};


static struct attribute_group msm_thermal_attr_group = {
    .attrs = msm_thermal_attributes,
    .name = "conf",
};

/********* STATS START *********/

#ifdef CONFIG_MAKO_THERMAL_STATS
static ssize_t show_throttle_times(struct kobject *a, struct attribute *b,
                                 char *buf)
{
    ssize_t len = 0;

    if (bricked_thermal_throttled == 1) {
        len += sprintf(buf + len, "%s %llu\n", "low",
                       (msm_thermal_stats.time_low +
                        (ktime_to_ms(ktime_get()) -
                         msm_thermal_stats.time_low_start)));
    } else
        len += sprintf(buf + len, "%s %llu\n", "low", msm_thermal_stats.time_low);

    if (bricked_thermal_throttled == 2) {
        len += sprintf(buf + len, "%s %llu\n", "mid",
                       (msm_thermal_stats.time_mid +
                        (ktime_to_ms(ktime_get()) -
                         msm_thermal_stats.time_mid_start)));
    } else
        len += sprintf(buf + len, "%s %llu\n", "mid", msm_thermal_stats.time_mid);

    if (bricked_thermal_throttled == 3) {
        len += sprintf(buf + len, "%s %llu\n", "max",
                       (msm_thermal_stats.time_max +
                        (ktime_to_ms(ktime_get()) -
                         msm_thermal_stats.time_max_start)));
    } else
        len += sprintf(buf + len, "%s %llu\n", "max", msm_thermal_stats.time_max);

    return len;
}
define_one_global_ro(throttle_times);

static ssize_t show_is_throttled(struct kobject *a, struct attribute *b,
                                 char *buf)
{
    return sprintf(buf, "%u\n", bricked_thermal_throttled);
}
define_one_global_ro(is_throttled);

static struct attribute *msm_thermal_stats_attributes[] = {
    &is_throttled.attr,
    &throttle_times.attr,
    NULL
};

static struct attribute_group msm_thermal_stats_attr_group = {
    .attrs = msm_thermal_stats_attributes,
    .name = "stats",
};
#endif
/**************************** SYSFS END ****************************/

int __init msm_thermal_init(struct msm_thermal_data *pdata)
{
    int ret = 0, rc = 0;

    mako_enabled = 1;
    check_temp_workq=alloc_workqueue("msm_thermal", WQ_UNBOUND | WQ_RESCUER, 1);
    if (!check_temp_workq)
        BUG_ON(ENOMEM);
    INIT_DELAYED_WORK(&check_temp_work, check_temp);
    queue_delayed_work(check_temp_workq, &check_temp_work, 1000);

    msm_thermal_kobject = kobject_create_and_add("msm_thermal", kernel_kobj);
    if (msm_thermal_kobject) {
        rc = sysfs_create_group(msm_thermal_kobject, &msm_thermal_attr_group);
        if (rc) {
            pr_warn("msm_thermal: sysfs: ERROR, could not create sysfs group");
        }
#ifdef CONFIG_MAKO_THERMAL_STATS
        rc = sysfs_create_group(msm_thermal_kobject,
                                &msm_thermal_stats_attr_group);
        if (rc) {
            pr_warn("msm_thermal: sysfs: ERROR, could not create sysfs stats group");
        }
#endif
    } else
        pr_warn("msm_thermal: sysfs: ERROR, could not create sysfs kobj");

    return ret;
}
