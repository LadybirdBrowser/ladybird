/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLHRElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLHRElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // https://www.w3.org/TR/html-aria/#el-hr
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::separator; }

private:
    explicit HTMLHRElement(DOM::VtableProbe);
    virtual ~HTMLHRElement() override = default;

    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
