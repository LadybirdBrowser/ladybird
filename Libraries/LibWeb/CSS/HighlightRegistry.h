/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Span.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/HighlightRegistry.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-highlight-api-1/#highlightregistry
class WEB_API HighlightRegistry final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(HighlightRegistry, highlight_registry)

public:
    [[nodiscard]] static GC::Ref<HighlightRegistry> create(DOM::Document&);

    bool has_highlights();
    bool has_ranges();
    bool contains_range(DOM::AbstractRange&);

    // Calls back with the name and the ranges of each registered custom highlight, from the bottom of the stacking
    // order to the top.
    void for_each_highlight_in_stacking_order(Function<void(Utf16String const& name, ReadonlySpan<GC::Ref<DOM::AbstractRange>> ranges)> const&);

    static bool range_is_paintable(DOM::Document const&, DOM::AbstractRange const&);
};

}
