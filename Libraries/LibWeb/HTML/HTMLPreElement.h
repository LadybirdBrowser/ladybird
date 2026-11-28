/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

class HTMLPreElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLPreElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // https://www.w3.org/TR/html-aria/#el-pre
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

private:
    explicit HTMLPreElement(DOM::VtableProbe);
    virtual ~HTMLPreElement() override = default;
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
