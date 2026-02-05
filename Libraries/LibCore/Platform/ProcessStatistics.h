/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibCore/Platform/ProcessInfo.h>

namespace Core::Platform {

struct ProcessStatistics {
    template<typename Callback>
    void for_each_process(Callback&& callback)
    {
        for (auto& process : processes)
            callback(*process);
    }

    u64 total_time_scheduled { 0 };
    Vector<NonnullOwnPtr<ProcessInfo>> processes;
};

ErrorOr<void> update_process_statistics(ProcessStatistics&);

}
