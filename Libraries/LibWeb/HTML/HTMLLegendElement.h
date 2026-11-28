/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLLegendElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLLegendElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    HTMLFormElement* form();

    virtual CSS::ElementBoxKind box_kind() const override;

private:
    explicit HTMLLegendElement(DOM::VtableProbe);
    virtual ~HTMLLegendElement() override = default;

    virtual void inserted() override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<DOM::Node> old_ancestor) override;
};

}
