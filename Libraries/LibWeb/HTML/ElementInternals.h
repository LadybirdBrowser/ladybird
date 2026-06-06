/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/ElementInternals.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/ValidityState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#elementinternals
class ElementInternals final : public Bindings::Wrappable {
    WEB_WRAPPABLE(ElementInternals, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ElementInternals);

public:
    static GC::Ref<ElementInternals> create(HTMLElement& target_element);

    GC::Ptr<DOM::ShadowRoot> shadow_root() const;

    using ElementInternalsFormValue = Variant<GC::Ref<FileAPI::File>, String, GC::Ref<XHR::FormData>, Empty>;
    WebIDL::ExceptionOr<void> set_form_value(JS::Realm&, ElementInternalsFormValue value, Optional<ElementInternalsFormValue> state);

    WebIDL::ExceptionOr<GC::Ptr<HTMLFormElement>> form(JS::Realm&) const;

    WebIDL::ExceptionOr<void> set_validity(JS::Realm&, Bindings::ValidityStateFlags const& flags, Optional<String> message, GC::Ptr<HTMLElement> anchor);
    WebIDL::ExceptionOr<bool> will_validate(JS::Realm&) const;
    WebIDL::ExceptionOr<GC::Ref<ValidityState const>> validity(JS::Realm&) const;
    WebIDL::ExceptionOr<String> validation_message(JS::Realm&) const;
    WebIDL::ExceptionOr<bool> check_validity(JS::Realm&) const;
    WebIDL::ExceptionOr<bool> report_validity(JS::Realm&) const;

    WebIDL::ExceptionOr<GC::Ptr<DOM::NodeList>> labels(JS::Realm&);
    GC::Ptr<CustomStateSet> states();

private:
    explicit ElementInternals(HTMLElement& target_element);

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#internals-target
    GC::Ref<HTMLElement> m_target_element;
};

}
