/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

class HTMLTableColElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLTableColElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    WebIDL::UnsignedLong span() const;
    void set_span(WebIDL::UnsignedLong);

private:
    explicit HTMLTableColElement(DOM::VtableProbe);
    virtual ~HTMLTableColElement() override = default;

    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
