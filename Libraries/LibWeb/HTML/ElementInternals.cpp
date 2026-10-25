/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/WeakHashMap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/ElementInternals.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/ElementInternals.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ElementInternals);

GC::Ref<ElementInternals> ElementInternals::create(HTMLElement& target_element)
{
    return GC::Heap::the().allocate<ElementInternals>(target_element);
}

ElementInternals::ElementInternals(HTMLElement& target_element)
    : m_target_element(target_element)
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
WebIDL::ExceptionOr<void> ElementInternals::set_form_value(ElementInternalsFormValue value, Optional<ElementInternalsFormValue> state)
{
    // 1. Let element be this's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    // 3. Set target element's submission value to value if value is not a FormData object, or to a clone of value's entry list otherwise.
    auto submission_value = value.visit(
        [](GC::Ref<FileAPI::File> file) -> FormAssociatedElement::FACESubmissionValue {
            return file;
        },
        [](Utf16String const& string) -> FormAssociatedElement::FACESubmissionValue {
            return string;
        },
        [](GC::Ref<XHR::FormData> form_data) -> FormAssociatedElement::FACESubmissionValue {
            return form_data->entry_list();
        },
        [](Empty const& empty) -> FormAssociatedElement::FACESubmissionValue {
            return empty;
        });

    element->set_face_submission_value({}, submission_value);

    // 4. If the state argument of the function is omitted, set element's state to its submission value.
    if (!state.has_value()) {
        element->set_face_state({}, submission_value);
    }

    // 5. Otherwise, if state is a FormData object, set element's state to a clone of state's entry list.
    // 6. Otherwise, set element's state to state.
    else {
        auto state_value = state.value().visit(
            [](GC::Ref<FileAPI::File> file) -> FormAssociatedElement::FACESubmissionValue {
                return GC::Ref { *file };
            },
            [](Utf16String const& string) -> FormAssociatedElement::FACESubmissionValue {
                return string;
            },
            [](GC::Ref<XHR::FormData> form_data) -> FormAssociatedElement::FACESubmissionValue {
                return form_data->entry_list();
            },
            [](Empty const& empty) -> FormAssociatedElement::FACESubmissionValue {
                return empty;
            });

        element->set_face_state({}, state_value);
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-form
WebIDL::ExceptionOr<GC::Ptr<HTMLFormElement>> ElementInternals::form() const
{
    // Form-associated custom elements don't have form IDL attribute. Instead, their ElementInternals object has a form IDL attribute.
    // On getting, it must throw a "NotSupportedError" DOMException if the target element is not a form-associated custom element.
    // Otherwise, it must return the element's form owner, or null if there isn't one.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    return m_target_element->form();
}

static bool validity_flags_has_one_or_more_true_values(ValidityStateFlags const& flags)
{
    return flags.value_missing
        || flags.type_mismatch
        || flags.pattern_mismatch
        || flags.too_long
        || flags.too_short
        || flags.range_underflow
        || flags.range_overflow
        || flags.step_mismatch
        || flags.bad_input
        || flags.custom_error;
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-setvalidity
WebIDL::ExceptionOr<void> ElementInternals::set_validity(ValidityStateFlags const& flags, Optional<Utf16String> message, GC::Ptr<HTMLElement> anchor)
{
    // 1. Let element be this's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    // 3. If flags contains one or more true values and message is not given or is the empty string, then throw a TypeError.
    if (validity_flags_has_one_or_more_true_values(flags) && (!message.has_value() || message->is_empty())) {
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "Invalid flag(s) and empty message"_utf16
        };
    }

    // 4. For each entry flag → value of flags, set element's validity flag with the name flag to value.
    element->set_face_validity_flags({}, flags);

    // 5. Set element's validation message to the empty string if message is not given or all of element's validity flags are false, or to message otherwise.
    Utf16String validation_message;
    if (message.has_value() && validity_flags_has_one_or_more_true_values(flags))
        validation_message = message.release_value();

    element->set_face_validation_message({}, validation_message);

    // 6. If element's customError validity flag is true, then set element's custom validity error message to element's
    //    validation message. Otherwise, set element's custom validity error message to the empty string.
    element->set_custom_validity_error_message({}, flags.custom_error ? validation_message : Utf16String {});

    // 7. If anchor is not given, then set it to element.
    if (!anchor) {
        anchor = element;
    }

    // 8. Otherwise, if anchor is not a shadow-including inclusive descendant of element, then throw a "NotFoundError" DOMException.
    else if (!anchor->is_shadow_including_inclusive_descendant_of(element)) {
        return WebIDL::NotFoundError::create("Anchor is not a shadow-including descendant of element"_utf16);
    }

    // 9. Set element's validation anchor to anchor.
    element->set_face_validation_anchor({}, anchor);

    // AD-HOC: Updating the validity flags changes which validity pseudo-classes match.
    CSS::Invalidation::invalidate_style_after_validity_change(*element);
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-willvalidate
WebIDL::ExceptionOr<bool> ElementInternals::will_validate() const
{
    // The willValidate attribute of ElementInternals interface, on getting, must throw a "NotSupportedError" DOMException if
    // the target element is not a form-associated custom element. Otherwise, it must return true if the target element is a
    // candidate for constraint validation, and false otherwise.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    return m_target_element->is_candidate_for_constraint_validation();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-validity
WebIDL::ExceptionOr<GC::Ref<ValidityState const>> ElementInternals::validity() const
{
    // The validity attribute of ElementInternals interface, on getting, must throw a "NotSupportedError" DOMException if
    // the target element is not a form-associated custom element. Otherwise, it must return a ValidityState object that
    // represents the validity states of the target element. This object is live.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    return ValidityState::create(m_target_element);
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-validationmessage
WebIDL::ExceptionOr<Utf16String> ElementInternals::validation_message() const
{
    // 1. Let element be this's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    // 3. Return element's validation message.
    return element->face_validation_message();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-checkvalidity
WebIDL::ExceptionOr<bool> ElementInternals::check_validity() const
{
    // 1. Let element be this ElementInternals's target element.
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    // 3. Run the check validity steps on element.
    return element->check_validity_steps();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-elementinternals-reportvalidity
WebIDL::ExceptionOr<bool> ElementInternals::report_validity() const
{
    // 1. Let element be this ElementInternals's target element
    auto element = m_target_element;

    // 2. If element is not a form-associated custom element, then throw a "NotSupportedError" DOMException.
    if (!element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    // 3. Run the report validity steps on element.
    return element->report_validity_steps();
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-elementinternals-labels
WebIDL::ExceptionOr<GC::Ptr<DOM::NodeList>> ElementInternals::labels()
{
    // Form-associated custom elements don't have a labels IDL attribute. Instead, their ElementInternals object has a labels IDL attribute.
    // On getting, it must throw a "NotSupportedError" DOMException if the target element is not a form-associated custom element.
    // Otherwise, it must return that NodeList object, and that same value must always be returned.
    if (!m_target_element->is_form_associated_custom_element())
        return WebIDL::NotSupportedError::create("Element is not a form-associated custom element"_utf16);

    return m_target_element->labels();
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-states
GC::Ptr<CustomStateSet> ElementInternals::states()
{
    // The states getter steps are to return this's target element's states set.
    return m_target_element->ensure_custom_state_set();
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#get-the-content-attribute
Optional<Utf16String> ElementInternals::get_attribute(Utf16FlyString const& content_attribute) const
{
    // 1. If elementInternals's target element's internal content attribute map[the reflected content attribute name]
    //    does not exist, then return null.
    // 2. Return elementInternals's target element's internal content attribute map[the reflected content attribute
    //    name].
    return m_target_element->internal_content_attribute(content_attribute);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#set-the-content-attribute
void ElementInternals::set_attribute_value(Utf16FlyString const& content_attribute, Utf16String const& value)
{
    // Set elementInternals's target element's internal content attribute map[the reflected content attribute name]
    // to value.
    m_target_element->internal_content_attribute_map().set(content_attribute, value);
    did_change_default_aria_semantics();
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#delete-the-content-attribute
void ElementInternals::remove_attribute(Utf16FlyString const& content_attribute)
{
    // Remove elementInternals's target element's internal content attribute map[the reflected content attribute
    // name].
    m_target_element->internal_content_attribute_map().remove(content_attribute);
    did_change_default_aria_semantics();
}

// AD-HOC: A change to the internal content attribute map changes the default ARIA semantics of the target element, so
//         ask the client to rebuild the accessibility tree, as a DOM mutation does in Node::queue_mutation_record().
void ElementInternals::did_change_default_aria_semantics()
{
    auto& page = m_target_element->document().page();
    if (page.accessibility_interested())
        page.client().page_did_change_accessibility_tree();
}

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute)                       \
    Optional<Utf16String> ElementInternals::name() const                  \
    {                                                                     \
        return get_attribute(ARIA::AttributeNames::name);                 \
    }                                                                     \
                                                                          \
    void ElementInternals::set_##name(Optional<Utf16String> const& value) \
    {                                                                     \
        if (value.has_value())                                            \
            set_attribute_value(ARIA::AttributeNames::name, *value);      \
        else                                                              \
            remove_attribute(ARIA::AttributeNames::name);                 \
    }
ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)    \
    GC::Ptr<DOM::Element> ElementInternals::attribute() const           \
    {                                                                   \
        return m_##attribute.ptr();                                     \
    }                                                                   \
                                                                        \
    void ElementInternals::set_##attribute(GC::Ptr<DOM::Element> value) \
    {                                                                   \
        m_##attribute = value;                                          \
    }
ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)                       \
    Optional<Vector<GC::Weak<DOM::Element>> const&> ElementInternals::attribute() const    \
    {                                                                                      \
        if (!m_##attribute.has_value())                                                    \
            return {};                                                                     \
        return *m_##attribute;                                                             \
    }                                                                                      \
                                                                                           \
    void ElementInternals::set_##attribute(Optional<Vector<GC::Weak<DOM::Element>>> value) \
    {                                                                                      \
        m_##attribute = move(value);                                                       \
    }
ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-element
GC::Ptr<DOM::Element> ElementInternals::get_the_attribute_associated_element(Utf16FlyString const& content_attribute, GC::Ptr<DOM::Element> explicitly_set_attribute_element) const
{
    // 1. Let element be the result of running reflectedTarget's get the element.
    auto const& element = get_the_element();

    // 2. Let contentAttributeValue be the result of running reflectedTarget's get the content attribute.
    auto content_attribute_value = get_attribute(content_attribute);

    return DOM::get_the_attribute_associated_element(element, content_attribute_value, explicitly_set_attribute_element);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-elements
Optional<GC::RootVector<GC::Ref<DOM::Element>>> ElementInternals::get_the_attribute_associated_elements(Utf16FlyString const& content_attribute, Optional<Vector<GC::Weak<DOM::Element>> const&> explicitly_set_attribute_elements) const
{
    // 2. Let element be the result of running reflectedTarget's get the element.
    auto const& element = get_the_element();

    return DOM::get_the_attribute_associated_elements(
        element, [&] { return get_attribute(content_attribute); }, explicitly_set_attribute_elements);
}

void ElementInternals::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target_element);
}

}

namespace Web::Bindings {

struct PerElementInternalsReflectedArrayCache {
    HashMap<Utf16FlyString, WrapperWorldWeakValueCache<JS::Array>> arrays_by_attribute;
};

static GC::WeakHashMap<HTML::ElementInternals, PerElementInternalsReflectedArrayCache>& reflected_element_array_caches()
{
    static NeverDestroyed<GC::WeakHashMap<HTML::ElementInternals, PerElementInternalsReflectedArrayCache>> caches;
    return *caches;
}

static WrapperWorldWeakValueCache<JS::Array>& reflected_element_array_cache_for(HTML::ElementInternals& element_internals, Utf16FlyString const& reflected_attribute)
{
    auto& per_element_internals = reflected_element_array_caches().ensure(element_internals);
    return per_element_internals.arrays_by_attribute.ensure(reflected_attribute);
}

GC::Ptr<JS::Array> cached_reflected_element_array(HTML::ElementInternals& element_internals, WrapperWorld const& wrapper_world, Utf16FlyString const& reflected_attribute)
{
    return reflected_element_array_cache_for(element_internals, reflected_attribute).get(wrapper_world);
}

void set_cached_reflected_element_array(HTML::ElementInternals& element_internals, WrapperWorld const& wrapper_world, Utf16FlyString const& reflected_attribute, GC::Ptr<JS::Array> array)
{
    reflected_element_array_cache_for(element_internals, reflected_attribute).set(wrapper_world, array);
}

}
