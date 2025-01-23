/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLSourceElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLSourceElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLSourceElement);

public:
    virtual ~HTMLSourceElement() override;

private:
    HTMLSourceElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void inserted() override;
    virtual void removed_from(DOM::Node* old_parent, DOM::Node& old_root) override;
};

}
