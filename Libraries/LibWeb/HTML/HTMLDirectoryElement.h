/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

// NOTE: This element is marked as obsolete, but is still listed as required by the specification.
class HTMLDirectoryElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLDirectoryElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

private:
    explicit HTMLDirectoryElement(DOM::VtableProbe);
    virtual ~HTMLDirectoryElement() override = default;
};

}
