/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/HTMLOrSVGElement.h>

namespace Web::MathML {

class MathMLElement : public DOM::Element
    , public HTML::GlobalEventHandlers
    , public HTML::HTMLOrSVGElement<MathMLElement> {
    WEB_PLATFORM_OBJECT(MathMLElement, DOM::Element);
    JS_DECLARE_ALLOCATOR(MathMLElement);

public:
    virtual ~MathMLElement() override;

    virtual Optional<ARIA::Role> default_role() const override;

protected:
    virtual void attribute_change_steps(FlyString const&, Optional<String> const&, Optional<String> const&, Optional<FlyString> const&) override;
    virtual WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) override;
    virtual void inserted() override;
    virtual JS::GCPtr<DOM::EventTarget> global_event_handlers_to_event_target(FlyString const&) override { return *this; }

private:
    MathMLElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Visitor&) override;

    virtual void initialize(JS::Realm&) override;
};

}
