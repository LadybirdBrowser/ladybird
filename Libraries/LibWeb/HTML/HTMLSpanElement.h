/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLSpanElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLSpanElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // https://www.w3.org/TR/html-aria/#el-span
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

private:
    explicit HTMLSpanElement(DOM::VtableProbe);
    virtual ~HTMLSpanElement() override = default;
};

}
