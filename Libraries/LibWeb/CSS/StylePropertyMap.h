/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/StylePropertyMap.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymap
class StylePropertyMap final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(StylePropertyMap, style_property_map)

public:
    [[nodiscard]] static GC::Ref<StylePropertyMap> create(GC::Ref<CSSStyleDeclaration>);
};

}
