/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibCore/Export.h>

namespace Core::TimeZone {

CORE_API ErrorOr<void> set_current_time_zone(StringView);

CORE_API String current_time_zone();

}
