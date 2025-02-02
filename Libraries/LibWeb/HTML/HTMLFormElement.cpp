/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLFormElementPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/FormControlInfrastructure.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormControlsCollection.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLOutputElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/RadioNodeList.h>
#include <LibWeb/HTML/SubmitEvent.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFormElement);

HTMLFormElement::HTMLFormElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
        .supports_named_properties = true,
        .has_legacy_unenumerable_named_properties_interface_extended_attribute = true,
        .has_legacy_override_built_ins_interface_extended_attribute = true,
    };
}

HTMLFormElement::~HTMLFormElement() = default;

void HTMLFormElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFormElement);
}

void HTMLFormElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_elements);
    visitor.visit(m_associated_elements);
    visitor.visit(m_planned_navigation);
    visitor.visit(m_rel_list);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#implicit-submission
WebIDL::ExceptionOr<void> HTMLFormElement::implicitly_submit_form()
{
    // If the user agent supports letting the user submit a form implicitly (for example, on some platforms hitting the
    // "enter" key while a text control is focused implicitly submits the form), then doing so for a form, whose default
    // button has activation behavior and is not disabled, must cause the user agent to fire a click event at that
    // default button.
    if (auto* default_button = this->default_button()) {
        auto& default_button_element = default_button->form_associated_element_to_html_element();

        if (default_button_element.has_activation_behavior() && default_button->enabled())
            default_button_element.click();

        return {};
    }

    // If the form has no submit button, then the implicit submission mechanism must perform the following steps:

    // 1. If the form has more than one field that blocks implicit submission, then return.
    if (number_of_fields_blocking_implicit_submission() > 1)
        return {};

    // 2. Submit the form element from the form element itself with userInvolvement set to "activation".
    TRY(submit_form(*this, { .user_involvement = UserNavigationInvolvement::Activation }));

    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-form-submit
WebIDL::ExceptionOr<void> HTMLFormElement::submit_form(GC::Ref<HTMLElement> submitter, SubmitFormOptions options)
{
    auto& vm = this->vm();
    auto& realm = this->realm();

    // 1. If form cannot navigate, then return.
    if (cannot_navigate())
        return {};

    // 2. If form's constructing entry list is true, then return.
    if (m_constructing_entry_list)
        return {};

    // 3. Let form document be form's node document.
    GC::Ref<DOM::Document> form_document = this->document();

    // 4. If form document's active sandboxing flag set has its sandboxed forms browsing context flag set, then return.
    if (has_flag(form_document->active_sandboxing_flag_set(), HTML::SandboxingFlagSet::SandboxedForms))
        return {};

    // 5. If the submitted from submit() method flag is not set, then:
    if (!options.from_submit_binding) {
        // 1. If form's firing submission events is true, then return.
        if (m_firing_submission_events)
            return {};

        // 2. Set form's firing submission events to true.
        m_firing_submission_events = true;

        // 3. For each element field in the list of submittable elements whose form owner is form, set field's user validity to true.
        for (auto& element : get_submittable_elements()) {
            // NOTE: Only input, select and textarea elements have a user validity flag.
            //       See https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#user-validity
            if (is<HTMLInputElement>(*element)) {
                (&as<HTMLInputElement>(*element))->set_user_validity(false);
            } else if (is<HTMLSelectElement>(*element)) {
                (&as<HTMLSelectElement>(*element))->set_user_validity(false);
            } else if (is<HTMLTextAreaElement>(*element)) {
                (&as<HTMLTextAreaElement>(*element))->set_user_validity(false);
            }
        }

        // FIXME: 4. If the submitter element's no-validate state is false, then interactively validate the constraints
        //           of form and examine the result. If the result is negative (i.e., the constraint validation concluded
        //           that there were invalid fields and probably informed the user of this), then:
        //           1. Set form's firing submission events to false.
        //           2. Return.

        // 5. Let submitterButton be null if submitter is form. Otherwise, let submitterButton be submitter.
        GC::Ptr<HTMLElement> submitter_button;
        if (submitter != this)
            submitter_button = submitter;

        // 6. Let shouldContinue be the result of firing an event named submit at form using SubmitEvent, with the
        //    submitter attribute initialized to submitterButton, the bubbles attribute initialized to true, and the
        //    cancelable attribute initialized to true.
        SubmitEventInit event_init {};
        event_init.submitter = submitter_button;
        auto submit_event = SubmitEvent::create(realm, EventNames::submit, event_init);
        submit_event->set_bubbles(true);
        submit_event->set_cancelable(true);
        bool should_continue = dispatch_event(*submit_event);

        // 7. Set form's firing submission events to false.
        m_firing_submission_events = false;

        // 8. If shouldContinue is false, then return.
        if (!should_continue)
            return {};

        // 9. If form cannot navigate, then return.
        // Spec Note: Cannot navigate is run again as dispatching the submit event could have changed the outcome.
        if (cannot_navigate())
            return {};
    }

    // 6. Let encoding be the result of picking an encoding for the form.
    auto encoding = TRY_OR_THROW_OOM(vm, pick_an_encoding());

    // 7. Let entry list be the result of constructing the entry list with form, submitter, and encoding.
    auto entry_list_or_null = TRY(construct_entry_list(realm, *this, submitter, encoding));

    // 8. Assert: entry list is not null.
    VERIFY(entry_list_or_null.has_value());
    auto entry_list = entry_list_or_null.release_value();

    // 9. If form cannot navigate, then return.
    // Spec Note: Cannot navigate is run again as dispatching the formdata event in constructing the entry list could
    //            have changed the outcome.
    if (cannot_navigate())
        return {};

    // 10. Let method be the submitter element's method.
    auto method = method_state_from_form_element(submitter);

    // 11. If method is dialog, then:
    if (method == MethodAttributeState::Dialog) {
        // 1. If form does not have an ancestor dialog element, then return.
        // 2. Let subject be form's nearest ancestor dialog element.
        auto* subject = first_ancestor_of_type<HTMLDialogElement>();
        if (!subject)
            return {};

        // 3. Let result be null.
        Optional<String> result;

        // 4. If submitter is an input element whose type attribute is in the Image Button state, then:
        if (is<HTMLInputElement>(*submitter)) {
            auto const& input_element = static_cast<HTMLInputElement const&>(*submitter);

            if (input_element.type_state() == HTMLInputElement::TypeAttributeState::ImageButton) {
                // 1. Let (x, y) be the selected coordinate.
                auto [x, y] = input_element.selected_coordinate();

                // 2. Set result to the concatenation of x, ",", and y.
                result = MUST(String::formatted("{},{}", x, y));
            }
        }

        // 5. Otherwise, if submitter has a value, then set result to that value.
        if (!result.has_value())
            result = submitter->get_attribute_value(AttributeNames::value);

        // 6. Close the dialog subject with result.
        subject->close(move(result));

        // 7. Return.
        return {};
    }

    // 12. Let action be the submitter element's action.
    auto action = action_from_form_element(submitter);

    // 13. If action is the empty string, let action be the URL of the form document.
    if (action.is_empty())
        action = form_document->url_string();

    // 14. Let parsed action be the result of encoding-parsing a URL given action, relative to submitter's node document.
    auto parsed_action = submitter->document().encoding_parse_url(action);

    // 15. If parsed action is failure, then return.
    if (!parsed_action.has_value()) {
        dbgln("Failed to submit form: Invalid URL: {}", action);
        return {};
    }

    // 16. Let scheme be the scheme of parsed action.
    auto const& scheme = parsed_action->scheme();

    // 17. Let enctype be the submitter element's enctype.
    auto encoding_type = encoding_type_state_from_form_element(submitter);

    // 18. Let formTarget be null.
    Optional<String> form_target;

    // 19. If the submitter element is a submit button and it has a formtarget attribute, then set formTarget to the
    //     formtarget attribute value.
    if (auto* form_associated_element = as_if<FormAssociatedElement>(*submitter); form_associated_element && form_associated_element->is_submit_button()) {
        if (auto formtarget_attribute = submitter->attribute(AttributeNames::formtarget); formtarget_attribute.has_value()) {
            form_target = formtarget_attribute.release_value();
        }
    }

    // 20. Let target be the result of getting an element's target given submitter's form owner and formTarget.
    auto target = get_an_elements_target(form_target);

    // 21. Let noopener be the result of getting an element's noopener with form, parsed action, and target.
    auto no_opener = get_an_elements_noopener(*parsed_action, target);

    // 22. Let targetNavigable be the first return value of applying the rules for choosing a navigable given target,
    //     form's node navigable, and noopener.
    auto target_navigable = form_document->navigable()->choose_a_navigable(target, no_opener).navigable;

    // 23. If targetNavigable is null, then return.
    if (!target_navigable) {
        dbgln("Failed to submit form: choose_a_browsing_context returning a null browsing context");
        return {};
    }

    // 24. Let historyHandling be "auto".
    auto history_handling = Bindings::NavigationHistoryBehavior::Auto;

    // 25. If form document equals targetNavigable's active document, and form document has not yet completely loaded,
    //     then set historyHandling to "replace".
    if (form_document == target_navigable->active_document() && !form_document->is_completely_loaded())
        history_handling = Bindings::NavigationHistoryBehavior::Replace;

    // 25. Select the appropriate row in the table below based on scheme as given by the first cell of each row.
    //     Then, select the appropriate cell on that row based on method as given in the first cell of each column.
    //     Then, jump to the steps named in that cell and defined below the table.

    //            | GET               | POST
    // ------------------------------------------------------
    // http       | Mutate action URL | Submit as entity body
    // https      | Mutate action URL | Submit as entity body
    // ftp        | Get action URL    | Get action URL
    // javascript | Get action URL    | Get action URL
    // data       | Mutate action URL | Get action URL
    // mailto     | Mail with headers | Mail as body

    // If scheme is not one of those listed in this table, then the behavior is not defined by this specification.
    // User agents should, in the absence of another specification defining this, act in a manner analogous to that defined
    // in this specification for similar schemes.

    // AD-HOC: In accordance with the above paragraph, we implement file:// submission URLs the same as data: URLs.

    // This should have been handled above.
    VERIFY(method != MethodAttributeState::Dialog);

    if (scheme.is_one_of("http"sv, "https"sv)) {
        if (method == MethodAttributeState::GET)
            TRY_OR_THROW_OOM(vm, mutate_action_url(parsed_action.release_value(), move(entry_list), move(encoding), *target_navigable, history_handling, options.user_involvement));
        else
            TRY_OR_THROW_OOM(vm, submit_as_entity_body(parsed_action.release_value(), move(entry_list), encoding_type, move(encoding), *target_navigable, history_handling, options.user_involvement));
    } else if (scheme.is_one_of("ftp"sv, "javascript"sv)) {
        get_action_url(parsed_action.release_value(), *target_navigable, history_handling, options.user_involvement);
    } else if (scheme.is_one_of("data"sv, "file"sv)) {
        if (method == MethodAttributeState::GET)
            TRY_OR_THROW_OOM(vm, mutate_action_url(parsed_action.release_value(), move(entry_list), move(encoding), *target_navigable, history_handling, options.user_involvement));
        else
            get_action_url(parsed_action.release_value(), *target_navigable, history_handling, options.user_involvement);
    } else if (scheme == "mailto"sv) {
        if (method == MethodAttributeState::GET)
            TRY_OR_THROW_OOM(vm, mail_with_headers(parsed_action.release_value(), move(entry_list), move(encoding), *target_navigable, history_handling, options.user_involvement));
        else
            TRY_OR_THROW_OOM(vm, mail_as_body(parsed_action.release_value(), move(entry_list), encoding_type, move(encoding), *target_navigable, history_handling, options.user_involvement));
    } else {
        dbgln("Failed to submit form: Unknown scheme: {}", scheme);
        return {};
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#resetting-a-form
void HTMLFormElement::reset_form()
{
    // 1. Let reset be the result of firing an event named reset at form, with the bubbles and cancelable attributes initialized to true.
    auto reset_event = DOM::Event::create(realm(), HTML::EventNames::reset);
    reset_event->set_bubbles(true);
    reset_event->set_cancelable(true);

    bool reset = dispatch_event(reset_event);

    // 2. If reset is true, then invoke the reset algorithm of each resettable element whose form owner is form.
    if (reset) {
        GC::RootVector<GC::Ref<HTMLElement>> associated_elements_copy(heap(), m_associated_elements);
        for (auto element : associated_elements_copy) {
            VERIFY(is<FormAssociatedElement>(*element));
            auto& form_associated_element = dynamic_cast<FormAssociatedElement&>(*element);
            if (form_associated_element.is_resettable())
                form_associated_element.reset_algorithm();
        }
    }
}

WebIDL::ExceptionOr<void> HTMLFormElement::submit()
{
    return submit_form(*this, { .from_submit_binding = true });
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-requestsubmit
WebIDL::ExceptionOr<void> HTMLFormElement::request_submit(GC::Ptr<Element> submitter)
{
    // 1. If submitter is not null, then:
    if (submitter) {
        // 1. If submitter is not a submit button, then throw a TypeError.
        auto* form_associated_element = dynamic_cast<FormAssociatedElement*>(submitter.ptr());
        if (!(form_associated_element && form_associated_element->is_submit_button()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The submitter is not a submit button"sv };

        // 2. If submitter's form owner is not this form element, then throw a "NotFoundError" DOMException.
        if (form_associated_element->form() != this)
            return WebIDL::NotFoundError::create(realm(), "The submitter is not owned by this form element"_string);
    }
    // 2. Otherwise, set submitter to this form element.
    else {
        submitter = this;
    }

    // 3. Submit this form element, from submitter.
    return submit_form(static_cast<HTMLElement&>(*submitter), {});
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-reset
void HTMLFormElement::reset()
{
    // 1. If the form element is marked as locked for reset, then return.
    if (m_locked_for_reset)
        return;

    // 2. Mark the form element as locked for reset.
    m_locked_for_reset = true;

    // 3. Reset the form element.
    reset_form();

    // 4. Unmark the form element as locked for reset.
    m_locked_for_reset = false;
}

void HTMLFormElement::add_associated_element(Badge<FormAssociatedElement>, HTMLElement& element)
{
    m_associated_elements.append(element);
}

void HTMLFormElement::remove_associated_element(Badge<FormAssociatedElement>, HTMLElement& element)
{
    m_associated_elements.remove_first_matching([&](auto& entry) { return entry.ptr() == &element; });

    // If an element listed in a form element's past names map changes form owner, then its entries must be removed from that map.
    m_past_names_map.remove_all_matching([&](auto&, auto const& entry) { return entry.node == &element; });
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fs-action
String HTMLFormElement::action_from_form_element(GC::Ref<HTMLElement> element) const
{
    // The action of an element is the value of the element's formaction attribute, if the element is a submit button
    // and has such an attribute, or the value of its form owner's action attribute, if it has one, or else the empty
    // string.
    if (auto const* form_associated_element = dynamic_cast<FormAssociatedElement const*>(element.ptr());
        form_associated_element && form_associated_element->is_submit_button()) {
        if (auto maybe_attribute = element->attribute(AttributeNames::formaction); maybe_attribute.has_value())
            return maybe_attribute.release_value();
    }

    if (auto maybe_attribute = attribute(AttributeNames::action); maybe_attribute.has_value())
        return maybe_attribute.release_value();

    return String {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#form-submission-attributes:attr-fs-method-2
static HTMLFormElement::MethodAttributeState method_attribute_to_method_state(StringView method)
{
#define __ENUMERATE_FORM_METHOD_ATTRIBUTE(keyword, state)             \
    if (Infra::is_ascii_case_insensitive_match(#keyword##sv, method)) \
        return HTMLFormElement::MethodAttributeState::state;
    ENUMERATE_FORM_METHOD_ATTRIBUTES
#undef __ENUMERATE_FORM_METHOD_ATTRIBUTE

    // The method attribute's invalid value default and missing value default are both the GET state.
    return HTMLFormElement::MethodAttributeState::GET;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fs-method
HTMLFormElement::MethodAttributeState HTMLFormElement::method_state_from_form_element(GC::Ref<HTMLElement const> element) const
{
    // If the element is a submit button and has a formmethod attribute, then the element's method is that attribute's state;
    // otherwise, it is the form owner's method attribute's state.
    if (auto const* form_associated_element = dynamic_cast<FormAssociatedElement const*>(element.ptr());
        form_associated_element && form_associated_element->is_submit_button()) {

        if (auto maybe_formmethod = element->attribute(AttributeNames::formmethod); maybe_formmethod.has_value()) {
            // NOTE: `formmethod` is the same as `method`, except that it has no missing value default.
            //       This is handled by not calling `method_attribute_to_method_state` in the first place if there is no `formmethod` attribute.
            return method_attribute_to_method_state(maybe_formmethod.value());
        }
    }

    if (auto maybe_method = attribute(AttributeNames::method); maybe_method.has_value()) {
        return method_attribute_to_method_state(maybe_method.value());
    }

    return MethodAttributeState::GET;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#form-submission-attributes:attr-fs-enctype-2
static HTMLFormElement::EncodingTypeAttributeState encoding_type_attribute_to_encoding_type_state(StringView encoding_type)
{
#define __ENUMERATE_FORM_METHOD_ENCODING_TYPE(keyword, state)               \
    if (Infra::is_ascii_case_insensitive_match(keyword##sv, encoding_type)) \
        return HTMLFormElement::EncodingTypeAttributeState::state;
    ENUMERATE_FORM_METHOD_ENCODING_TYPES
#undef __ENUMERATE_FORM_METHOD_ENCODING_TYPE

    // The enctype attribute's invalid value default and missing value default are both the application/x-www-form-urlencoded state.
    return HTMLFormElement::EncodingTypeAttributeState::FormUrlEncoded;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fs-enctype
HTMLFormElement::EncodingTypeAttributeState HTMLFormElement::encoding_type_state_from_form_element(GC::Ref<HTMLElement> element) const
{
    // If the element is a submit button and has a formenctype attribute, then the element's enctype is that attribute's state;
    // otherwise, it is the form owner's enctype attribute's state.
    if (auto const* form_associated_element = dynamic_cast<FormAssociatedElement const*>(element.ptr());
        form_associated_element && form_associated_element->is_submit_button()) {
        if (auto formenctype = element->attribute(AttributeNames::formenctype); formenctype.has_value()) {
            // NOTE: `formenctype` is the same as `enctype`, except that it has nomissing value default.
            //       This is handled by not calling `encoding_type_attribute_to_encoding_type_state` in the first place if there is no
            //       `formenctype` attribute.
            return encoding_type_attribute_to_encoding_type_state(formenctype.value());
        }
    }

    if (auto maybe_enctype = attribute(AttributeNames::enctype); maybe_enctype.has_value())
        return encoding_type_attribute_to_encoding_type_state(maybe_enctype.value());

    return EncodingTypeAttributeState::FormUrlEncoded;
}

// https://html.spec.whatwg.org/multipage/forms.html#category-listed
static bool is_listed_element(DOM::Element const& element)
{
    // Denotes elements that are listed in the form.elements and fieldset.elements APIs.
    // These elements also have a form content attribute, and a matching form IDL attribute,
    // that allow authors to specify an explicit form owner.
    // => button, fieldset, input, object, output, select, textarea, form-associated custom elements

    if (is<HTMLButtonElement>(element)
        || is<HTMLFieldSetElement>(element)
        || is<HTMLInputElement>(element)
        || is<HTMLObjectElement>(element)
        || is<HTMLOutputElement>(element)
        || is<HTMLSelectElement>(element)
        || is<HTMLTextAreaElement>(element)) {
        return true;
    }

    // FIXME: Form-associated custom elements return also true

    return false;
}

static bool is_form_control(DOM::Element const& element, HTMLFormElement const& form)
{
    // The elements IDL attribute must return an HTMLFormControlsCollection rooted at the form element's root,
    // whose filter matches listed elements whose form owner is the form element,
    // with the exception of input elements whose type attribute is in the Image Button state, which must,
    // for historical reasons, be excluded from this particular collection.

    if (!is_listed_element(element))
        return false;

    if (is<HTMLInputElement>(element)
        && static_cast<HTMLInputElement const&>(element).type_state() == HTMLInputElement::TypeAttributeState::ImageButton) {
        return false;
    }

    auto const& form_associated_element = dynamic_cast<FormAssociatedElement const&>(element);
    if (form_associated_element.form() != &form)
        return false;

    return true;
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-elements
GC::Ref<HTMLFormControlsCollection> HTMLFormElement::elements() const
{
    if (!m_elements) {
        auto& root = as<ParentNode>(const_cast<HTMLFormElement*>(this)->root());
        m_elements = HTMLFormControlsCollection::create(root, DOM::HTMLCollection::Scope::Descendants, [this](Element const& element) {
            return is_form_control(element, *this);
        });
    }
    return *m_elements;
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-length
unsigned HTMLFormElement::length() const
{
    // The length IDL attribute must return the number of nodes represented by the elements collection.
    return elements()->length();
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-checkvalidity
WebIDL::ExceptionOr<bool> HTMLFormElement::check_validity()
{
    dbgln("(STUBBED) HTMLFormElement::check_validity(). Called on: {}", debug_description());
    return true;
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-reportvalidity
WebIDL::ExceptionOr<bool> HTMLFormElement::report_validity()
{
    dbgln("(STUBBED) HTMLFormElement::report_validity(). Called on: {}", debug_description());
    return true;
}

// https://html.spec.whatwg.org/multipage/forms.html#category-submit
Vector<GC::Ref<DOM::Element>> HTMLFormElement::get_submittable_elements()
{
    Vector<GC::Ref<DOM::Element>> submittable_elements;

    root().for_each_in_subtree([&](auto& node) {
        if (auto* form_associated_element = dynamic_cast<FormAssociatedElement*>(&node)) {
            if (form_associated_element->is_submittable() && form_associated_element->form() == this)
                submittable_elements.append(form_associated_element->form_associated_element_to_html_element());
        }

        return TraversalDecision::Continue;
    });

    return submittable_elements;
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-rellist
GC::Ref<DOM::DOMTokenList> HTMLFormElement::rel_list()
{
    // The relList IDL attribute must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-fs-method
WebIDL::ExceptionOr<void> HTMLFormElement::set_method(String const& method)
{
    // The method and enctype IDL attributes must reflect the respective content attributes of the same name, limited to only known values.
    return set_attribute(AttributeNames::method, method);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-fs-action
String HTMLFormElement::action() const
{
    // The action IDL attribute must reflect the content attribute of the same name, except that on getting, when the
    // content attribute is missing or its value is the empty string, the element's node document's URL must be returned
    // instead.
    auto form_action_attribute = attribute(AttributeNames::action);
    if (!form_action_attribute.has_value() || form_action_attribute.value().is_empty()) {
        return document().url_string();
    }

    return document().base_url().complete_url(form_action_attribute.value()).to_string();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-fs-action
WebIDL::ExceptionOr<void> HTMLFormElement::set_action(String const& value)
{
    return set_attribute(AttributeNames::action, value);
}

void HTMLFormElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.value_or(String {}));
    }
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#picking-an-encoding-for-the-form
ErrorOr<String> HTMLFormElement::pick_an_encoding() const
{
    // 1. Let encoding be the document's character encoding.
    auto encoding = document().encoding_or_default();

    // 2. If the form element has an accept-charset attribute, set encoding to the return value of running these substeps:
    if (auto maybe_input = attribute(AttributeNames::accept_charset); maybe_input.has_value()) {
        // 1. Let input be the value of the form element's accept-charset attribute.
        auto input = maybe_input.release_value();

        // 2. Let candidate encoding labels be the result of splitting input on ASCII whitespace.
        auto candidate_encoding_labels = input.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);

        // 3. Let candidate encodings be an empty list of character encodings.
        Vector<StringView> candidate_encodings;

        // 4. For each token in candidate encoding labels in turn (in the order in which they were found in input),
        //    get an encoding for the token and, if this does not result in failure, append the encoding to candidate
        //    encodings.
        for (auto const& token : candidate_encoding_labels) {
            auto candidate_encoding = TextCodec::get_standardized_encoding(token);
            if (candidate_encoding.has_value())
                TRY(candidate_encodings.try_append(candidate_encoding.value()));
        }

        // 5. If candidate encodings is empty, return UTF-8.
        if (candidate_encodings.is_empty())
            return "UTF-8"_string;

        // 6. Return the first encoding in candidate encodings.
        return String::from_utf8(candidate_encodings.first());
    }

    // 3. Return the result of getting an output encoding from encoding.
    return MUST(String::from_utf8(TextCodec::get_output_encoding(encoding)));
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#convert-to-a-list-of-name-value-pairs
static ErrorOr<Vector<DOMURL::QueryParam>> convert_to_list_of_name_value_pairs(Vector<XHR::FormDataEntry> const& entry_list)
{
    // 1. Let list be an empty list of name-value pairs.
    Vector<DOMURL::QueryParam> list;

    // 2. For each entry of entry list:
    for (auto const& entry : entry_list) {
        // 1. Let name be entry's name, with every occurrence of U+000D (CR) not followed by U+000A (LF), and every occurrence of U+000A (LF)
        //    not preceded by U+000D (CR), replaced by a string consisting of U+000D (CR) and U+000A (LF).
        auto name = TRY(normalize_line_breaks(entry.name));

        // 2. If entry's value is a File object, then let value be entry's value's name. Otherwise, let value be entry's value.
        String value;
        entry.value.visit(
            [&value](GC::Root<FileAPI::File> const& file) {
                value = file->name();
            },
            [&value](String const& string) {
                value = string;
            });

        // 3. Replace every occurrence of U+000D (CR) not followed by U+000A (LF), and every occurrence of
        //    U+000A (LF) not preceded by U+000D (CR), in value, by a string consisting of U+000D (CR) and U+000A (LF).
        auto normalized_value = TRY(normalize_line_breaks(value));

        // 4. Append to list a new name-value pair whose name is name and whose value is value.
        TRY(list.try_append(DOMURL::QueryParam { .name = move(name), .value = move(normalized_value) }));
    }

    // 3. Return list.
    return list;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#text/plain-encoding-algorithm
static ErrorOr<String> plain_text_encode(Vector<DOMURL::QueryParam> const& pairs)
{
    // 1. Let result be the empty string.
    StringBuilder result;

    // 2. For each pair in pairs:
    for (auto const& pair : pairs) {
        // 1. Append pair's name to result.
        TRY(result.try_append(pair.name));

        // 2. Append a single U+003D EQUALS SIGN character (=) to result.
        TRY(result.try_append('='));

        // 3. Append pair's value to result.
        TRY(result.try_append(pair.value));

        // 4. Append a U+000D CARRIAGE RETURN (CR) U+000A LINE FEED (LF) character pair to result.
        TRY(result.try_append("\r\n"sv));
    }

    // 3. Return result.
    return result.to_string();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#submit-mutate-action
ErrorOr<void> HTMLFormElement::mutate_action_url(URL::URL parsed_action, Vector<XHR::FormDataEntry> entry_list, String encoding, GC::Ref<Navigable> target_navigable, Bindings::NavigationHistoryBehavior history_handling, UserNavigationInvolvement user_involvement)
{
    // 1. Let pairs be the result of converting to a list of name-value pairs with entry list.
    auto pairs = TRY(convert_to_list_of_name_value_pairs(entry_list));

    // 2. Let query be the result of running the application/x-www-form-urlencoded serializer with pairs and encoding.
    auto query = url_encode(pairs, encoding);

    // 3. Set parsed action's query component to query.
    parsed_action.set_query(query);

    // 4. Plan to navigate to parsed action.
    plan_to_navigate_to(move(parsed_action), Empty {}, target_navigable, history_handling, user_involvement);
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#submit-body
ErrorOr<void> HTMLFormElement::submit_as_entity_body(URL::URL parsed_action, Vector<XHR::FormDataEntry> entry_list, EncodingTypeAttributeState encoding_type, [[maybe_unused]] String encoding, GC::Ref<Navigable> target_navigable, Bindings::NavigationHistoryBehavior history_handling, UserNavigationInvolvement user_involvement)
{
    // 1. Assert: method is POST.

    POSTResource::RequestContentType mime_type {};
    Vector<POSTResource::Directive> mime_type_directives;
    ByteBuffer body;

    // 2. Switch on enctype:
    switch (encoding_type) {
    case EncodingTypeAttributeState::FormUrlEncoded: {
        // -> application/x-www-form-urlencoded
        // 1. Let pairs be the result of converting to a list of name-value pairs with entry list.
        auto pairs = TRY(convert_to_list_of_name_value_pairs(entry_list));

        // 2. Let body be the result of running the application/x-www-form-urlencoded serializer with pairs and encoding.
        body = TRY(ByteBuffer::copy(url_encode(pairs, encoding).bytes()));

        // 3. Set body to the result of encoding body.
        // NOTE: `encoding` refers to `UTF-8 encode`, which body already is encoded as because it uses AK::String.

        // 4. Let mimeType be `application/x-www-form-urlencoded`.
        mime_type = POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded;
        break;
    }
    case EncodingTypeAttributeState::FormData: {
        // -> multipart/form-data
        // 1. Let body be the result of running the multipart/form-data encoding algorithm with entry list and encoding.
        auto body_and_mime_type = TRY(serialize_to_multipart_form_data(entry_list));
        body = move(body_and_mime_type.serialized_data);

        // 2. Let mimeType be the isomorphic encoding of the concatenation of "multipart/form-data; boundary=" and the multipart/form-data
        //    boundary string generated by the multipart/form-data encoding algorithm.
        mime_type = POSTResource::RequestContentType::MultipartFormData;
        mime_type_directives.empend("boundary"sv, move(body_and_mime_type.boundary));
        break;
    }
    case EncodingTypeAttributeState::PlainText: {
        // -> text/plain
        // 1. Let pairs be the result of converting to a list of name-value pairs with entry list.
        auto pairs = TRY(convert_to_list_of_name_value_pairs(entry_list));

        // 2. Let body be the result of running the text/plain encoding algorithm with pairs.
        body = TRY(ByteBuffer::copy(TRY(plain_text_encode(pairs)).bytes()));

        // FIXME: 3. Set body to the result of encoding body using encoding.

        // 4. Let mimeType be `text/plain`.
        mime_type = POSTResource::RequestContentType::TextPlain;
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }

    // 3. Plan to navigate to parsed action given a POST resource whose request body is body and request content-type is mimeType.
    plan_to_navigate_to(parsed_action, POSTResource { .request_body = move(body), .request_content_type = mime_type, .request_content_type_directives = move(mime_type_directives) }, target_navigable, history_handling, user_involvement);
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#submit-get-action
void HTMLFormElement::get_action_url(URL::URL parsed_action, GC::Ref<Navigable> target_navigable, Bindings::NavigationHistoryBehavior history_handling, UserNavigationInvolvement user_involvement)
{
    // 1. Plan to navigate to parsed action.
    // Spec Note: entry list is discarded.
    plan_to_navigate_to(move(parsed_action), Empty {}, target_navigable, history_handling, user_involvement);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#submit-mailto-headers
ErrorOr<void> HTMLFormElement::mail_with_headers(URL::URL parsed_action, Vector<XHR::FormDataEntry> entry_list, [[maybe_unused]] String encoding, GC::Ref<Navigable> target_navigable, Bindings::NavigationHistoryBehavior history_handling, UserNavigationInvolvement user_involvement)
{
    // 1. Let pairs be the result of converting to a list of name-value pairs with entry list.
    auto pairs = TRY(convert_to_list_of_name_value_pairs(entry_list));

    // 2. Let headers be the result of running the application/x-www-form-urlencoded serializer with pairs and encoding.
    auto headers = url_encode(pairs, encoding);

    // 3. Replace occurrences of U+002B PLUS SIGN characters (+) in headers with the string "%20".
    TRY(headers.replace("+"sv, "%20"sv, ReplaceMode::All));

    // 4. Set parsed action's query to headers.
    parsed_action.set_query(headers);

    // 5. Plan to navigate to parsed action.
    plan_to_navigate_to(move(parsed_action), Empty {}, target_navigable, history_handling, user_involvement);
    return {};
}

ErrorOr<void> HTMLFormElement::mail_as_body(URL::URL parsed_action, Vector<XHR::FormDataEntry> entry_list, EncodingTypeAttributeState encoding_type, [[maybe_unused]] String encoding, GC::Ref<Navigable> target_navigable, Bindings::NavigationHistoryBehavior history_handling, UserNavigationInvolvement user_involvement)
{
    // 1. Let pairs be the result of converting to a list of name-value pairs with entry list.
    auto pairs = TRY(convert_to_list_of_name_value_pairs(entry_list));

    String body;

    // 2. Switch on enctype:
    switch (encoding_type) {
    case EncodingTypeAttributeState::PlainText: {
        // -> text/plain
        // 1. Let body be the result of running the text/plain encoding algorithm with pairs.
        body = TRY(plain_text_encode(pairs));

        // 2. Set body to the result of running UTF-8 percent-encode on body using the default encode set. [URL]
        // NOTE: body is already UTF-8 encoded due to using AK::String, so we only have to do the percent encoding.
        // NOTE: "default encode set" links to "path percent-encode-set": https://url.spec.whatwg.org/#default-encode-set
        body = URL::percent_encode(body, URL::PercentEncodeSet::Path);
        break;
    }
    default:
        // -> Otherwise
        // Let body be the result of running the application/x-www-form-urlencoded serializer with pairs and encoding.
        body = url_encode(pairs, encoding);
        break;
    }

    // 3. If parsed action's query is null, then set it to the empty string.
    if (!parsed_action.query().has_value())
        parsed_action.set_query(String {});

    StringBuilder query_builder;

    query_builder.append(*parsed_action.query());

    // 4. If parsed action's query is not the empty string, then append a single U+0026 AMPERSAND character (&) to it.
    if (!parsed_action.query()->is_empty())
        TRY(query_builder.try_append('&'));

    // 5. Append "body=" to parsed action's query.
    TRY(query_builder.try_append("body="sv));

    // 6. Append body to parsed action's query.
    TRY(query_builder.try_append(body));

    parsed_action.set_query(MUST(query_builder.to_string()));

    // 7. Plan to navigate to parsed action.
    plan_to_navigate_to(move(parsed_action), Empty {}, target_navigable, history_handling, user_involvement);
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#plan-to-navigate
void HTMLFormElement::plan_to_navigate_to(URL::URL url, Variant<Empty, String, POSTResource> post_resource, GC::Ref<Navigable> target_navigable, Bindings::NavigationHistoryBehavior history_handling, UserNavigationInvolvement user_involvement)
{
    // 1. Let referrerPolicy be the empty string.
    ReferrerPolicy::ReferrerPolicy referrer_policy = ReferrerPolicy::ReferrerPolicy::EmptyString;

    // 2. If the form element's link types include the noreferrer keyword, then set referrerPolicy to "no-referrer".
    auto rel = MUST(get_attribute_value(HTML::AttributeNames::rel).to_lowercase());
    auto link_types = rel.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
    if (link_types.contains_slow("noreferrer"sv))
        referrer_policy = ReferrerPolicy::ReferrerPolicy::NoReferrer;

    // 3. If the form has a non-null planned navigation, remove it from its task queue.
    if (m_planned_navigation) {
        HTML::main_thread_event_loop().task_queue().remove_tasks_matching([this](Task const& task) {
            return &task == m_planned_navigation;
        });
    }

    // 4. Queue an element task on the DOM manipulation task source given the form element and the following steps:
    // NOTE: `this`, `actual_resource` and `target_navigable` are protected by GC::HeapFunction.
    queue_an_element_task(Task::Source::DOMManipulation, [this, url, post_resource, target_navigable, history_handling, referrer_policy, user_involvement]() {
        // 1. Set the form's planned navigation to null.
        m_planned_navigation = {};

        // 2. Navigate targetNavigable to url using the form element's node document, with historyHandling set to historyHandling,
        //    referrerPolicy set to referrerPolicy, documentResource set to postResource, and cspNavigationType set to "form-submission".
        MUST(target_navigable->navigate({ .url = url,
            .source_document = this->document(),
            .document_resource = post_resource,
            .response = nullptr,
            .exceptions_enabled = false,
            .history_handling = history_handling,
            .referrer_policy = referrer_policy,
            .user_involvement = user_involvement }));
    });

    // 5. Set the form's planned navigation to the just-queued task.
    m_planned_navigation = HTML::main_thread_event_loop().task_queue().last_added_task();
    VERIFY(m_planned_navigation);
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-item
Optional<JS::Value> HTMLFormElement::item_value(size_t index) const
{
    // To determine the value of an indexed property for a form element, the user agent must return the value returned by
    // the item method on the elements collection, when invoked with the given index as its argument.
    if (auto value = elements()->item(index))
        return value;
    return {};
}

// https://html.spec.whatwg.org/multipage/forms.html#the-form-element:supported-property-names
Vector<FlyString> HTMLFormElement::supported_property_names() const
{
    // The supported property names consist of the names obtained from the following algorithm, in the order obtained from this algorithm:

    // 1. Let sourced names be an initially empty ordered list of tuples consisting of a string, an element, a source,
    //    where the source is either id, name, or past, and, if the source is past, an age.
    struct SourcedName {
        FlyString name;
        GC::Ptr<DOM::Element const> element;
        enum class Source {
            Id,
            Name,
            Past,
        } source;
        AK::Duration age;
    };
    Vector<SourcedName> sourced_names;

    // 2. For each listed element candidate whose form owner is the form element, with the exception of any
    //    input elements whose type attribute is in the Image Button state:
    for (auto const& candidate : m_associated_elements) {
        if (!is_form_control(*candidate, *this))
            continue;

        // 1. If candidate has an id attribute, add an entry to sourced names with that id attribute's value as the
        //    string, candidate as the element, and id as the source.
        if (candidate->id().has_value())
            sourced_names.append(SourcedName { candidate->id().value(), candidate, SourcedName::Source::Id, {} });

        // 2. If candidate has a name attribute, add an entry to sourced names with that name attribute's value as the
        //    string, candidate as the element, and name as the source.
        if (candidate->name().has_value())
            sourced_names.append(SourcedName { candidate->name().value(), candidate, SourcedName::Source::Name, {} });
    }

    // 3. For each img element candidate whose form owner is the form element:
    for (auto const& candidate : m_associated_elements) {
        if (!is<HTMLImageElement>(*candidate))
            continue;

        // Every element in m_associated_elements has this as the form owner.

        // 1. If candidate has an id attribute, add an entry to sourced names with that id attribute's value as the
        //    string, candidate as the element, and id as the source.
        if (candidate->id().has_value())
            sourced_names.append(SourcedName { candidate->id().value(), candidate, SourcedName::Source::Id, {} });

        // 2. If candidate has a name attribute, add an entry to sourced names with that name attribute's value as the
        //    string, candidate as the element, and name as the source.
        if (candidate->name().has_value())
            sourced_names.append(SourcedName { candidate->name().value(), candidate, SourcedName::Source::Name, {} });
    }

    // 4. For each entry past entry in the past names map add an entry to sourced names with the past entry's name as
    //    the string, past entry's element as the element, past as the source, and the length of time past entry has
    //    been in the past names map as the age.
    auto const now = MonotonicTime::now();
    for (auto const& entry : m_past_names_map)
        sourced_names.append(SourcedName { entry.key, static_cast<DOM::Element const*>(entry.value.node.ptr()), SourcedName::Source::Past, now - entry.value.insertion_time });

    // 5. Sort sourced names by tree order of the element entry of each tuple, sorting entries with the same element by
    //    putting entries whose source is id first, then entries whose source is name, and finally entries whose source
    //    is past, and sorting entries with the same element and source by their age, oldest first.
    // FIXME: Require less const casts here by changing the signature of DOM::Node::compare_document_position
    quick_sort(sourced_names, [](auto const& lhs, auto const& rhs) -> bool {
        if (lhs.element != rhs.element)
            return const_cast<DOM::Element*>(lhs.element.ptr())->compare_document_position(const_cast<DOM::Element*>(rhs.element.ptr())) & DOM::Node::DOCUMENT_POSITION_FOLLOWING;
        if (lhs.source != rhs.source)
            return lhs.source < rhs.source;
        return lhs.age < rhs.age;
    });

    // FIXME: Surely there's a more efficient way to do this without so many FlyStrings and collections?
    // 6. Remove any entries in sourced names that have the empty string as their name.
    // 7. Remove any entries in sourced names that have the same name as an earlier entry in the map.
    // 8. Return the list of names from sourced names, maintaining their relative order.
    OrderedHashTable<FlyString> names;
    names.ensure_capacity(sourced_names.size());
    for (auto const& entry : sourced_names) {
        if (entry.name.is_empty())
            continue;
        names.set(entry.name, AK::HashSetExistingEntryBehavior::Keep);
    }

    Vector<FlyString> result;
    result.ensure_capacity(names.size());
    for (auto const& name : names)
        result.unchecked_append(name);

    return result;
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-form-nameditem
JS::Value HTMLFormElement::named_item_value(FlyString const& name) const
{
    auto& realm = this->realm();
    auto& root = as<ParentNode>(this->root());

    // To determine the value of a named property name for a form element, the user agent must run the following steps:

    // 1. Let candidates be a live RadioNodeList object containing all the listed elements, whose form owner is the form
    //    element, that have either an id attribute or a name attribute equal to name, with the exception of input
    //    elements whose type attribute is in the Image Button state, in tree order.
    auto candidates = RadioNodeList::create(realm, root, DOM::LiveNodeList::Scope::Descendants, [this, name](auto& node) -> bool {
        if (!is<DOM::Element>(node))
            return false;
        auto const& element = static_cast<DOM::Element const&>(node);

        // Form controls are defined as listed elements, with the exception of input elements in the Image Button state,
        // whose form owner is the form element.
        if (!is_form_control(element, *this))
            return false;

        return name == element.id() || name == element.name();
    });

    // 2. If candidates is empty, let candidates be a live RadioNodeList object containing all the img elements,
    //    whose form owner is the form element, that have either an id attribute or a name attribute equal to name,
    //    in tree order.
    if (candidates->length() == 0) {
        candidates = RadioNodeList::create(realm, root, DOM::LiveNodeList::Scope::Descendants, [this, name](auto& node) -> bool {
            if (!is<HTMLImageElement>(node))
                return false;

            auto const& element = static_cast<HTMLImageElement const&>(node);
            if (element.form() != this)
                return false;

            return name == element.id() || name == element.name();
        });
    }

    auto length = candidates->length();

    // 3. If candidates is empty, name is the name of one of the entries in the form element's past names map: return the object associated with name in that map.
    if (length == 0) {
        auto it = m_past_names_map.find(name);
        if (it != m_past_names_map.end())
            return it->value.node;
    }

    // 4. If candidates contains more than one node, return candidates.
    if (length > 1)
        return candidates;

    // 5. Otherwise, candidates contains exactly one node. Add a mapping from name to the node in candidates in the form
    //    element's past names map, replacing the previous entry with the same name, if any.
    auto const* node = candidates->item(0);
    m_past_names_map.set(name, HTMLFormElement::PastNameEntry { .node = node, .insertion_time = MonotonicTime::now() });

    // 6. Return the node in candidates.
    return node;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#default-button
FormAssociatedElement* HTMLFormElement::default_button()
{
    // A form element's default button is the first submit button in tree order whose form owner is that form element.
    FormAssociatedElement* default_button = nullptr;

    root().for_each_in_subtree([&](auto& node) {
        auto* form_associated_element = dynamic_cast<FormAssociatedElement*>(&node);
        if (!form_associated_element)
            return TraversalDecision::Continue;

        if (form_associated_element->form() == this && form_associated_element->is_submit_button()) {
            default_button = form_associated_element;
            return TraversalDecision::Break;
        }

        return TraversalDecision::Continue;
    });

    return default_button;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#field-that-blocks-implicit-submission
size_t HTMLFormElement::number_of_fields_blocking_implicit_submission() const
{
    // For the purpose of the previous paragraph, an element is a field that blocks implicit submission of a form
    // element if it is an input element whose form owner is that form element and whose type attribute is in one of
    // the following states: Text, Search, Telephone, URL, Email, Password, Date, Month, Week, Time,
    // Local Date and Time, Number.
    size_t count = 0;

    for (auto element : m_associated_elements) {
        if (!is<HTMLInputElement>(*element))
            continue;

        auto const& input = static_cast<HTMLInputElement&>(*element);
        using enum HTMLInputElement::TypeAttributeState;

        switch (input.type_state()) {
        case Text:
        case Search:
        case Telephone:
        case URL:
        case Email:
        case Password:
        case Date:
        case Month:
        case Week:
        case Time:
        case LocalDateAndTime:
        case Number:
            ++count;
            break;

        default:
            break;
        }
    };

    return count;
}

}
