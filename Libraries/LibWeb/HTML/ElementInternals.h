/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/ValidityState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#elementinternals
class ElementInternals final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ElementInternals, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ElementInternals);

public:
    static GC::Ref<ElementInternals> create(JS::Realm&, HTMLElement& target_element);

    GC::Ptr<DOM::ShadowRoot> shadow_root() const;

    using ElementInternalsFormValue = Variant<GC::Root<FileAPI::File>, String, GC::Root<XHR::FormData>, Empty>;
    WebIDL::ExceptionOr<void> set_form_value(ElementInternalsFormValue value, Optional<ElementInternalsFormValue> state);

    WebIDL::ExceptionOr<GC::Ptr<HTMLFormElement>> form() const;

    WebIDL::ExceptionOr<void> set_validity(ValidityStateFlags const& flags, Optional<String> message, GC::Ptr<HTMLElement> anchor);
    WebIDL::ExceptionOr<bool> will_validate() const;
    WebIDL::ExceptionOr<GC::Ref<ValidityState const>> validity() const;
    WebIDL::ExceptionOr<String> validation_message() const;
    WebIDL::ExceptionOr<bool> check_validity() const;
    WebIDL::ExceptionOr<bool> report_validity() const;

    WebIDL::ExceptionOr<GC::Ptr<DOM::NodeList>> labels();
    GC::Ptr<CustomStateSet> states();

private:
    explicit ElementInternals(JS::Realm&, HTMLElement& target_element);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#internals-target
    GC::Ref<HTMLElement> m_target_element;
};

}
