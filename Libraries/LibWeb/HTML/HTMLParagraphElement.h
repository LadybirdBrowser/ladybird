/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLParagraphElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLParagraphElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;

    // https://www.w3.org/TR/html-aria/#el-p
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::paragraph; }

private:
    explicit HTMLParagraphElement(DOM::VtableProbe);
    virtual ~HTMLParagraphElement() override = default;
};

}
