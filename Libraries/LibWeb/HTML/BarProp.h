/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/BarProp.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#barprop
class BarProp final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(BarProp, bar_prop)

public:
    [[nodiscard]] static GC::Ref<BarProp> create(Window&);
};

}
