// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPUFreq governor based on scheduler-provided WALT utilization data.
 *
 * This 5.4 backport keeps the locally validated schedutil implementation and
 * registers a separate "walt" governor with independent per-policy state.
 * The approach and naming are based on Qualcomm's standalone WALT governor as
 * published by OnePlus OSS in android_kernel_oneplus_sm8550, commit
 * c462ef8ffab7a58e035ee04705b16cdfced494b1.
 *
 * Original upstream and vendor copyrights retained from that implementation:
 * Copyright (C) 2016, Intel Corporation
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Thanks to pswbuild <pswbuild@oppo.com>, OnePlus OSS and Qualcomm for making
 * the reference implementation publicly available.
 */

/* Keep all runtime state separate from the schedutil governor. */
#define schedutil_cpu_util	waltgov_cpu_util
#define schedutil_gov		walt_gov
#define CPUFREQ_GOV_NAME	"walt"
#define CPUFREQ_GOV_SKIP_SCHEDUTIL_DEFAULT

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_WALT
#define CPUFREQ_GOV_DEFAULT_ENABLED
#endif

#include "cpufreq_schedutil.c"
