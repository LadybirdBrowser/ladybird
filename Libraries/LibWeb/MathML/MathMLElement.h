/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/HTMLOrSVGOrMathMLElement.h>

namespace Web::MathML {

class MathMLElement : public DOM::Element
    , public HTML::GlobalEventHandlers
    , public HTML::HTMLOrSVGOrMathMLElement<MathMLElement> {
    WEB_PLATFORM_OBJECT(MathMLElement, DOM::Element);
    GC_DECLARE_ALLOCATOR(MathMLElement);

public:
    virtual ~MathMLElement() override;

    virtual Optional<ARIA::Role> default_role() const override;

protected:
    MathMLElement(DOM::Document&, DOM::QualifiedName);
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) const override;
    virtual void inserted() override;
    virtual GC::Ptr<DOM::EventTarget> global_event_handlers_to_event_target(Utf16FlyString const&) override { return *this; }
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;

    virtual void visit_edges(Visitor&) override;

    virtual void initialize(JS::Realm&) override;
};

}
