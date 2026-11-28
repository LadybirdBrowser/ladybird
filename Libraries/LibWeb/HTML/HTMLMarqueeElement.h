/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

// NOTE: This element is marked as obsolete, but is still listed as required by the specification.
class HTMLMarqueeElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLMarqueeElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    WebIDL::UnsignedLong scroll_amount();
    void set_scroll_amount(WebIDL::UnsignedLong);

    WebIDL::UnsignedLong scroll_delay();
    void set_scroll_delay(WebIDL::UnsignedLong);

private:
    explicit HTMLMarqueeElement(DOM::VtableProbe);
    virtual ~HTMLMarqueeElement() override = default;
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;
};

}
