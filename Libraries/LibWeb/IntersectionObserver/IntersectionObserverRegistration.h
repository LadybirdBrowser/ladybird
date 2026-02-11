/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::IntersectionObserver {

// https://www.w3.org/TR/intersection-observer/#intersectionobserverregistration
struct IntersectionObserverRegistration {
    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverregistration-observer
    // [A]n observer property holding an IntersectionObserver.
    GC::Ref<IntersectionObserver> observer;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverregistration-observer
    // NOTE: Optional is used in place of the spec using -1 to indicate no previous index.
    // [A] previousThresholdIndex property holding a number between -1 and the length of the observer's thresholds property (inclusive).
    Optional<size_t> previous_threshold_index;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverregistration-previousisintersecting
    // [A] previousIsIntersecting property holding a boolean.
    bool previous_is_intersecting { false };
};

}
