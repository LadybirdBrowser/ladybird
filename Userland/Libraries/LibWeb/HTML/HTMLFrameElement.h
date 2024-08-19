/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

// NOTE: This element is marked as obsolete, but is still listed as required by the specification.
class HTMLFrameElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLFrameElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLFrameElement);

public:
    virtual ~HTMLFrameElement() override;

private:
    HTMLFrameElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    // ^DOM::Element
    virtual i32 default_tab_index_value() const override;
};

}
