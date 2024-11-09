/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/System.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class NavigatorConcurrentHardwareMixin {
public:
    // https://html.spec.whatwg.org/multipage/workers.html#dom-navigator-hardwareconcurrency
    static WebIDL::UnsignedLongLong hardware_concurrency() { return Core::System::hardware_concurrency(); }
};

}
