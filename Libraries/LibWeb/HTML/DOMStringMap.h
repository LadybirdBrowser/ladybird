/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DOMStringMap.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#domstringmap
class DOMStringMap final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMStringMap, dom_string_map)

public:
    [[nodiscard]] static GC::Ref<DOMStringMap> create(DOM::Element&);
};

}
