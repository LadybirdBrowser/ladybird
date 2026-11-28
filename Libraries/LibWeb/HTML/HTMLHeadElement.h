/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLHeadElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLHeadElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

private:
    explicit HTMLHeadElement(DOM::VtableProbe);
    virtual ~HTMLHeadElement() override = default;
};

}
