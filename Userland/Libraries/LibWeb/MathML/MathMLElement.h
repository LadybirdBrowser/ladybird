/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/DOMStringMap.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>

namespace Web::MathML {

class MathMLElement : public DOM::Element
    , public HTML::GlobalEventHandlers {
    WEB_PLATFORM_OBJECT(MathMLElement, DOM::Element);
    GC_DECLARE_ALLOCATOR(MathMLElement);

public:
    virtual ~MathMLElement() override;

    [[nodiscard]] GC::Ref<HTML::DOMStringMap> dataset();

    virtual Optional<ARIA::Role> default_role() const override;

    void focus();
    void blur();

protected:
    virtual GC::Ptr<DOM::EventTarget> global_event_handlers_to_event_target(FlyString const&) override { return *this; }

private:
    MathMLElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Visitor&) override;

    virtual void initialize(JS::Realm&) override;

    GC::Ptr<HTML::DOMStringMap> m_dataset;
};

}
