/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibWeb/HTML/NavigatorConcurrentHardware.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#dom-navigator-hardwareconcurrency
WebIDL::UnsignedLongLong NavigatorConcurrentHardwareMixin::hardware_concurrency()
{
    // The navigator.hardwareConcurrency attribute's getter must return a number between 1 and the number of logical
    // processors potentially available to the user agent. If this cannot be determined, the getter must return 1.
    // User agents should err toward exposing the number of logical processors available, using lower values only in
    // cases where there are user-agent specific limits in place (such as a limitation on the number of workers that
    // can be created) or when the user agent desires to limit fingerprinting possibilities.
    auto logical_processors = Core::System::hardware_concurrency();

    // In WebKit mode, the count is capped the way the claimed engine caps it: 8 on a machine with eight or more
    // logical processors, and 4 on the rest. The real count would single the navigator out as not that engine. But
    // the count never exceeds the real one, which the spec's range above forbids.
    if (ResourceLoader::navigator_compatibility_mode() == NavigatorCompatibilityMode::WebKit)
        return min(logical_processors, logical_processors < 8 ? 4u : 8u);

    return logical_processors;
}

}
