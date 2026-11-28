/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

// NB: libweb_rust defines, allocates and destroys every HTMLUnknownElement, see Rust/src/dom/element_objects.rs.
class HTMLUnknownElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLUnknownElement, HTMLElement);

private:
    friend struct DOM::RustElementObjectHooks;

    explicit HTMLUnknownElement(DOM::VtableProbe probe)
        : HTMLElement(probe)
    {
    }
    virtual ~HTMLUnknownElement() override = default;
};

}
