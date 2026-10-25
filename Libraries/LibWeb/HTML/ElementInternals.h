/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/ARIA/AttributeNames.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/ValidityState.h>

namespace Web::Bindings {

WEB_API GC::Ptr<JS::Array> cached_reflected_element_array(HTML::ElementInternals&, WrapperWorld const&, Utf16FlyString const&);
WEB_API void set_cached_reflected_element_array(HTML::ElementInternals&, WrapperWorld const&, Utf16FlyString const&, GC::Ptr<JS::Array>);

}

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/custom-elements.html#elementinternals
class ElementInternals final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(ElementInternals, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(ElementInternals);

public:
    static GC::Ref<ElementInternals> create(HTMLElement& target_element);

    GC::Ptr<DOM::ShadowRoot> shadow_root() const;

    using ElementInternalsFormValue = Variant<GC::Ref<FileAPI::File>, Utf16String, GC::Ref<XHR::FormData>, Empty>;
    WebIDL::ExceptionOr<void> set_form_value(ElementInternalsFormValue value, Optional<ElementInternalsFormValue> state);

    WebIDL::ExceptionOr<GC::Ptr<HTMLFormElement>> form() const;

    WebIDL::ExceptionOr<void> set_validity(ValidityStateFlags const& flags, Optional<Utf16String> message, GC::Ptr<HTMLElement> anchor);
    WebIDL::ExceptionOr<bool> will_validate() const;
    WebIDL::ExceptionOr<GC::Ref<ValidityState const>> validity() const;
    WebIDL::ExceptionOr<Utf16String> validation_message() const;
    WebIDL::ExceptionOr<bool> check_validity() const;
    WebIDL::ExceptionOr<bool> report_validity() const;

    WebIDL::ExceptionOr<GC::Ptr<DOM::NodeList>> labels();
    GC::Ptr<CustomStateSet> states();

    // https://html.spec.whatwg.org/multipage/custom-elements.html#accessibility-semantics
#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute) \
    Optional<Utf16String> name() const;             \
    void set_##name(Optional<Utf16String> const&);
    ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#explicitly-set-attr-element
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    GC::Ptr<DOM::Element> attribute() const;                         \
    void set_##attribute(GC::Ptr<DOM::Element> value);
    ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#explicitly-set-attr-elements
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)   \
    Optional<Vector<GC::Weak<DOM::Element>> const&> attribute() const; \
    void set_##attribute(Optional<Vector<GC::Weak<DOM::Element>>> value);
    ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflected-target
    // NB: The reflection algorithms for an ElementInternals reflected target, as used by the generated bindings.
    HTMLElement& get_the_element() const { return m_target_element; }
    Optional<Utf16String> get_attribute(Utf16FlyString const& content_attribute) const;
    void set_attribute_value(Utf16FlyString const& content_attribute, Utf16String const& value);
    void remove_attribute(Utf16FlyString const& content_attribute);

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-element
    GC::Ptr<DOM::Element> get_the_attribute_associated_element(Utf16FlyString const& content_attribute, GC::Ptr<DOM::Element> explicitly_set_attribute_element) const;

    // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#attr-associated-elements
    Optional<GC::RootVector<GC::Ref<DOM::Element>>> get_the_attribute_associated_elements(Utf16FlyString const& content_attribute, Optional<Vector<GC::Weak<DOM::Element>> const&> explicitly_set_attribute_elements) const;

private:
    explicit ElementInternals(HTMLElement& target_element);

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

    void did_change_default_aria_semantics();

    // https://html.spec.whatwg.org/multipage/custom-elements.html#internals-target
    GC::Ref<HTMLElement> m_target_element;

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    GC::Weak<DOM::Element> m_##attribute;
    ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    Optional<Vector<GC::Weak<DOM::Element>>> m_##attribute;
    ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE
};

}
