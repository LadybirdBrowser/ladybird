/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLUListElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLUListElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // https://www.w3.org/TR/html-aria/#el-ul
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::list; }

private:
    explicit HTMLUListElement(DOM::VtableProbe);
    virtual ~HTMLUListElement() override = default;

    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
