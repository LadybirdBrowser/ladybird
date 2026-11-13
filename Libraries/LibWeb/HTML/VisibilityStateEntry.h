/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/VisibilityStateEntry.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#visibilitystateentry
class VisibilityStateEntry final : public PerformanceTimeline::PerformanceEntry {
    WEB_RUST_INTERFACE(VisibilityStateEntry, visibility_state_entry)

public:
    static GC::Ref<VisibilityStateEntry> create(VisibilityState, HighResolutionTime::DOMHighResTimeStamp timestamp);
};

}
