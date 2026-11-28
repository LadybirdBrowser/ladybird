/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLTimeElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLTimeElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // https://www.w3.org/TR/html-aria/#el-time
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::time; }

private:
    explicit HTMLTimeElement(DOM::VtableProbe);
    virtual ~HTMLTimeElement() override = default;
};

}
