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

struct ValidityStateFlags {
    bool value_missing = false;
    bool type_mismatch = false;
    bool pattern_mismatch = false;
    bool too_long = false;
    bool too_short = false;
    bool range_underflow = false;
    bool range_overflow = false;
    bool step_mismatch = false;
    bool bad_input = false;
    bool custom_error = false;

    bool has_one_or_more_true_values() const
    {
        return value_missing || type_mismatch || pattern_mismatch || too_long || too_short || range_underflow || range_overflow || step_mismatch || bad_input || custom_error;
    }
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#elementinternals
class ElementInternals final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ElementInternals, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ElementInternals);

public:
    static GC::Ref<ElementInternals> create(JS::Realm&, HTMLElement& target_element);

    GC::Ptr<DOM::ShadowRoot> shadow_root() const;

    WebIDL::ExceptionOr<void> set_form_value(Variant<GC::Root<FileAPI::File>, String, GC::Root<XHR::FormData>> value, Optional<Variant<GC::Root<FileAPI::File>, String, GC::Root<XHR::FormData>>> state);

    WebIDL::ExceptionOr<GC::Ptr<HTMLFormElement>> form() const;

    WebIDL::ExceptionOr<void> set_validity(ValidityStateFlags const& flags, Optional<String> message, Optional<GC::Ptr<HTMLElement>> anchor);
    WebIDL::ExceptionOr<bool> will_validate() const;
    WebIDL::ExceptionOr<GC::Ref<ValidityState const>> validity() const;
    WebIDL::ExceptionOr<String> validation_message() const;
    WebIDL::ExceptionOr<bool> check_validity() const;
    WebIDL::ExceptionOr<bool> report_validity() const;

    WebIDL::ExceptionOr<GC::Ptr<DOM::NodeList>> labels();

private:
    explicit ElementInternals(JS::Realm&, HTMLElement& target_element);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#internals-target
    GC::Ref<HTMLElement> m_target_element;
};

}
