/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLDivElement : public HTMLElement {
    WEB_WRAPPABLE(HTMLDivElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // https://www.w3.org/TR/html-aria/#el-div
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

protected:
    explicit HTMLDivElement(DOM::VtableProbe);
    virtual ~HTMLDivElement() override = default;

private:
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
