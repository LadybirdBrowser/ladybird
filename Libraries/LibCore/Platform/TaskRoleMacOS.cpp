/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if !defined(AK_OS_MACOS)
#    error "This file is only available on macOS"
#endif

#include <LibCore/Platform/TaskRole.h>
#include <mach/mach.h>
#include <mach/task_policy.h>

namespace Core::Platform {

static ErrorOr<void> mach_result_to_error(kern_return_t result)
{
    if (result != KERN_SUCCESS)
        return Error::from_string_view(StringView { mach_error_string(result), strlen(mach_error_string(result)) });
    return {};
}

ErrorOr<void> adopt_foreground_application_task_role()
{
    task_category_policy_data_t category_policy { .role = TASK_FOREGROUND_APPLICATION };
    TRY(mach_result_to_error(task_policy_set(mach_task_self(), TASK_CATEGORY_POLICY,
        reinterpret_cast<task_policy_t>(&category_policy), TASK_CATEGORY_POLICY_COUNT)));

    task_qos_policy qos_tiers { .task_latency_qos_tier = LATENCY_QOS_TIER_0, .task_throughput_qos_tier = THROUGHPUT_QOS_TIER_0 };
    TRY(mach_result_to_error(task_policy_set(mach_task_self(), TASK_BASE_QOS_POLICY,
        reinterpret_cast<task_policy_t>(&qos_tiers), TASK_QOS_POLICY_COUNT)));
    return {};
}

}
