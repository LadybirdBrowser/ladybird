/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLParamElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLParamElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

private:
    explicit HTMLParamElement(DOM::VtableProbe);
    virtual ~HTMLParamElement() override = default;
};

}
