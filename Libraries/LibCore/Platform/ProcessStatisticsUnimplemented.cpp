/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Platform/ProcessStatistics.h>

namespace Core::Platform {

ErrorOr<void> update_process_statistics(ProcessStatistics&)
{
    return {};
}

}
