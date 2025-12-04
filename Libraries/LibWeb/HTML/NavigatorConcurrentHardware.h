/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class NavigatorConcurrentHardwareMixin {
public:
    static WebIDL::UnsignedLongLong hardware_concurrency();
};

}
