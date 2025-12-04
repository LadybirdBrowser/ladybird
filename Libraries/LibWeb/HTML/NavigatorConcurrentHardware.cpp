/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibWeb/HTML/NavigatorConcurrentHardware.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#dom-navigator-hardwareconcurrency
WebIDL::UnsignedLongLong NavigatorConcurrentHardwareMixin::hardware_concurrency()
{
    return Core::System::hardware_concurrency();
}

}
