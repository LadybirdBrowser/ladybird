/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
};

}
