/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLDataElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLDataElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // https://www.w3.org/TR/html-aria/#el-data
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

private:
    explicit HTMLDataElement(DOM::VtableProbe);
    virtual ~HTMLDataElement() override = default;
};

}
