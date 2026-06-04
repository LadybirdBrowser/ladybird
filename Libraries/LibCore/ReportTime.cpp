/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ReportTime.h"

#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumberFormat.h>

namespace Core {

static auto& timing_info_table()
{
    static NeverDestroyed<HashMap<ByteString, TimingInfo>> timing_info_table;
    return *timing_info_table;
}

void log_timing_info(ByteString const& name, AK::Duration const& elapsed_time, u64 print_every_n_calls)
{
    auto& timing_info = timing_info_table().ensure(name);
    timing_info.call_count++;
    timing_info.cumulative_time_nanoseconds += elapsed_time.to_nanoseconds();

    u64 average_nanoseconds = timing_info.cumulative_time_nanoseconds / timing_info.call_count;
    if (timing_info.call_count % print_every_n_calls != 0)
        return;
    dbgln("[Timing info for: {}] current: {:10} | average: {:10} | total: {:10} | calls: {}",
        name,
        human_readable_short_time(elapsed_time),
        human_readable_short_time(AK::Duration::from_nanoseconds(average_nanoseconds)),
        human_readable_short_time(AK::Duration::from_nanoseconds(timing_info.cumulative_time_nanoseconds)),
        timing_info.call_count);
}

}
