/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if !defined(AK_OS_MACH)
#    error "This file is only available on Mach platforms"
#endif

#include <LibCore/Export.h>
#include <LibCore/Platform/ProcessStatistics.h>

namespace Core::Platform {

}
