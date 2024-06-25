/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>

#pragma once

namespace Unicode {

String current_time_zone();
Vector<String> const& available_time_zones();
Vector<String> available_time_zones_in_region(StringView region);
Optional<String> resolve_primary_time_zone(StringView time_zone);

}
