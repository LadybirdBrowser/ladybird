/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ElementInternalsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/ElementInternals.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ElementInternals);

GC::Ref<ElementInternals> ElementInternals::create(JS::Realm& realm, HTMLElement& target_element)
{
    return realm.create<ElementInternals>(realm, target_element);
}

ElementInternals::ElementInternals(JS::Realm& realm, HTMLElement& target_element)
    : Bindings::PlatformObject(realm)
    , m_target_element(target_element)
{
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-shadowroot
GC::Ptr<DOM::ShadowRoot> ElementInternals::shadow_root() const
{
    // 1. Let target be this's target element.
    auto target = m_target_element;

    // 2. If target is not a shadow host, then return null.
    if (!target->is_shadow_host())
        return nullptr;

    // 3. Let shadow be target's shadow root.
    auto shadow = target->shadow_root();

    // 4. If shadow's available to element internals is false, then return null.
    if (!shadow->available_to_element_internals())
        return nullptr;

    // 5. Return shadow.
    return shadow;
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-setformvalue
WebIDL::ExceptionOr<void> ElementInternals::set_form_value(Variant<GC::Root<FileAPI::File>, String, GC::Root<XHR::FormData>> value, Optional<Variant<GC::Root<FileAPI::File>, String, GC::Root<XHR::FormData>>> state)
{
    // 1. Let element be this's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    (void)value;
    (void)state;

    // FIXME: 3. Set target element's submission value to value if value is not a FormData object, or to a clone of value's entry list otherwise.

    // FIXME: 4. If the state argument of the function is omitted, set element's state to its submission value.

    // FIXME: 5. Otherwise, if state is a FormData object, set element's state to a clone of state's entry list.

    // FIXME: 6. Otherwise, set element's state to state.

    dbgln("FIXME: ElementInternals::set_form_value()");
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-form
WebIDL::ExceptionOr<GC::Ptr<HTMLFormElement>> ElementInternals::form() const
{
    // Form-associated custom elements don't have form IDL attribute. Instead, their ElementInternals object has a form IDL attribute.
    // On getting, it must throw a "NotSupportedError" DOMException if the target element is not a form-associated custom element.
    // Otherwise, it must return the element's form owner, or null if there isn't one.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    dbgln("FIXME: ElementInternals::form()");
    return WebIDL::NotFoundError::create(realm(), "FIXME: ElementInternals::form()"_string);
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-setvalidity
WebIDL::ExceptionOr<void> ElementInternals::set_validity(ValidityStateFlags const& flags, Optional<String> message, Optional<GC::Ptr<HTMLElement>> anchor)
{
    // 1. Let element be this's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    // 3. If flags contains one or more true values and message is not given or is the empty string, then throw a TypeError.
    if (flags.has_one_or_more_true_values() && (!message.has_value() || message->is_empty())) {
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "Invalid flag(s) and empty message"sv
        };
    }

    // FIXME: 4. For each entry flag → value of flags, set element's validity flag with the name flag to value.

    // FIXME: 5. Set element's validation message to the empty string if message is not given or all of element's validity flags are false, or to message otherwise.

    // FIXME: 6. If element's customError validity flag is true, then set element's custom validity error message to element's validation message. Otherwise, set element's custom validity error message to the empty string.

    // FIXME: 7. Set element's validation anchor to null if anchor is not given. Otherwise, if anchor is not a shadow-including descendant of element, then throw a "NotFoundError" DOMException. Otherwise, set element's validation anchor to anchor.
    if (!anchor.has_value() || !anchor.value().ptr()) {
        // FIXME
    } else if (!anchor.value()->is_shadow_including_descendant_of(element)) {
        return WebIDL::NotFoundError::create(realm(), "Anchor is not a shadow-including descendant of element"_string);
    } else {
        // FIXME
    }

    dbgln("FIXME: ElementInternals::set_validity()");
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-willvalidate
WebIDL::ExceptionOr<bool> ElementInternals::will_validate() const
{
    // The willValidate attribute of ElementInternals interface, on getting, must throw a "NotSupportedError" DOMException if
    // the target element is not a form-associated custom element. Otherwise, it must return true if the target element is a
    // candidate for constraint validation, and false otherwise.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    dbgln("FIXME: ElementInternals::will_validate()");
    return true;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-validity
WebIDL::ExceptionOr<GC::Ref<ValidityState const>> ElementInternals::validity() const
{
    // The validity attribute of ElementInternals interface, on getting, must throw a "NotSupportedError" DOMException if
    // the target element is not a form-associated custom element. Otherwise, it must return a ValidityState object that
    // represents the validity states of the target element. This object is live.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    dbgln("FIXME: ElementInternals::validity()");
    return WebIDL::NotSupportedError::create(realm(), "FIXME: ElementInternals::validity()"_string);
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-validationmessage
WebIDL::ExceptionOr<String> ElementInternals::validation_message() const
{
    // 1. Let element be this's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    // FIXME: 3. Return element's validation message.

    dbgln("FIXME: ElementInternals::validation_message()");
    return "FIXME: ElementInternals::validation_message()"_string;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-checkvalidity
WebIDL::ExceptionOr<bool> ElementInternals::check_validity() const
{
    // 1. Let element be this ElementInternals's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    // FIXME: 3. Run the check validity steps on element.

    dbgln("FIXME: ElementInternals::check_validity()");
    return true;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-reportvalidity
WebIDL::ExceptionOr<bool> ElementInternals::report_validity() const
{
    // 1. Let element be this ElementInternals's target element
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    // FIXME: 3. Run the report validity steps on element.

    dbgln("FIXME: ElementInternals::report_validity()");
    return true;
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-elementinternals-labels
WebIDL::ExceptionOr<GC::Ptr<DOM::NodeList>> ElementInternals::labels()
{
    // Form-associated custom elements don't have a labels IDL attribute. Instead, their ElementInternals object has a labels IDL attribute.
    // On getting, it must throw a "NotSupportedError" DOMException if the target element is not a form-associated custom element.
    // Otherwise, it must return that NodeList object, and that same value must always be returned.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create(realm(), "Element is not a form-associated custom element"_string);

    dbgln("FIXME: ElementInternals::labels()");
    return WebIDL::NotSupportedError::create(realm(), "FIXME: ElementInternals::labels()"_string);
}

void ElementInternals::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ElementInternals);
    Base::initialize(realm);
}

void ElementInternals::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target_element);
}

}
