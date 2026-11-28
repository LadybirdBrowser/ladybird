/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLQuoteElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLQuoteElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    virtual Optional<ARIA::Role> default_role() const override;

private:
    explicit HTMLQuoteElement(DOM::VtableProbe);
    virtual ~HTMLQuoteElement() override = default;
};

}
