/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLDListElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLDListElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

private:
    explicit HTMLDListElement(DOM::VtableProbe);
    virtual ~HTMLDListElement() override = default;
};

}
