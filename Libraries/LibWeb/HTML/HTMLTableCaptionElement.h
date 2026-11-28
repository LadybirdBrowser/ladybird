/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLTableCaptionElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLTableCaptionElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;

    // https://www.w3.org/TR/html-aria/#el-caption
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::caption; }

private:
    explicit HTMLTableCaptionElement(DOM::VtableProbe);
    virtual ~HTMLTableCaptionElement() override = default;
};

}
