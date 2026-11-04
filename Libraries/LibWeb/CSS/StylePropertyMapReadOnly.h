/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/StylePropertyMapReadOnly.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
class StylePropertyMapReadOnly final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(StylePropertyMapReadOnly, style_property_map_read_only)

public:
    [[nodiscard]] static GC::Ref<StylePropertyMapReadOnly> create_computed_style(DOM::Element&);
};

}
