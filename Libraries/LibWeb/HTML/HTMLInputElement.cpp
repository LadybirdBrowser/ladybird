/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Adam Hodgen <ant1441@gmail.com>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Fernando Kiotheka <fer@k6a.dev>
 * Copyright (c) 2025, Felipe Muñoz Mazur <felipe.munoz.mazur@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/DateTime.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/HTMLInputElementPrototype.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Dates.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLDivElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/CheckBox.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/RadioButton.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/MimeSniff/Resource.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLInputElement);

HTMLInputElement::HTMLInputElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLInputElement::~HTMLInputElement() = default;

void HTMLInputElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLInputElement);
}

void HTMLInputElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_inner_text_element);
    visitor.visit(m_text_node);
    visitor.visit(m_placeholder_element);
    visitor.visit(m_placeholder_text_node);
    visitor.visit(m_color_well_element);
    visitor.visit(m_file_button);
    visitor.visit(m_file_label);
    visitor.visit(m_legacy_pre_activation_behavior_checked_element_in_group);
    visitor.visit(m_selected_files);
    visitor.visit(m_slider_runnable_track);
    visitor.visit(m_slider_progress_element);
    visitor.visit(m_slider_thumb);
    visitor.visit(m_resource_request);
}

GC::Ptr<Layout::Node> HTMLInputElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    if (type_state() == TypeAttributeState::Hidden)
        return nullptr;

    // NOTE: Image inputs are `appearance: none` per the default UA style,
    //       but we still need to create an ImageBox for them, or no image will get loaded.
    if (type_state() == TypeAttributeState::ImageButton) {
        return heap().allocate<Layout::ImageBox>(document(), *this, move(style), *this);
    }

    // https://drafts.csswg.org/css-ui/#appearance-switching
    // This specification introduces the appearance property to provide some control over this behavior.
    // In particular, using appearance: none allows authors to suppress the native appearance of widgets,
    // giving them a primitive appearance where CSS can be used to restyle them.
    if (style->appearance() == CSS::Appearance::None) {
        return Element::create_layout_node_for_display_type(document(), style->display(), style, this);
    }

    if (type_state() == TypeAttributeState::SubmitButton || type_state() == TypeAttributeState::Button || type_state() == TypeAttributeState::ResetButton)
        return heap().allocate<Layout::BlockContainer>(document(), this, move(style));

    if (type_state() == TypeAttributeState::Checkbox)
        return heap().allocate<Layout::CheckBox>(document(), *this, move(style));

    if (type_state() == TypeAttributeState::RadioButton)
        return heap().allocate<Layout::RadioButton>(document(), *this, move(style));

    return Element::create_layout_node_for_display_type(document(), style->display(), style, this);
}

void HTMLInputElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    if (type_state() == TypeAttributeState::Hidden || type_state() == TypeAttributeState::SubmitButton || type_state() == TypeAttributeState::Button || type_state() == TypeAttributeState::ResetButton || type_state() == TypeAttributeState::ImageButton || type_state() == TypeAttributeState::Checkbox || type_state() == TypeAttributeState::RadioButton)
        return;

    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));

    // AD-HOC: We rewrite `display: inline` to `display: inline-block`.
    //         This is required for the internal shadow tree to work correctly in layout.
    if (style.display().is_inline_outside() && style.display().is_flow_inside())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::InlineBlock)));

    if (type_state() != TypeAttributeState::FileUpload) {
        if (style.property(CSS::PropertyID::Width).has_auto())
            style.set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length(size(), CSS::Length::Type::Ch)));
    }

    // NOTE: The following line-height check is done for web compatability and usability reasons.
    // FIXME: The "normal" line-height value should be calculated but assume 1.0 for now.
    double normal_line_height = 1.0;
    double current_line_height = style.line_height().to_double();

    if (is_single_line() && current_line_height < normal_line_height)
        style.set_property(CSS::PropertyID::LineHeight, CSS::CSSKeywordValue::create(CSS::Keyword::Normal));
}

void HTMLInputElement::set_checked(bool checked)
{
    // The dirty checkedness flag must be initially set to false when the element is created,
    // and must be set to true whenever the user interacts with the control in a way that changes the checkedness.
    m_dirty_checkedness = true;
    if (m_checked == checked)
        return;

    m_checked = checked;

    invalidate_style(
        DOM::StyleInvalidationReason::HTMLInputElementSetChecked,
        { { .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Checked } },
        {});

    if (auto* paintable = this->paintable())
        paintable->set_needs_display();
}

void HTMLInputElement::set_checked_binding(bool checked)
{
    if (type_state() == TypeAttributeState::RadioButton) {
        if (checked)
            set_checked_within_group();
        else
            set_checked(false);
    } else {
        set_checked(checked);
    }
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-indeterminate
void HTMLInputElement::set_indeterminate(bool value)
{
    // On setting, it must be set to the new value. It has no effect except for changing the appearance of checkbox controls.
    m_indeterminate = value;
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-list
GC::Ptr<HTMLDataListElement const> HTMLInputElement::list() const
{
    // The list IDL attribute must return the current suggestions source element, if any, or null otherwise.
    if (auto data_list_element = suggestions_source_element(); data_list_element.has_value())
        return *data_list_element;

    return nullptr;
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-list
Optional<GC::Ref<HTMLDataListElement const>> HTMLInputElement::suggestions_source_element() const
{
    // The suggestions source element is the first element in the tree in tree order to have an ID equal to the value of the list attribute,
    // if that element is a datalist element. If there is no list attribute, or if there is no element with that ID,
    // or if the first element with that ID is not a datalist element, then there is no suggestions source element.
    Optional<GC::Ref<HTMLDataListElement const>> result;
    if (auto list_attribute_value = get_attribute(HTML::AttributeNames::list); list_attribute_value.has_value()) {
        root().for_each_in_inclusive_subtree_of_type<DOM::Element>([&](auto& element) {
            if (element.id() == *list_attribute_value) {
                if (auto data_list_element = as_if<HTMLDataListElement>(element))
                    result = *data_list_element;

                return TraversalDecision::Break;
            }

            return TraversalDecision::Continue;
        });
    }

    return result;
}

// https://html.spec.whatwg.org/multipage/input.html#compiled-pattern-regular-expression
Optional<Regex<ECMA262>> HTMLInputElement::compiled_pattern_regular_expression() const
{
    // 1. If the element does not have a pattern attribute specified, then return nothing. The element has no compiled pattern regular expression.
    auto maybe_pattern = get_attribute(HTML::AttributeNames::pattern);
    if (!maybe_pattern.has_value())
        return {};

    // 2. Let pattern be the value of the pattern attribute of the element.
    auto pattern = maybe_pattern.release_value().to_byte_string();

    // 3. Let regexpCompletion be RegExpCreate(pattern, "v").
    Regex<ECMA262> regexp_completion(pattern, JS::RegExpObject::default_flags | ECMAScriptFlags::UnicodeSets);

    // 4. If regexpCompletion is an abrupt completion, then return nothing. The element has no compiled pattern regular expression.
    if (regexp_completion.parser_result.error != regex::Error::NoError)
        return {};

    // 5. Let anchoredPattern be the string "^(?:", followed by pattern, followed by ")$".
    auto anchored_pattern = ByteString::formatted("^(?:{})$", pattern);

    // 6. Return ! RegExpCreate(anchoredPattern, "v").
    return Regex<ECMA262>(anchored_pattern, JS::RegExpObject::default_flags | ECMAScriptFlags::UnicodeSets);
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-files
GC::Ptr<FileAPI::FileList> HTMLInputElement::files()
{
    // On getting, if the IDL attribute applies, it must return a FileList object that represents the current selected files.
    //  The same object must be returned until the list of selected files changes.
    // If the IDL attribute does not apply, then it must instead return null.
    if (m_type != TypeAttributeState::FileUpload)
        return nullptr;

    if (!m_selected_files)
        m_selected_files = FileAPI::FileList::create(realm());
    return m_selected_files;
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-files
void HTMLInputElement::set_files(GC::Ptr<FileAPI::FileList> files)
{
    // 1. If the IDL attribute does not apply or the given value is null, then return.
    if (m_type != TypeAttributeState::FileUpload || files == nullptr)
        return;

    // 2. Replace the element's selected files with the given value.
    m_selected_files = files;
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-accept
FileFilter HTMLInputElement::parse_accept_attribute() const
{
    FileFilter filter;

    // If specified, the attribute must consist of a set of comma-separated tokens, each of which must be an ASCII
    // case-insensitive match for one of the following:
    auto accept = get_attribute_value(HTML::AttributeNames::accept);

    accept.bytes_as_string_view().for_each_split_view(',', SplitBehavior::Nothing, [&](StringView value) {
        // The string "audio/*"
        //     Indicates that sound files are accepted.
        if (value.equals_ignoring_ascii_case("audio/*"sv))
            filter.add_filter(FileFilter::FileType::Audio);

        // The string "video/*"
        //     Indicates that video files are accepted.
        if (value.equals_ignoring_ascii_case("video/*"sv))
            filter.add_filter(FileFilter::FileType::Video);

        // The string "image/*"
        //     Indicates that image files are accepted.
        if (value.equals_ignoring_ascii_case("image/*"sv))
            filter.add_filter(FileFilter::FileType::Image);

        // A valid MIME type string with no parameters
        //     Indicates that files of the specified type are accepted.
        else if (auto mime_type = MimeSniff::MimeType::parse(value); mime_type.has_value() && mime_type->parameters().is_empty())
            filter.add_filter(FileFilter::MimeType { mime_type->essence() });

        // A string whose first character is a U+002E FULL STOP character (.)
        //     Indicates that files with the specified file extension are accepted.
        else if (value.starts_with('.'))
            filter.add_filter(FileFilter::Extension { MUST(String::from_utf8(value.substring_view(1))) });
    });

    return filter;
}

// https://html.spec.whatwg.org/multipage/input.html#update-the-file-selection
void HTMLInputElement::update_the_file_selection(GC::Ref<FileAPI::FileList> files)
{
    // 1. Queue an element task on the user interaction task source given element and the following steps:
    queue_an_element_task(Task::Source::UserInteraction, [this, files] {
        // 1. Update element's selected files so that it represents the user's selection.
        this->set_files(files.ptr());

        // 2. Fire an event named input at the input element, with the bubbles and composed attributes initialized to true.
        auto input_event = DOM::Event::create(this->realm(), EventNames::input, { .bubbles = true, .composed = true });
        this->dispatch_event(input_event);

        // 3. Fire an event named change at the input element, with the bubbles attribute initialized to true.
        auto change_event = DOM::Event::create(this->realm(), EventNames::change, { .bubbles = true });
        this->dispatch_event(change_event);
    });
}

// https://html.spec.whatwg.org/multipage/input.html#show-the-picker,-if-applicable
static void show_the_picker_if_applicable(HTMLInputElement& element)
{
    // To show the picker, if applicable for an input element element:

    // 1. If element's relevant global object does not have transient activation, then return.
    auto& global_object = relevant_global_object(element);
    if (!is<HTML::Window>(global_object))
        return;
    auto& relevant_global_object = static_cast<HTML::Window&>(global_object);
    if (!relevant_global_object.has_transient_activation())
        return;

    // 2. If element is not mutable, then return.
    if (!element.is_mutable())
        return;

    // 3. Consume user activation given element's relevant global object.
    relevant_global_object.consume_user_activation();

    // 4. If element does not support a picker, then return.
    if (!element.supports_a_picker())
        return;

    // 5. If element is an input element and element's type attribute is in the File Upload state, then run these steps in parallel:
    if (element.type_state() == HTMLInputElement::TypeAttributeState::FileUpload) {
        // NOTE: These steps cannot be fully implemented here, and must be done in the PageClient when the response comes back from the PageHost
        //       See: ViewImplementation::on_request_file_picker, Page::did_request_file_picker(), Page::file_picker_closed()

        // 1. Optionally, wait until any prior execution of this algorithm has terminated.
        // FIXME: 2. Let dismissed be the result of WebDriver BiDi file dialog opened with element.
        bool dismissed = false;
        // 3. If dismissed is false:
        if (!dismissed) {
            // 1. Display a prompt to the user requesting that the user specify some files.
            //    If the multiple attribute is not set on element, there must be no more than one file selected;
            //    otherwise, any number may be selected.
            //    Files can be from the filesystem or created on the fly, e.g., a picture taken from a camera connected
            //    to the user's device.
            // 2. Wait for the user to have made their selection.
            auto accepted_file_types = element.parse_accept_attribute();
            auto allow_multiple_files = element.has_attribute(HTML::AttributeNames::multiple) ? AllowMultipleFiles::Yes : AllowMultipleFiles::No;
            auto weak_element = element.make_weak_ptr<HTMLInputElement>();

            element.set_is_open(true);
            element.document().browsing_context()->top_level_browsing_context()->page().did_request_file_picker(weak_element, accepted_file_types, allow_multiple_files);
        }
        // 4. If dismissed is true or if the user dismissed the prompt without changing their selection,
        //    then queue an element task on the user interaction task source given element to fire an event named cancel at element,
        //    with the bubbles attribute initialized to true.
        else {
            // FIXME: Handle the "dismissed is true" case here.
        }
        // 5. Otherwise, update the file selection for element.
    }

    // 6. Otherwise, the user agent should show the relevant user interface for selecting a value for element, in the
    //    way it normally would when the user interacts with the control.
    //    When showing such a user interface, it must respect the requirements stated in the relevant parts of the
    //    specification for how element behaves given its type attribute state. (For example, various sections describe
    //    restrictions on the resulting value string.)
    //    This step can have side effects, such as closing other pickers that were previously shown by this algorithm.
    //    (If this closes a file selection picker, then per the above that will lead to firing either input and change
    //    events, or a cancel event.)
    else {
        if (element.type_state() == HTMLInputElement::TypeAttributeState::Color) {
            auto weak_element = element.make_weak_ptr<HTMLInputElement>();
            element.set_is_open(true);
            element.document().browsing_context()->top_level_browsing_context()->page().did_request_color_picker(weak_element, Color::from_string(element.value()).value_or(Color(0, 0, 0)));
        }
    }
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-showpicker
WebIDL::ExceptionOr<void> HTMLInputElement::show_picker()
{
    // The showPicker() method steps are:

    // 1. If this is not mutable, then throw an "InvalidStateError" DOMException.
    if (!is_mutable())
        return WebIDL::InvalidStateError::create(realm(), "Element is not mutable"_string);

    // 2. If this's relevant settings object's origin is not same origin with this's relevant settings object's top-level origin,
    // and this's type attribute is not in the File Upload state or Color state, then throw a "SecurityError" DOMException.
    // NOTE: File and Color inputs are exempted from this check for historical reason: their input activation behavior also shows their pickers,
    //       and has never been guarded by an origin check.
    if (!relevant_settings_object(*this).origin().is_same_origin(relevant_settings_object(*this).top_level_origin)
        && m_type != TypeAttributeState::FileUpload && m_type != TypeAttributeState::Color) {
        return WebIDL::SecurityError::create(realm(), "Cross origin pickers are not allowed"_string);
    }

    // 3. If this's relevant global object does not have transient activation, then throw a "NotAllowedError" DOMException.
    // FIXME: The global object we get here should probably not need casted to Window to check for transient activation
    auto& global_object = relevant_global_object(*this);
    if (!is<HTML::Window>(global_object) || !static_cast<HTML::Window&>(global_object).has_transient_activation()) {
        return WebIDL::NotAllowedError::create(realm(), "Too long since user activation to show picker"_string);
    }

    // 4. Show the picker, if applicable, for this.
    show_the_picker_if_applicable(*this);
    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#input-activation-behavior
WebIDL::ExceptionOr<void> HTMLInputElement::run_input_activation_behavior(DOM::Event const& event)
{
    if (type_state() == TypeAttributeState::Checkbox || type_state() == TypeAttributeState::RadioButton) {
        // 1. If the element is not connected, then return.
        if (!is_connected())
            return {};

        // 2. Fire an event named input at the element with the bubbles and composed attributes initialized to true.
        auto input_event = DOM::Event::create(realm(), HTML::EventNames::input);
        input_event->set_bubbles(true);
        input_event->set_composed(true);
        dispatch_event(input_event);

        // 3. Fire an event named change at the element with the bubbles attribute initialized to true.
        auto change_event = DOM::Event::create(realm(), HTML::EventNames::change);
        change_event->set_bubbles(true);
        dispatch_event(*change_event);
    }
    // https://html.spec.whatwg.org/multipage/input.html#submit-button-state-(type=submit)
    else if (type_state() == TypeAttributeState::SubmitButton) {
        GC::Ptr<HTMLFormElement> form;

        // The input element represents a button that, when activated, submits the form.
        if (is_actually_disabled())
            return {};

        // 1. If the element does not have a form owner, then return.
        if (!(form = this->form()))
            return {};

        // 2. If the element's node document is not fully active, then return.
        if (!document().is_fully_active())
            return {};

        // 3. Submit the element's form owner from the element with userInvolvement set to event's user navigation involvement.
        TRY(form->submit_form(*this, { .user_involvement = user_navigation_involvement(event) }));
    } else if (type_state() == TypeAttributeState::FileUpload || type_state() == TypeAttributeState::Color) {
        show_the_picker_if_applicable(*this);
    }
    // https://html.spec.whatwg.org/multipage/input.html#image-button-state-(type=image):input-activation-behavior
    else if (type_state() == TypeAttributeState::ImageButton) {
        // 1. If the element does not have a form owner, then return.
        auto* form = this->form();
        if (!form)
            return {};

        // 2. If the element's node document is not fully active, then return.
        if (!document().is_fully_active())
            return {};

        // 3. If the user activated the control while explicitly selecting a coordinate, then set the element's selected
        //    coordinate to that coordinate.
        if (event.is_trusted() && is<UIEvents::MouseEvent>(event)) {
            auto const& mouse_event = static_cast<UIEvents::MouseEvent const&>(event);

            CSSPixels x { mouse_event.offset_x() };
            CSSPixels y { mouse_event.offset_y() };

            m_selected_coordinate = { x.to_int(), y.to_int() };
        }

        // 4. Submit the element's form owner from the element with userInvolvement set to event's user navigation involvement.
        TRY(form->submit_form(*this, { .user_involvement = user_navigation_involvement(event) }));
    }
    // https://html.spec.whatwg.org/multipage/input.html#reset-button-state-(type=reset)
    else if (type_state() == TypeAttributeState::ResetButton) {
        // The input element represents a button that, when activated, resets the form.
        if (is_actually_disabled())
            return {};

        // 1. If the element does not have a form owner, then return.
        auto* form = this->form();
        if (!form)
            return {};

        // 2. If the element's node document is not fully active, then return.
        if (!document().is_fully_active())
            return {};

        // 3. Reset the form owner from the element.
        form->reset_form();
    }

    return {};
}

void HTMLInputElement::did_edit_text_node()
{
    // An input element's dirty value flag must be set to true whenever the user interacts with the control in a way that changes the value.
    auto old_value = move(m_value);
    m_value = value_sanitization_algorithm(m_text_node->data());
    m_dirty_value = true;

    m_has_uncommitted_changes = true;

    if (m_value != old_value)
        relevant_value_was_changed();

    update_placeholder_visibility();

    user_interaction_did_change_input_value();
}

void HTMLInputElement::did_pick_color(Optional<Color> picked_color, ColorPickerUpdateState state)
{
    set_is_open(false);

    if (type_state() == TypeAttributeState::Color && picked_color.has_value()) {
        // then when the user changes the element's value
        m_value = value_sanitization_algorithm(picked_color.value().to_string_without_alpha());
        m_dirty_value = true;

        update_color_well_element();

        // the user agent must queue an element task on the user interaction task source
        user_interaction_did_change_input_value();

        // https://html.spec.whatwg.org/multipage/input.html#common-input-element-events
        // [...] any time the user commits the change, the user agent must queue an element task on the user interaction task source
        if (state == ColorPickerUpdateState::Closed) {
            queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
                // given the input element
                // to set its user validity to true
                m_user_validity = true;
                // and fire an event named change at the input element, with the bubbles attribute initialized to true.
                auto change_event = DOM::Event::create(realm(), HTML::EventNames::change);
                change_event->set_bubbles(true);
                dispatch_event(*change_event);
            });
        }
    }
}

void HTMLInputElement::did_select_files(Span<SelectedFile> selected_files, MultipleHandling multiple_handling)
{
    set_is_open(false);

    // https://html.spec.whatwg.org/multipage/input.html#show-the-picker,-if-applicable
    // 4. If the user dismissed the prompt without changing their selection, then queue an element task on the user
    //    interaction task source given element to fire an event named cancel at element, with the bubbles attribute
    //    initialized to true.
    if (selected_files.is_empty()) {
        queue_an_element_task(HTML::Task::Source::UserInteraction, [this]() {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::cancel, { .bubbles = true }));
        });

        return;
    }

    auto files = FileAPI::FileList::create(realm());

    for (auto& selected_file : selected_files) {
        auto contents = selected_file.take_contents();

        auto mime_type = MimeSniff::Resource::sniff(contents);
        auto blob = FileAPI::Blob::create(realm(), move(contents), mime_type.essence());

        // FIXME: The FileAPI should use ByteString for file names.
        auto file_name = MUST(String::from_byte_string(selected_file.name()));

        // FIXME: Fill in other fields (e.g. last_modified).
        FileAPI::FilePropertyBag options {};
        options.type = mime_type.essence();

        auto file = MUST(FileAPI::File::create(realm(), { GC::make_root(blob) }, file_name, move(options)));
        files->add_file(file);
    }

    // https://html.spec.whatwg.org/multipage/input.html#update-the-file-selection
    // 1. Queue an element task on the user interaction task source given element and the following steps:
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this, files, multiple_handling]() mutable {
        auto multiple = has_attribute(HTML::AttributeNames::multiple);

        // 1. Update element's selected files so that it represents the user's selection.
        if (m_selected_files && multiple && multiple_handling == MultipleHandling::Append) {
            for (size_t i = 0; i < files->length(); ++i)
                m_selected_files->add_file(*files->item(i));
        } else {
            m_selected_files = files;
        }

        update_file_input_shadow_tree();

        // 2. Fire an event named input at the input element, with the bubbles and composed attributes initialized to true.
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::input, { .bubbles = true, .composed = true }));

        // 3. Fire an event named change at the input element, with the bubbles attribute initialized to true.
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::change, { .bubbles = true }));
    });
}

String HTMLInputElement::value() const
{
    switch (value_attribute_mode()) {
    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-value
    case ValueAttributeMode::Value:
        // Return the current value of the element.
        return m_value;

    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-default
    case ValueAttributeMode::Default:
        // On getting, if the element has a value content attribute, return that attribute's value; otherwise, return
        // the empty string.
        return get_attribute_value(AttributeNames::value);

    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-default-on
    case ValueAttributeMode::DefaultOn:
        // On getting, if the element has a value content attribute, return that attribute's value; otherwise, return
        // the string "on".
        return get_attribute(AttributeNames::value).value_or("on"_string);

    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-filename
    case ValueAttributeMode::Filename:
        // On getting, return the string "C:\fakepath\" followed by the name of the first file in the list of selected
        // files, if any, or the empty string if the list is empty.
        if (m_selected_files && m_selected_files->item(0))
            return MUST(String::formatted("C:\\fakepath\\{}", m_selected_files->item(0)->name()));
        return String {};
    }

    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> HTMLInputElement::set_value(String const& value)
{
    auto& realm = this->realm();

    switch (value_attribute_mode()) {
    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-value
    case ValueAttributeMode::Value: {
        // 1. Let oldValue be the element's value.
        auto old_value = move(m_value);

        // 2. Set the element's value to the new value.
        // NOTE: For the TextNode this is done as part of step 4 below.

        // 3. Set the element's dirty value flag to true.
        m_dirty_value = true;

        // 4. Invoke the value sanitization algorithm, if the element's type attribute's current state defines one.
        m_value = value_sanitization_algorithm(value);

        // 5. If the element's value (after applying the value sanitization algorithm) is different from oldValue,
        //    and the element has a text entry cursor position, move the text entry cursor position to the end of the
        //    text control, unselecting any selected text and resetting the selection direction to "none".
        if (m_value != old_value) {
            relevant_value_was_changed();

            if (m_text_node) {
                m_text_node->set_data(m_value);
                update_placeholder_visibility();

                set_the_selection_range(m_text_node->length(), m_text_node->length());
            }

            update_shadow_tree();
        }

        break;
    }

    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-default
    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-default-on
    case ValueAttributeMode::Default:
    case ValueAttributeMode::DefaultOn:
        // On setting, set the value of the element's value content attribute to the new value.
        TRY(set_attribute(HTML::AttributeNames::value, value));
        break;

    // https://html.spec.whatwg.org/multipage/input.html#dom-input-value-filename
    case ValueAttributeMode::Filename:
        // On setting, if the new value is the empty string, empty the list of selected files; otherwise, throw an "InvalidStateError" DOMException.
        if (!value.is_empty())
            return WebIDL::InvalidStateError::create(realm, "Setting value of input type file to non-empty string"_string);

        m_selected_files = nullptr;
        break;
    }

    return {};
}

void HTMLInputElement::commit_pending_changes()
{
    // The change event fires when the value is committed, if that makes sense for the control,
    // or else when the control loses focus
    switch (type_state()) {
    case TypeAttributeState::Email:
    case TypeAttributeState::Password:
    case TypeAttributeState::Search:
    case TypeAttributeState::Telephone:
    case TypeAttributeState::Text:
    case TypeAttributeState::URL:
    case TypeAttributeState::Checkbox:
    case TypeAttributeState::RadioButton:
        if (!m_has_uncommitted_changes)
            return;
        break;

    default:
        break;
    }

    m_has_uncommitted_changes = false;

    auto change_event = DOM::Event::create(realm(), HTML::EventNames::change, { .bubbles = true });
    dispatch_event(change_event);
}

static GC::Ref<CSS::CSSStyleProperties> placeholder_style_when_visible()
{
    static GC::Root<CSS::CSSStyleProperties> style;
    if (!style) {
        style = CSS::CSSStyleProperties::create(internal_css_realm(), {}, {});
        style->set_declarations_from_text(R"~~~(
                width: 100%;
                align-items: center;
                text-overflow: clip;
                white-space: nowrap;
                display: block;
            )~~~"sv);
    }
    return *style;
}

static GC::Ref<CSS::CSSStyleProperties> placeholder_style_when_hidden()
{
    static GC::Root<CSS::CSSStyleProperties> style;
    if (!style) {
        style = CSS::CSSStyleProperties::create(internal_css_realm(), {}, {});
        style->set_declarations_from_text("display: none;"sv);
    }
    return *style;
}

void HTMLInputElement::update_placeholder_visibility()
{
    if (!m_placeholder_element)
        return;
    if (this->placeholder_value().has_value())
        m_placeholder_element->set_inline_style(placeholder_style_when_visible());
    else
        m_placeholder_element->set_inline_style(placeholder_style_when_hidden());
}

void HTMLInputElement::update_button_input_shadow_tree()
{
    if (m_text_node) {
        Optional<String> label = get_attribute(HTML::AttributeNames::value);
        if (!label.has_value()) {
            if (type_state() == TypeAttributeState::ResetButton) {
                // https://html.spec.whatwg.org/multipage/input.html#reset-button-state-(type=reset)
                // If the element has a value attribute, the button's label must be the value of that attribute;
                // otherwise, it must be an implementation-defined string that means "Reset" or some such.
                label = "Reset"_string;
            } else if (type_state() == TypeAttributeState::SubmitButton) {
                // https://html.spec.whatwg.org/multipage/input.html#submit-button-state-(type=submit)
                // If the element has a value attribute, the button's label must be the value of that attribute;
                // otherwise, it must be an implementation-defined string that means "Submit" or some such.
                label = "Submit"_string;
            } else {
                // https://html.spec.whatwg.org/multipage/input.html#button-state-(type=button)
                // If the element has a value attribute, the button's label must be the value of that attribute;
                // otherwise, it must be the empty string.
                label = value();
            }
        }

        m_text_node->set_data(label.value());
        update_placeholder_visibility();
    }
}

void HTMLInputElement::update_text_input_shadow_tree()
{
    if (m_text_node) {
        m_text_node->set_data(m_value);
        update_placeholder_visibility();
    }
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:attr-input-readonly-3
static bool is_allowed_to_be_readonly(HTML::HTMLInputElement::TypeAttributeState state)
{
    switch (state) {
    case HTML::HTMLInputElement::TypeAttributeState::Text:
    case HTML::HTMLInputElement::TypeAttributeState::Search:
    case HTML::HTMLInputElement::TypeAttributeState::Telephone:
    case HTML::HTMLInputElement::TypeAttributeState::URL:
    case HTML::HTMLInputElement::TypeAttributeState::Email:
    case HTML::HTMLInputElement::TypeAttributeState::Password:
    case HTML::HTMLInputElement::TypeAttributeState::Date:
    case HTML::HTMLInputElement::TypeAttributeState::Month:
    case HTML::HTMLInputElement::TypeAttributeState::Week:
    case HTML::HTMLInputElement::TypeAttributeState::Time:
    case HTML::HTMLInputElement::TypeAttributeState::LocalDateAndTime:
    case HTML::HTMLInputElement::TypeAttributeState::Number:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:attr-input-maxlength-3
static bool is_applicable_for_maxlength_attribute(HTML::HTMLInputElement::TypeAttributeState state)
{
    switch (state) {
    case HTML::HTMLInputElement::TypeAttributeState::Text:
    case HTML::HTMLInputElement::TypeAttributeState::Search:
    case HTML::HTMLInputElement::TypeAttributeState::Telephone:
    case HTML::HTMLInputElement::TypeAttributeState::URL:
    case HTML::HTMLInputElement::TypeAttributeState::Email:
    case HTML::HTMLInputElement::TypeAttributeState::Password:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-maxlength
void HTMLInputElement::handle_maxlength_attribute()
{
    // The maxlength attribute, when it applies, is a form control maxlength attribute.
    if (m_text_node && is_applicable_for_maxlength_attribute(type_state())) {
        auto max_length = this->max_length();
        if (max_length >= 0) {
            m_text_node->set_max_length(max_length);
        } else {
            m_text_node->set_max_length({});
        }
    }
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-readonly
void HTMLInputElement::handle_readonly_attribute(Optional<String> const& maybe_value)
{
    // The readonly attribute is a boolean attribute that controls whether or not the user can edit the form control. When specified, the element is not mutable.
    set_is_mutable(!maybe_value.has_value() || !is_allowed_to_be_readonly(m_type));
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:attr-input-placeholder-3
static bool is_allowed_to_have_placeholder(HTML::HTMLInputElement::TypeAttributeState state)
{
    switch (state) {
    case HTML::HTMLInputElement::TypeAttributeState::Text:
    case HTML::HTMLInputElement::TypeAttributeState::Search:
    case HTML::HTMLInputElement::TypeAttributeState::URL:
    case HTML::HTMLInputElement::TypeAttributeState::Telephone:
    case HTML::HTMLInputElement::TypeAttributeState::Email:
    case HTML::HTMLInputElement::TypeAttributeState::Password:
    case HTML::HTMLInputElement::TypeAttributeState::Number:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-placeholder
String HTMLInputElement::placeholder() const
{
    auto maybe_placeholder = get_attribute(HTML::AttributeNames::placeholder);
    if (!maybe_placeholder.has_value())
        return String {};
    auto placeholder = *maybe_placeholder;

    // The attribute, if specified, must have a value that contains no U+000A LINE FEED (LF) or U+000D CARRIAGE RETURN (CR) characters.
    StringBuilder builder;
    for (auto c : placeholder.bytes_as_string_view()) {
        if (c != '\r' && c != '\n')
            builder.append(c);
    }
    return MUST(builder.to_string());
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-placeholder
Optional<String> HTMLInputElement::placeholder_value() const
{
    if (!m_text_node || !m_text_node->data().is_empty())
        return {};
    if (!is_allowed_to_have_placeholder(type_state()))
        return {};
    if (!has_attribute(HTML::AttributeNames::placeholder))
        return {};
    return placeholder();
}

void HTMLInputElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return;

    switch (type_state()) {
    case TypeAttributeState::Hidden:
    case TypeAttributeState::RadioButton:
    case TypeAttributeState::Checkbox:
        break;
    case TypeAttributeState::Button:
    case TypeAttributeState::SubmitButton:
    case TypeAttributeState::ResetButton:
        create_button_input_shadow_tree();
        break;
    case TypeAttributeState::ImageButton:
        break;
    case TypeAttributeState::Color:
        create_color_input_shadow_tree();
        break;
    case TypeAttributeState::FileUpload:
        create_file_input_shadow_tree();
        break;
    case TypeAttributeState::Range:
        create_range_input_shadow_tree();
        break;
    // FIXME: This could be better factored. Everything except the above types becomes a text input.
    default:
        create_text_input_shadow_tree();
        break;
    }
}

void HTMLInputElement::update_shadow_tree()
{
    switch (type_state()) {
    case TypeAttributeState::Color:
        update_color_well_element();
        break;
    case TypeAttributeState::FileUpload:
        update_file_input_shadow_tree();
        break;
    case TypeAttributeState::Range:
        update_slider_shadow_tree_elements();
        break;
    case TypeAttributeState::Button:
    case TypeAttributeState::ResetButton:
    case TypeAttributeState::SubmitButton:
        update_button_input_shadow_tree();
        break;
    default:
        update_text_input_shadow_tree();
        break;
    }
}

void HTMLInputElement::create_button_input_shadow_tree()
{
    auto shadow_root = realm().create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);
    set_shadow_root(shadow_root);
    auto text_container = MUST(DOM::create_element(document(), HTML::TagNames::span, Namespace::HTML));
    MUST(text_container->set_attribute(HTML::AttributeNames::style, "display: inline-block; pointer-events: none;"_string));
    Optional<String> label = get_attribute(HTML::AttributeNames::value);
    if (!label.has_value()) {
        if (type_state() == TypeAttributeState::ResetButton) {
            // https://html.spec.whatwg.org/multipage/input.html#reset-button-state-(type=reset)
            // If the element has a value attribute, the button's label must be the value of that attribute;
            // otherwise, it must be an implementation-defined string that means "Reset" or some such.
            label = "Reset"_string;
        } else if (type_state() == TypeAttributeState::SubmitButton) {
            // https://html.spec.whatwg.org/multipage/input.html#submit-button-state-(type=submit)
            // If the element has a value attribute, the button's label must be the value of that attribute;
            // otherwise, it must be an implementation-defined string that means "Submit" or some such.
            label = "Submit"_string;
        } else {
            // https://html.spec.whatwg.org/multipage/input.html#button-state-(type=button)
            // If the element has a value attribute, the button's label must be the value of that attribute;
            // otherwise, it must be the empty string.
            label = value();
        }
    }
    m_text_node = realm().create<DOM::Text>(document(), label.value());
    MUST(text_container->append_child(*m_text_node));
    MUST(shadow_root->append_child(*text_container));
}

void HTMLInputElement::create_text_input_shadow_tree()
{
    auto shadow_root = realm().create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);
    set_shadow_root(shadow_root);

    auto initial_value = m_value;
    auto element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    {
        static GC::Root<CSS::CSSStyleProperties> style;
        if (!style) {
            style = CSS::CSSStyleProperties::create(internal_css_realm(), {}, {});
            style->set_declarations_from_text(R"~~~(
                display: flex;
                height: 100%;
                align-items: center;
                white-space: pre;
                border: none;
                padding: 1px 2px;
            )~~~"sv);
        }
        element->set_inline_style(*style);
    }
    MUST(shadow_root->append_child(element));

    m_placeholder_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    m_placeholder_element->set_use_pseudo_element(CSS::PseudoElement::Placeholder);
    update_placeholder_visibility();

    MUST(element->append_child(*m_placeholder_element));

    m_placeholder_text_node = realm().create<DOM::Text>(document(), String {});
    m_placeholder_text_node->set_data(placeholder());
    MUST(m_placeholder_element->append_child(*m_placeholder_text_node));

    // https://www.w3.org/TR/css-ui-4/#input-rules
    m_inner_text_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    {
        static GC::Root<CSS::CSSStyleProperties> style;
        if (!style) {
            style = CSS::CSSStyleProperties::create(internal_css_realm(), {}, {});
            style->set_declarations_from_text(R"~~~(
                width: 100%;
                height: 1lh;
                align-items: center;
                text-overflow: clip;
                white-space: nowrap;
            )~~~"sv);
        }
        m_inner_text_element->set_inline_style(*style);
    }
    MUST(element->append_child(*m_inner_text_element));

    m_text_node = realm().create<DOM::Text>(document(), move(initial_value));
    handle_readonly_attribute(attribute(HTML::AttributeNames::readonly));
    if (type_state() == TypeAttributeState::Password)
        m_text_node->set_is_password_input({}, true);
    handle_maxlength_attribute();
    MUST(m_inner_text_element->append_child(*m_text_node));

    update_placeholder_visibility();

    if (type_state() == TypeAttributeState::Number) {
        // Up button
        auto up_button = MUST(DOM::create_element(document(), HTML::TagNames::button, Namespace::HTML));
        // FIXME: This cursor property doesn't work
        MUST(up_button->set_attribute(HTML::AttributeNames::style, R"~~~(
            padding: 0;
            cursor: default;
        )~~~"_string));
        MUST(up_button->set_inner_html("<svg style=\"width: 1em; height: 1em;\" xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><path fill=\"currentColor\" d=\"M7.41,15.41L12,10.83L16.59,15.41L18,14L12,8L6,14L7.41,15.41Z\" /></svg>"sv));
        MUST(element->append_child(up_button));

        auto mouseup_callback_function = JS::NativeFunction::create(
            realm(), [this](JS::VM&) {
                commit_pending_changes();
                return JS::js_undefined();
            },
            0, FlyString {}, &realm());
        auto mouseup_callback = realm().heap().allocate<WebIDL::CallbackType>(*mouseup_callback_function, realm());
        DOM::AddEventListenerOptions mouseup_listener_options;
        mouseup_listener_options.once = true;

        auto up_callback_function = JS::NativeFunction::create(
            realm(), [this](JS::VM&) {
                if (is_mutable()) {
                    MUST(step_up());
                    user_interaction_did_change_input_value();
                }
                return JS::js_undefined();
            },
            0, FlyString {}, &realm());
        auto step_up_callback = realm().heap().allocate<WebIDL::CallbackType>(*up_callback_function, realm());
        up_button->add_event_listener_without_options(UIEvents::EventNames::mousedown, DOM::IDLEventListener::create(realm(), step_up_callback));
        up_button->add_event_listener_without_options(UIEvents::EventNames::mouseup, DOM::IDLEventListener::create(realm(), mouseup_callback));

        // Down button
        auto down_button = MUST(DOM::create_element(document(), HTML::TagNames::button, Namespace::HTML));
        MUST(down_button->set_attribute(HTML::AttributeNames::style, R"~~~(
            padding: 0;
            cursor: default;
        )~~~"_string));
        MUST(down_button->set_inner_html("<svg style=\"width: 1em; height: 1em;\" xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><path fill=\"currentColor\" d=\"M7.41,8.58L12,13.17L16.59,8.58L18,10L12,16L6,10L7.41,8.58Z\" /></svg>"sv));
        MUST(element->append_child(down_button));

        auto down_callback_function = JS::NativeFunction::create(
            realm(), [this](JS::VM&) {
                if (is_mutable()) {
                    MUST(step_down());
                    user_interaction_did_change_input_value();
                }
                return JS::js_undefined();
            },
            0, FlyString {}, &realm());
        auto step_down_callback = realm().heap().allocate<WebIDL::CallbackType>(*down_callback_function, realm());
        down_button->add_event_listener_without_options(UIEvents::EventNames::mousedown, DOM::IDLEventListener::create(realm(), step_down_callback));
        down_button->add_event_listener_without_options(UIEvents::EventNames::mouseup, DOM::IDLEventListener::create(realm(), mouseup_callback));
    }
}

void HTMLInputElement::create_color_input_shadow_tree()
{
    auto shadow_root = realm().create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);

    auto color = value_sanitization_algorithm(m_value);

    auto border = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(border->set_attribute(HTML::AttributeNames::style, R"~~~(
        width: fit-content;
        height: fit-content;
        padding: 4px;
        border: 1px solid ButtonBorder;
        background-color: ButtonFace;
)~~~"_string));

    m_color_well_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(m_color_well_element->set_attribute(HTML::AttributeNames::style, R"~~~(
        width: 32px;
        height: 16px;
        border: 1px solid ButtonBorder;
        box-sizing: border-box;
)~~~"_string));
    MUST(m_color_well_element->style_for_bindings()->set_property(CSS::PropertyID::BackgroundColor, color));

    MUST(border->append_child(*m_color_well_element));
    MUST(shadow_root->append_child(border));
    set_shadow_root(shadow_root);
}

void HTMLInputElement::update_color_well_element()
{
    if (!m_color_well_element)
        return;

    MUST(m_color_well_element->style_for_bindings()->set_property(CSS::PropertyID::BackgroundColor, m_value));
}

void HTMLInputElement::create_file_input_shadow_tree()
{
    auto& realm = this->realm();

    auto shadow_root = realm.create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);

    m_file_button = DOM::create_element(document(), HTML::TagNames::button, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    m_file_button->set_use_pseudo_element(CSS::PseudoElement::FileSelectorButton);

    m_file_label = DOM::create_element(document(), HTML::TagNames::label, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(m_file_label->set_attribute(HTML::AttributeNames::style, "padding-left: 4px;"_string));

    auto on_button_click = [this](JS::VM&) {
        show_the_picker_if_applicable(*this);
        return JS::js_undefined();
    };

    auto on_button_click_function = JS::NativeFunction::create(realm, move(on_button_click), 0, FlyString {}, &realm);
    auto on_button_click_callback = realm.heap().allocate<WebIDL::CallbackType>(on_button_click_function, realm);
    m_file_button->add_event_listener_without_options(UIEvents::EventNames::click, DOM::IDLEventListener::create(realm, on_button_click_callback));

    update_file_input_shadow_tree();

    MUST(shadow_root->append_child(*m_file_button));
    MUST(shadow_root->append_child(*m_file_label));

    set_shadow_root(shadow_root);
}

void HTMLInputElement::update_file_input_shadow_tree()
{
    if (!m_file_button || !m_file_label)
        return;

    auto files_label = has_attribute(HTML::AttributeNames::multiple) ? "files"sv : "file"sv;
    m_file_button->set_text_content(MUST(String::formatted("Select {}...", files_label)));

    if (m_selected_files && m_selected_files->length() > 0) {
        if (m_selected_files->length() == 1)
            m_file_label->set_text_content(m_selected_files->item(0)->name());
        else
            m_file_label->set_text_content(MUST(String::formatted("{} files selected.", m_selected_files->length())));
    } else {
        m_file_label->set_text_content(MUST(String::formatted("No {} selected.", files_label)));
    }
}

void HTMLInputElement::create_range_input_shadow_tree()
{
    auto shadow_root = realm().create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);
    set_shadow_root(shadow_root);

    m_slider_runnable_track = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    m_slider_runnable_track->set_use_pseudo_element(CSS::PseudoElement::Track);
    MUST(shadow_root->append_child(*m_slider_runnable_track));

    m_slider_progress_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    m_slider_progress_element->set_use_pseudo_element(CSS::PseudoElement::Fill);
    MUST(m_slider_runnable_track->append_child(*m_slider_progress_element));

    m_slider_thumb = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    m_slider_thumb->set_use_pseudo_element(CSS::PseudoElement::Thumb);
    MUST(m_slider_runnable_track->append_child(*m_slider_thumb));

    update_slider_shadow_tree_elements();

    auto keydown_callback_function = JS::NativeFunction::create(
        realm(), [this](JS::VM& vm) {
            auto key = MUST(vm.argument(0).get(vm, "key"_fly_string)).as_string().utf8_string();

            if (key == "ArrowLeft" || key == "ArrowDown")
                MUST(step_down());
            if (key == "PageDown")
                MUST(step_down(10));

            if (key == "ArrowRight" || key == "ArrowUp")
                MUST(step_up());
            if (key == "PageUp")
                MUST(step_up(10));

            user_interaction_did_change_input_value();
            return JS::js_undefined();
        },
        0, ""_fly_string, &realm());
    auto keydown_callback = realm().heap().allocate<WebIDL::CallbackType>(*keydown_callback_function, realm());
    add_event_listener_without_options(UIEvents::EventNames::keydown, DOM::IDLEventListener::create(realm(), keydown_callback));

    auto wheel_callback_function = JS::NativeFunction::create(
        realm(), [this](JS::VM& vm) {
            auto delta_y = MUST(vm.argument(0).get(vm, "deltaY"_fly_string)).as_i32();
            if (delta_y > 0) {
                MUST(step_down());
            } else {
                MUST(step_up());
            }
            user_interaction_did_change_input_value();
            return JS::js_undefined();
        },
        0, ""_fly_string, &realm());
    auto wheel_callback = realm().heap().allocate<WebIDL::CallbackType>(*wheel_callback_function, realm());
    add_event_listener_without_options(UIEvents::EventNames::wheel, DOM::IDLEventListener::create(realm(), wheel_callback));

    auto update_slider_by_mouse = [this](JS::VM& vm) {
        auto client_x = MUST(vm.argument(0).get(vm, "clientX"_fly_string)).as_double();
        auto rect = get_bounding_client_rect();
        double minimum = *min();
        double maximum = *max();
        // FIXME: Snap new value to input steps
        MUST(set_value_as_number(clamp(round(((client_x - rect.left().to_double()) / rect.width().to_double()) * (maximum - minimum) + minimum), minimum, maximum)));
        user_interaction_did_change_input_value();
    };

    auto mousedown_callback_function = JS::NativeFunction::create(
        realm(), [this, update_slider_by_mouse](JS::VM& vm) {
            update_slider_by_mouse(vm);

            auto mousemove_callback_function = JS::NativeFunction::create(
                realm(), [update_slider_by_mouse](JS::VM& vm) {
                    update_slider_by_mouse(vm);
                    return JS::js_undefined();
                },
                0, ""_fly_string, &realm());
            auto mousemove_callback = realm().heap().allocate<WebIDL::CallbackType>(*mousemove_callback_function, realm());
            auto mousemove_listener = DOM::IDLEventListener::create(realm(), mousemove_callback);
            auto& window = static_cast<HTML::Window&>(relevant_global_object(*this));
            window.add_event_listener_without_options(UIEvents::EventNames::mousemove, mousemove_listener);

            auto mouseup_callback_function = JS::NativeFunction::create(
                realm(), [this, mousemove_listener](JS::VM&) {
                    auto& window = static_cast<HTML::Window&>(relevant_global_object(*this));
                    window.remove_event_listener_without_options(UIEvents::EventNames::mousemove, mousemove_listener);
                    return JS::js_undefined();
                },
                0, ""_fly_string, &realm());
            auto mouseup_callback = realm().heap().allocate<WebIDL::CallbackType>(*mouseup_callback_function, realm());
            DOM::AddEventListenerOptions mouseup_listener_options;
            mouseup_listener_options.once = true;
            window.add_event_listener(UIEvents::EventNames::mouseup, DOM::IDLEventListener::create(realm(), mouseup_callback), mouseup_listener_options);

            return JS::js_undefined();
        },
        0, ""_fly_string, &realm());
    auto mousedown_callback = realm().heap().allocate<WebIDL::CallbackType>(*mousedown_callback_function, realm());
    add_event_listener_without_options(UIEvents::EventNames::mousedown, DOM::IDLEventListener::create(realm(), mousedown_callback));
}

void HTMLInputElement::user_interaction_did_change_input_value()
{
    // https://html.spec.whatwg.org/multipage/input.html#common-input-element-events
    // For input elements without a defined input activation behavior, but to which these events apply,
    // and for which the user interface involves both interactive manipulation and an explicit commit action,
    // then when the user changes the element's value, the user agent must queue an element task on the user interaction task source
    // given the input element to fire an event named input at the input element, with the bubbles and composed attributes initialized to true
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
        auto input_event = DOM::Event::create(realm(), HTML::EventNames::input);
        input_event->set_bubbles(true);
        input_event->set_composed(true);
        dispatch_event(*input_event);
    });
    // and any time the user commits the change, the user agent must queue an element task on the user interaction task source given the input
    // element to set its user validity to true and fire an event named change at the input element, with the bubbles attribute initialized to true.
    // FIXME: Does this need to happen here?
}

void HTMLInputElement::update_slider_shadow_tree_elements()
{
    double value = convert_string_to_number(value_sanitization_algorithm(m_value)).value_or(0);
    double minimum = *min();
    double maximum = *max();
    double position = (value - minimum) / (maximum - minimum) * 100;

    if (m_slider_progress_element)
        MUST(m_slider_progress_element->style_for_bindings()->set_property(CSS::PropertyID::Width, MUST(String::formatted("{}%", position))));

    if (m_slider_thumb)
        MUST(m_slider_thumb->style_for_bindings()->set_property(CSS::PropertyID::MarginLeft, MUST(String::formatted("{}%", position))));
}

void HTMLInputElement::did_receive_focus()
{
    if (!m_text_node)
        return;
    m_text_node->invalidate_style(DOM::StyleInvalidationReason::DidReceiveFocus);

    if (m_placeholder_text_node)
        m_placeholder_text_node->invalidate_style(DOM::StyleInvalidationReason::DidReceiveFocus);
}

void HTMLInputElement::did_lose_focus()
{
    if (m_text_node) {
        m_text_node->invalidate_style(DOM::StyleInvalidationReason::DidLoseFocus);
    }

    if (m_placeholder_text_node)
        m_placeholder_text_node->invalidate_style(DOM::StyleInvalidationReason::DidLoseFocus);

    commit_pending_changes();
}

void HTMLInputElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    PopoverInvokerElement::associated_attribute_changed(name, value, namespace_);

    if (name == HTML::AttributeNames::checked) {
        // https://html.spec.whatwg.org/multipage/input.html#the-input-element:concept-input-checked-dirty-2
        // When the checked content attribute is added, if the control does not have dirty checkedness, the user agent must set the checkedness of the element to true;
        // when the checked content attribute is removed, if the control does not have dirty checkedness, the user agent must set the checkedness of the element to false.
        if (!m_dirty_checkedness) {
            set_checked(value.has_value());
            // set_checked() sets the dirty checkedness flag. We reset it here sinceit shouldn't be set when updating the attribute value
            m_dirty_checkedness = false;
        }
    } else if (name == HTML::AttributeNames::type) {
        auto new_type_attribute_state = parse_type_attribute(value.value_or(String {}));
        type_attribute_changed(m_type, new_type_attribute_state);

        // https://html.spec.whatwg.org/multipage/input.html#image-button-state-(type=image):the-input-element-4
        // the input element's type attribute is changed back to the Image Button state, and the src attribute is present,
        // and its value has changed since the last time the type attribute was in the Image Button state
        if (type_state() == TypeAttributeState::ImageButton) {
            if (auto src = attribute(AttributeNames::src); src.has_value() && src != m_last_src_value)
                handle_src_attribute(*src).release_value_but_fixme_should_propagate_errors();
        }

    } else if (name == HTML::AttributeNames::value) {
        if (!m_dirty_value) {
            auto old_value = move(m_value);
            if (!value.has_value()) {
                m_value = String {};
            } else {
                m_value = value_sanitization_algorithm(*value);
            }

            if (m_value != old_value)
                relevant_value_was_changed();

            update_shadow_tree();
        }
    } else if (name == HTML::AttributeNames::placeholder) {
        if (m_placeholder_text_node) {
            m_placeholder_text_node->set_data(placeholder());
            update_placeholder_visibility();
        }
    } else if (name == HTML::AttributeNames::readonly) {
        handle_readonly_attribute(value);
    } else if (name == HTML::AttributeNames::src) {
        handle_src_attribute(value.value_or({})).release_value_but_fixme_should_propagate_errors();
    } else if (name == HTML::AttributeNames::alt) {
        if (layout_node() && type_state() == TypeAttributeState::ImageButton)
            did_update_alt_text(as<Layout::ImageBox>(*layout_node()));
    } else if (name == HTML::AttributeNames::maxlength) {
        handle_maxlength_attribute();
    } else if (name == HTML::AttributeNames::multiple) {
        update_shadow_tree();
    }
}

// https://html.spec.whatwg.org/multipage/input.html#input-type-change
void HTMLInputElement::type_attribute_changed(TypeAttributeState old_state, TypeAttributeState new_state)
{
    auto new_value_attribute_mode = value_attribute_mode_for_type_state(new_state);
    auto old_value_attribute_mode = value_attribute_mode_for_type_state(old_state);

    // 1. If the previous state of the element's type attribute put the value IDL attribute in the value mode, and the element's
    //    value is not the empty string, and the new state of the element's type attribute puts the value IDL attribute in either
    //    the default mode or the default/on mode, then set the element's value content attribute to the element's value.
    if (old_value_attribute_mode == ValueAttributeMode::Value && !m_value.is_empty() && (first_is_one_of(new_value_attribute_mode, ValueAttributeMode::Default, ValueAttributeMode::DefaultOn))) {
        MUST(set_attribute(HTML::AttributeNames::value, m_value));
    }

    // 2. Otherwise, if the previous state of the element's type attribute put the value IDL attribute in any mode other
    //    than the value mode, and the new state of the element's type attribute puts the value IDL attribute in the value mode,
    //    then set the value of the element to the value of the value content attribute, if there is one, or the empty string
    //    otherwise, and then set the control's dirty value flag to false.
    else if (old_value_attribute_mode != ValueAttributeMode::Value && new_value_attribute_mode == ValueAttributeMode::Value) {
        m_value = attribute(HTML::AttributeNames::value).value_or({});
        m_dirty_value = false;
    }

    // 3. Otherwise, if the previous state of the element's type attribute put the value IDL attribute in any mode other
    //    than the filename mode, and the new state of the element's type attribute puts the value IDL attribute in the filename mode,
    //    then set the value of the element to the empty string.
    else if (old_value_attribute_mode != ValueAttributeMode::Filename && new_value_attribute_mode == ValueAttributeMode::Filename) {
        m_value = String {};
    }

    // 4. Update the element's rendering and behavior to the new state's.
    m_type = new_state;
    set_shadow_root(nullptr);
    create_shadow_tree_if_needed();

    // FIXME: 5. Signal a type change for the element. (The Radio Button state uses this, in particular.)

    // 6. Invoke the value sanitization algorithm, if one is defined for the type attribute's new state.
    m_value = value_sanitization_algorithm(m_value);

    // 7. Let previouslySelectable be true if setRangeText() previously applied to the element, and false otherwise.
    auto previously_selectable = selection_or_range_applies_for_type_state(old_state);

    // 8. Let nowSelectable be true if setRangeText() now applies to the element, and false otherwise.
    auto now_selectable = selection_or_range_applies_for_type_state(new_state);

    // 9. If previouslySelectable is false and nowSelectable is true, set the element's text entry cursor position to the
    //    beginning of the text control, and set its selection direction to "none".
    if (!previously_selectable && now_selectable) {
        set_selection_direction(OptionalNone {});
    }
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-src
WebIDL::ExceptionOr<void> HTMLInputElement::handle_src_attribute(String const& value)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    if (type_state() != TypeAttributeState::ImageButton)
        return {};

    m_last_src_value = value;

    // 1. Let url be the result of encoding-parsing a URL given the src attribute's value, relative to the element's
    //    node document.
    auto url = document().encoding_parse_url(value);

    // 2. If url is failure, then return.
    if (!url.has_value())
        return {};

    // 3. Let request be a new request whose URL is url, client is the element's node document's relevant settings
    //    object, destination is "image", initiator type is "input", credentials mode is "include", and whose
    //    use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(url.release_value());
    request->set_client(&document().relevant_settings_object());
    request->set_destination(Fetch::Infrastructure::Request::Destination::Image);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Input);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
    request->set_use_url_credentials(true);

    // 4. Fetch request, with processResponseEndOfBody set to the following steps given response response:
    m_resource_request = SharedResourceRequest::get_or_create(realm, document().page(), request->url());
    m_resource_request->add_callbacks(
        [this, &realm]() {
            // 1. If the download was successful and the image is available, queue an element task on the user interaction
            //    task source given the input element to fire an event named load at the input element.
            queue_an_element_task(HTML::Task::Source::UserInteraction, [this, &realm]() {
                dispatch_event(DOM::Event::create(realm, HTML::EventNames::load));
            });

            m_load_event_delayer.clear();
            set_needs_layout_tree_update(true);
        },
        [this, &realm]() {
            // 2. Otherwise, if the fetching process fails without a response from the remote server, or completes but the
            //    image is not a valid or supported image, then queue an element task on the user interaction task source
            //    given the input element to fire an event named error on the input element.
            queue_an_element_task(HTML::Task::Source::UserInteraction, [this, &realm]() {
                dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));
            });

            m_load_event_delayer.clear();
        });

    if (m_resource_request->needs_fetching()) {
        m_resource_request->fetch_resource(realm, request);
    }

    // Fetching the image must delay the load event of the element's node document until the task that is queued by the
    // networking task source once the resource has been fetched (defined below) has been run.
    m_load_event_delayer.emplace(document());

    return {};
}

HTMLInputElement::TypeAttributeState HTMLInputElement::parse_type_attribute(StringView type)
{
#define __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(keyword, state) \
    if (type.equals_ignoring_ascii_case(keyword##sv))         \
        return HTMLInputElement::TypeAttributeState::state;
    ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTES
#undef __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE

    // The missing value default and the invalid value default are the Text state.
    // https://html.spec.whatwg.org/multipage/input.html#the-input-element:missing-value-default
    // https://html.spec.whatwg.org/multipage/input.html#the-input-element:invalid-value-default
    return HTMLInputElement::TypeAttributeState::Text;
}

StringView HTMLInputElement::type() const
{
    // FIXME: This should probably be `Reflect` in the IDL.
    switch (m_type) {
#define __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(keyword, state) \
    case TypeAttributeState::state:                           \
        return keyword##sv;
        ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTES
#undef __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE
    }

    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> HTMLInputElement::set_type(String const& type)
{
    return set_attribute(HTML::AttributeNames::type, type);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-simple-colour
static bool is_valid_simple_color(StringView value)
{
    // if it is exactly seven characters long,
    if (value.length() != 7)
        return false;
    // and the first character is a U+0023 NUMBER SIGN character (#),
    if (!value.starts_with('#'))
        return false;
    // and the remaining six characters are all ASCII hex digits
    for (size_t i = 1; i < value.length(); i++)
        if (!is_ascii_hex_digit(value[i]))
            return false;

    return true;
}

// https://html.spec.whatwg.org/multipage/input.html#value-sanitization-algorithm
String HTMLInputElement::value_sanitization_algorithm(String const& value) const
{
    if (type_state() == HTMLInputElement::TypeAttributeState::Text || type_state() == HTMLInputElement::TypeAttributeState::Search || type_state() == HTMLInputElement::TypeAttributeState::Telephone || type_state() == HTMLInputElement::TypeAttributeState::Password) {
        // Strip newlines from the value.
        if (value.bytes_as_string_view().contains('\r') || value.bytes_as_string_view().contains('\n')) {
            StringBuilder builder;
            for (auto c : value.bytes_as_string_view()) {
                if (c != '\r' && c != '\n')
                    builder.append(c);
            }
            return MUST(builder.to_string());
        }
    } else if (type_state() == HTMLInputElement::TypeAttributeState::URL) {
        // Strip newlines from the value, then strip leading and trailing ASCII whitespace from the value.
        if (value.bytes_as_string_view().contains('\r') || value.bytes_as_string_view().contains('\n')) {
            StringBuilder builder;
            for (auto c : value.bytes_as_string_view()) {
                if (c != '\r' && c != '\n')
                    builder.append(c);
            }
            return MUST(String::from_utf8(builder.string_view().trim(Infra::ASCII_WHITESPACE)));
        }
        return MUST(value.trim(Infra::ASCII_WHITESPACE));
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Email) {
        // https://html.spec.whatwg.org/multipage/input.html#email-state-(type=email):value-sanitization-algorithm
        // FIXME: handle the `multiple` attribute
        // Strip newlines from the value, then strip leading and trailing ASCII whitespace from the value.
        if (value.bytes_as_string_view().contains('\r') || value.bytes_as_string_view().contains('\n')) {
            StringBuilder builder;
            for (auto c : value.bytes_as_string_view()) {
                if (c != '\r' && c != '\n')
                    builder.append(c);
            }
            return MUST(String::from_utf8(builder.string_view().trim(Infra::ASCII_WHITESPACE)));
        }
        return MUST(value.trim(Infra::ASCII_WHITESPACE));
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Number) {
        // https://html.spec.whatwg.org/multipage/input.html#number-state-(type=number):value-sanitization-algorithm
        // If the value of the element is not a valid floating-point number, then set it
        // to the empty string instead.
        if (!is_valid_floating_point_number(value))
            return String {};
        auto maybe_value = parse_floating_point_number(value);
        // AD-HOC: The spec doesn’t require these checks — but other engines do them, and
        // there’s a WPT case which tests that the value is less than Number.MAX_VALUE.
        if (!maybe_value.has_value() || !isfinite(maybe_value.value()))
            return String {};
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Date) {
        // https://html.spec.whatwg.org/multipage/input.html#date-state-(type=date):value-sanitization-algorithm
        if (!is_valid_date_string(value))
            return String {};
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Month) {
        // https://html.spec.whatwg.org/multipage/input.html#month-state-(type=month):value-sanitization-algorithm
        if (!is_valid_month_string(value))
            return String {};
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Week) {
        // https://html.spec.whatwg.org/multipage/input.html#week-state-(type=week):value-sanitization-algorithm
        if (!is_valid_week_string(value))
            return String {};
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Time) {
        // https://html.spec.whatwg.org/multipage/input.html#time-state-(type=time):value-sanitization-algorithm
        if (!is_valid_time_string(value))
            return String {};
    } else if (type_state() == HTMLInputElement::TypeAttributeState::LocalDateAndTime) {
        // https://html.spec.whatwg.org/multipage/input.html#local-date-and-time-state-(type=datetime-local):value-sanitization-algorithm
        if (is_valid_local_date_and_time_string(value))
            return normalize_local_date_and_time_string(value);
        return String {};
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Range) {
        // https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range):value-sanitization-algorithm
        // If the value of the element is not a valid floating-point number, then set it to the best representation, as a floating-point number, of the default value.
        auto maybe_value = parse_floating_point_number(value);
        if (!is_valid_floating_point_number(value) ||
            // AD-HOC: The spec doesn’t require these checks — but other engines do them.
            !maybe_value.has_value() || !isfinite(maybe_value.value())) {
            // The default value is the minimum plus half the difference between the minimum and the maximum, unless the maximum is less than the minimum, in which case the default value is the minimum.
            auto minimum = *min();
            auto maximum = *max();
            if (maximum < minimum)
                return JS::number_to_string(minimum);
            return JS::number_to_string(minimum + (maximum - minimum) / 2);
        }
    } else if (type_state() == HTMLInputElement::TypeAttributeState::Color) {
        // https://html.spec.whatwg.org/multipage/input.html#color-state-(type=color):value-sanitization-algorithm
        // If the value of the element is a valid simple color, then set it to the value of the element converted to ASCII lowercase;
        if (is_valid_simple_color(value))
            return value.to_ascii_lowercase();
        // otherwise, set it to the string "#000000".
        return "#000000"_string;
    }
    return value;
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:concept-form-reset-control
void HTMLInputElement::reset_algorithm()
{
    // The reset algorithm for input elements is to set its user validity, dirty value flag, and dirty checkedness flag back to false,
    m_user_validity = false;
    m_dirty_value = false;
    m_dirty_checkedness = false;

    // set the value of the element to the value of the value content attribute, if there is one, or the empty string otherwise,
    auto old_value = move(m_value);
    m_value = get_attribute_value(AttributeNames::value);

    // set the checkedness of the element to true if the element has a checked content attribute and false if it does not,
    m_checked = has_attribute(AttributeNames::checked);

    // empty the list of selected files,
    if (m_selected_files)
        m_selected_files = FileAPI::FileList::create(realm());

    // and then invoke the value sanitization algorithm, if the type attribute's current state defines one.
    m_value = value_sanitization_algorithm(m_value);

    if (m_value != old_value)
        relevant_value_was_changed();

    if (m_text_node) {
        m_text_node->set_data(m_value);
        update_placeholder_visibility();
    }

    update_shadow_tree();
}

// https://w3c.github.io/webdriver/#dfn-clear-algorithm
void HTMLInputElement::clear_algorithm()
{
    // The clear algorithm for input elements is to set the dirty value flag and dirty checkedness flag back to false,
    m_dirty_value = false;
    m_dirty_checkedness = false;

    // set the value of the element to an empty string,
    auto old_value = move(m_value);
    m_value = String {};

    // set the checkedness of the element to true if the element has a checked content attribute and false if it does not,
    m_checked = has_attribute(AttributeNames::checked);

    // empty the list of selected files,
    if (m_selected_files)
        m_selected_files = FileAPI::FileList::create(realm());

    // and then invoke the value sanitization algorithm iff the type attribute's current state defines one.
    m_value = value_sanitization_algorithm(m_value);

    // Unlike their associated reset algorithms, changes made to form controls as part of these algorithms do count as
    // changes caused by the user (and thus, e.g. do cause input events to fire).
    user_interaction_did_change_input_value();

    if (m_value != old_value)
        relevant_value_was_changed();

    if (m_text_node) {
        m_text_node->set_data(m_value);
        update_placeholder_visibility();
    }

    update_shadow_tree();
}

void HTMLInputElement::form_associated_element_was_inserted()
{
    create_shadow_tree_if_needed();

    if (is_connected()) {
        // https://html.spec.whatwg.org/multipage/input.html#radio-button-state-(type=radio)
        // When any of the following phenomena occur, if the element's checkedness state is true after the occurrence,
        // the checkedness state of all the other elements in the same radio button group must be set to false:
        // ...
        // - The element becomes connected.
        if (type_state() == TypeAttributeState::RadioButton && checked()) {
            root().for_each_in_inclusive_subtree_of_type<HTMLInputElement>([&](auto& element) {
                if (element.checked() && &element != this && is_in_same_radio_button_group(*this, element))
                    element.set_checked(false);
                return TraversalDecision::Continue;
            });
        }
    }
}

void HTMLInputElement::form_associated_element_was_removed(DOM::Node*)
{
    set_shadow_root(nullptr);
}

bool HTMLInputElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    if (type_state() != TypeAttributeState::ImageButton)
        return false;

    return first_is_one_of(name,
        HTML::AttributeNames::align,
        HTML::AttributeNames::border,
        HTML::AttributeNames::height,
        HTML::AttributeNames::hspace,
        HTML::AttributeNames::vspace,
        HTML::AttributeNames::width);
}

void HTMLInputElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    if (type_state() != TypeAttributeState::ImageButton)
        return;

    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::align) {
            if (value.equals_ignoring_ascii_case("center"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Center));
            else if (value.equals_ignoring_ascii_case("middle"sv))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Middle));
        } else if (name == HTML::AttributeNames::border) {
            if (auto parsed_value = parse_non_negative_integer(value); parsed_value.has_value()) {
                auto width_style_value = CSS::LengthStyleValue::create(CSS::Length::make_px(*parsed_value));
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopWidth, width_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightWidth, width_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomWidth, width_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftWidth, width_style_value);

                auto border_style_value = CSS::CSSKeywordValue::create(CSS::Keyword::Solid);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopStyle, border_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightStyle, border_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomStyle, border_style_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftStyle, border_style_value);
            }
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, *parsed_value);
            }
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:maps-to-the-dimension-property
        else if (name == HTML::AttributeNames::hspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginLeft, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginRight, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::vspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginTop, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginBottom, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, *parsed_value);
            }
        }
    });
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element%3Aconcept-node-clone-ext
WebIDL::ExceptionOr<void> HTMLInputElement::cloned(DOM::Node& copy, bool subtree) const
{
    TRY(Base::cloned(copy, subtree));

    // The cloning steps for input elements given node, copy, and subtree are to propagate the value, dirty value flag, checkedness, and dirty checkedness flag from node to copy.
    auto& input_clone = as<HTMLInputElement>(copy);
    input_clone.m_value = m_value;
    input_clone.m_dirty_value = m_dirty_value;
    input_clone.m_checked = m_checked;
    input_clone.m_dirty_checkedness = m_dirty_checkedness;

    // AD-HOC: The spec doesn't mention propagating this state, but there is a WPT test that expects cloned nodes to preserve it.
    input_clone.m_indeterminate = m_indeterminate;

    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#radio-button-group
static bool is_in_same_radio_button_group(HTML::HTMLInputElement const& a, HTML::HTMLInputElement const& b)
{
    auto non_empty_equals = [](auto const& value_a, auto const& value_b) {
        return !value_a.is_empty() && value_a == value_b;
    };
    // The radio button group that contains an input element a also contains all the
    // other input elements b that fulfill all of the following conditions:
    return (
        // - Both a and b are in the same tree.
        &a.root() == &b.root()
        // - The input element b's type attribute is in the Radio Button state.
        && a.type_state() == b.type_state()
        && b.type_state() == HTMLInputElement::TypeAttributeState::RadioButton
        // - Either a and b have the same form owner, or they both have no form owner.
        && a.form() == b.form()
        // - They both have a name attribute, their name attributes are not empty, and the
        // value of a's name attribute equals the value of b's name attribute.
        && a.name().has_value()
        && b.name().has_value()
        && non_empty_equals(a.name().value(), b.name().value()));
}

// https://html.spec.whatwg.org/multipage/input.html#radio-button-state-(type=radio)
void HTMLInputElement::set_checked_within_group()
{
    if (checked())
        return;

    set_checked(true);

    // No point iterating the tree if we have an empty name.
    if (!name().has_value() || name()->is_empty())
        return;

    root().for_each_in_inclusive_subtree_of_type<HTML::HTMLInputElement>([&](auto& element) {
        if (element.checked() && &element != this && is_in_same_radio_button_group(*this, element))
            element.set_checked(false);
        return TraversalDecision::Continue;
    });
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:legacy-pre-activation-behavior
void HTMLInputElement::legacy_pre_activation_behavior()
{
    m_before_legacy_pre_activation_behavior_checked = checked();
    m_before_legacy_pre_activation_behavior_indeterminate = indeterminate();

    // 1. If this element's type attribute is in the Checkbox state, then set
    // this element's checkedness to its opposite value (i.e. true if it is
    // false, false if it is true) and set this element's indeterminate IDL
    // attribute to false.
    if (type_state() == TypeAttributeState::Checkbox) {
        set_checked(!checked());
        set_indeterminate(false);
    }

    // 2. If this element's type attribute is in the Radio Button state, then
    // get a reference to the element in this element's radio button group that
    // has its checkedness set to true, if any, and then set this element's
    // checkedness to true.
    if (type_state() == TypeAttributeState::RadioButton) {
        root().for_each_in_inclusive_subtree_of_type<HTML::HTMLInputElement>([&](auto& element) {
            if (element.checked() && is_in_same_radio_button_group(*this, element)) {
                m_legacy_pre_activation_behavior_checked_element_in_group = &element;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });

        set_checked_within_group();
    }
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:legacy-canceled-activation-behavior
void HTMLInputElement::legacy_cancelled_activation_behavior()
{
    // 1. If the element's type attribute is in the Checkbox state, then set the
    // element's checkedness and the element's indeterminate IDL attribute back
    // to the values they had before the legacy-pre-activation behavior was run.
    if (type_state() == TypeAttributeState::Checkbox) {
        set_checked(m_before_legacy_pre_activation_behavior_checked);
        set_indeterminate(m_before_legacy_pre_activation_behavior_indeterminate);
    }

    // 2. If this element 's type attribute is in the Radio Button state, then
    // if the element to which a reference was obtained in the
    // legacy-pre-activation behavior, if any, is still in what is now this
    // element' s radio button group, if it still has one, and if so, setting
    // that element 's checkedness to true; or else, if there was no such
    // element, or that element is no longer in this element' s radio button
    // group, or if this element no longer has a radio button group, setting
    // this element's checkedness to false.
    if (type_state() == TypeAttributeState::RadioButton) {
        bool did_reselect_previous_element = false;
        if (m_legacy_pre_activation_behavior_checked_element_in_group) {
            auto& element_in_group = *m_legacy_pre_activation_behavior_checked_element_in_group;
            if (is_in_same_radio_button_group(*this, element_in_group)) {
                element_in_group.set_checked_within_group();
                did_reselect_previous_element = true;
            }

            m_legacy_pre_activation_behavior_checked_element_in_group = nullptr;
        }

        if (!did_reselect_previous_element)
            set_checked(false);
    }
}

void HTMLInputElement::legacy_cancelled_activation_behavior_was_not_called()
{
    m_legacy_pre_activation_behavior_checked_element_in_group = nullptr;
}

GC::Ptr<DecodedImageData> HTMLInputElement::image_data() const
{
    if (m_resource_request)
        return m_resource_request->image_data();
    return nullptr;
}

bool HTMLInputElement::is_image_available() const
{
    return image_data() != nullptr;
}

Optional<CSSPixels> HTMLInputElement::intrinsic_width() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_width();
    return {};
}

Optional<CSSPixels> HTMLInputElement::intrinsic_height() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_height();
    return {};
}

Optional<CSSPixelFraction> HTMLInputElement::intrinsic_aspect_ratio() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_aspect_ratio();
    return {};
}

RefPtr<Gfx::ImmutableBitmap> HTMLInputElement::current_image_bitmap(Gfx::IntSize size) const
{
    if (auto image_data = this->image_data())
        return image_data->bitmap(0, size);
    return nullptr;
}

void HTMLInputElement::set_visible_in_viewport(bool)
{
    // FIXME: Loosen grip on image data when it's not visible, e.g via volatile memory.
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLInputElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

//  https://html.spec.whatwg.org/multipage/input.html#dom-input-maxlength
WebIDL::Long HTMLInputElement::max_length() const
{
    // The maxLength IDL attribute must reflect the maxlength content attribute, limited to only non-negative numbers.
    if (auto maxlength_string = get_attribute(HTML::AttributeNames::maxlength); maxlength_string.has_value()) {
        if (auto maxlength = parse_non_negative_integer(*maxlength_string); maxlength.has_value() && *maxlength <= 2147483647)
            return *maxlength;
    }
    return -1;
}

WebIDL::ExceptionOr<void> HTMLInputElement::set_max_length(WebIDL::Long value)
{
    // The maxLength IDL attribute must reflect the maxlength content attribute, limited to only non-negative numbers.
    return set_attribute(HTML::AttributeNames::maxlength, TRY(convert_non_negative_integer_to_string(realm(), value)));
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-minlength
WebIDL::Long HTMLInputElement::min_length() const
{
    // The minLength IDL attribute must reflect the minlength content attribute, limited to only non-negative numbers.
    if (auto minlength_string = get_attribute(HTML::AttributeNames::minlength); minlength_string.has_value()) {
        if (auto minlength = parse_non_negative_integer(*minlength_string); minlength.has_value() && *minlength <= 2147483647)
            return *minlength;
    }
    return -1;
}

WebIDL::ExceptionOr<void> HTMLInputElement::set_min_length(WebIDL::Long value)
{
    // The minLength IDL attribute must reflect the minlength content attribute, limited to only non-negative numbers.
    return set_attribute(HTML::AttributeNames::minlength, TRY(convert_non_negative_integer_to_string(realm(), value)));
}

// https://html.spec.whatwg.org/multipage/input.html#the-size-attribute
WebIDL::UnsignedLong HTMLInputElement::size() const
{
    // The size attribute, if specified, must have a value that is a valid non-negative integer greater than zero.
    // The size IDL attribute is limited to only positive numbers and has a default value of 20.
    if (auto size_string = get_attribute(HTML::AttributeNames::size); size_string.has_value()) {
        if (auto size = parse_non_negative_integer(*size_string); size.has_value() && *size != 0 && *size <= 2147483647)
            return *size;
    }
    return 20;
}

WebIDL::ExceptionOr<void> HTMLInputElement::set_size(WebIDL::UnsignedLong value)
{
    if (value == 0)
        return WebIDL::IndexSizeError::create(realm(), "Size must be greater than zero"_string);
    if (value > 2147483647)
        value = 20;
    return set_attribute(HTML::AttributeNames::size, String::number(value));
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-height
WebIDL::UnsignedLong HTMLInputElement::height() const
{
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLInputElementHeight);

    // When the input element's type attribute is not in the Image Button state, then no image is available.
    if (type_state() != TypeAttributeState::ImageButton)
        return 0;

    // Return the rendered height of the image, in CSS pixels, if the image is being rendered.
    if (auto* paintable_box = this->paintable_box())
        return paintable_box->content_height().to_int();

    // On setting [the width or height IDL attribute], they must act as if they reflected the respective content attributes of the same name.
    if (auto height_string = get_attribute(HTML::AttributeNames::height); height_string.has_value()) {
        if (auto height = parse_non_negative_integer(*height_string); height.has_value() && *height <= 2147483647)
            return *height;
    }

    // ...or else the natural height and height of the image, in CSS pixels, if an image is available but not being rendered
    if (auto bitmap = current_image_bitmap())
        return bitmap->height();

    // ...or else 0, if the image is not available or does not have intrinsic dimensions.
    return 0;
}

WebIDL::ExceptionOr<void> HTMLInputElement::set_height(WebIDL::UnsignedLong value)
{
    if (value > 2147483647)
        value = 0;

    return set_attribute(HTML::AttributeNames::height, String::number(value));
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-width
WebIDL::UnsignedLong HTMLInputElement::width() const
{
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::HTMLInputElementWidth);

    // When the input element's type attribute is not in the Image Button state, then no image is available.
    if (type_state() != TypeAttributeState::ImageButton)
        return 0;

    // Return the rendered width of the image, in CSS pixels, if the image is being rendered.
    if (auto* paintable_box = this->paintable_box())
        return paintable_box->content_width().to_int();

    // On setting [the width or height IDL attribute], they must act as if they reflected the respective content attributes of the same name.
    if (auto width_string = get_attribute(HTML::AttributeNames::width); width_string.has_value()) {
        if (auto width = parse_non_negative_integer(*width_string); width.has_value() && *width <= 2147483647)
            return *width;
    }

    // ...or else the natural width and height of the image, in CSS pixels, if an image is available but not being rendered
    if (auto bitmap = current_image_bitmap())
        return bitmap->width();

    // ...or else 0, if the image is not available or does not have intrinsic dimensions.
    return 0;
}

WebIDL::ExceptionOr<void> HTMLInputElement::set_width(WebIDL::UnsignedLong value)
{
    if (value > 2147483647)
        value = 0;

    return set_attribute(HTML::AttributeNames::width, String::number(value));
}

// https://html.spec.whatwg.org/multipage/input.html#month-state-(type=month):concept-input-value-string-number
static Optional<double> convert_month_string_to_number(StringView input)
{
    // The algorithm to convert a string to a number, given a string input, is as follows: If parsing a month from input
    // results in an error, then return an error; otherwise, return the number of months between January 1970 and the
    // parsed month.
    auto maybe_year_and_month = parse_a_month_string(input);
    if (!maybe_year_and_month.has_value())
        return {};
    return number_of_months_since_unix_epoch(maybe_year_and_month.value());
}

// https://html.spec.whatwg.org/multipage/input.html#week-state-(type=week):concept-input-value-string-number
static Optional<double> convert_week_string_to_number(StringView input)
{
    // The algorithm to convert a string to a number, given a string input, is as follows: If parsing a week
    // string from input results in an error, then return an error; otherwise, return the number of
    // milliseconds elapsed from midnight UTC on the morning of 1970-01-01 (the time represented by the value
    // "1970-01-01T00:00:00.0Z") to midnight UTC on the morning of the Monday of the parsed week, ignoring
    // leap seconds.
    auto parsed_week = parse_a_week_string(input);
    if (!parsed_week.has_value())
        return {};
    return UnixDateTime::from_iso8601_week(parsed_week->week_year, parsed_week->week).milliseconds_since_epoch();
}

// https://html.spec.whatwg.org/multipage/input.html#date-state-(type=date):concept-input-value-number-string
static Optional<double> convert_date_string_to_number(StringView input)
{
    // The algorithm to convert a string to a number, given a string input, is as follows: If parsing a date
    // from input results in an error, then return an error; otherwise, return the number of milliseconds
    // elapsed from midnight UTC on the morning of 1970-01-01 (the time represented by the value
    // "1970-01-01T00:00:00.0Z") to midnight UTC on the morning of the parsed date, ignoring leap seconds.
    auto maybe_date = parse_a_date_string(input);
    if (!maybe_date.has_value())
        return {};
    auto date = maybe_date.value();

    auto date_time = UnixDateTime::from_unix_time_parts(date.year, date.month, date.day, 0, 0, 0, 0);
    return date_time.milliseconds_since_epoch();
}

// https://html.spec.whatwg.org/multipage/input.html#local-date-and-time-state-(type=datetime-local):parse-a-local-date-and-time-string-2
static Optional<double> convert_local_date_and_time_string_to_number(StringView input)
{
    // The algorithm to convert a string to a number, given a string input, is as follows: If parsing a date and time
    // from input results in an error, then return an error; otherwise, return the number of milliseconds elapsed from
    // midnight on the morning of 1970-01-01 (the time represented by the value "1970-01-01T00:00:00.0") to the parsed
    // local date and time, ignoring leap seconds.
    auto maybe_date_and_time = parse_a_local_date_and_time_string(input);
    if (!maybe_date_and_time.has_value())
        return {};
    auto date_and_time = maybe_date_and_time.value();
    auto date = date_and_time.date;
    auto time = date_and_time.time;

    auto date_time = UnixDateTime::from_unix_time_parts(date.year, date.month, date.day, time.hour, time.minute, time.second, 0);
    return date_time.milliseconds_since_epoch();
}

// https://html.spec.whatwg.org/multipage/input.html#time-state-(type=time):concept-input-value-string-number
Optional<double> HTMLInputElement::convert_time_string_to_number(StringView input) const
{
    // The algorithm to convert a string to a number, given a string input, is as follows: If parsing a time from input
    // results in an error, then return an error; otherwise, return the number of milliseconds elapsed from midnight to
    // the parsed time on a day with no time changes.
    auto maybe_time = parse_time_string(realm(), input);
    if (maybe_time.is_exception())
        return {};
    return maybe_time.value()->date_value();
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-value-string-number
Optional<double> HTMLInputElement::convert_string_to_number(StringView input) const
{
    // https://html.spec.whatwg.org/multipage/input.html#number-state-(type=number):concept-input-value-string-number
    if (type_state() == TypeAttributeState::Number)
        return parse_floating_point_number(input);

    // https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range):concept-input-value-string-number
    if (type_state() == TypeAttributeState::Range)
        return parse_floating_point_number(input);

    if (type_state() == TypeAttributeState::Month)
        return convert_month_string_to_number(input);

    if (type_state() == TypeAttributeState::Week)
        return convert_week_string_to_number(input);

    if (type_state() == TypeAttributeState::Date)
        return convert_date_string_to_number(input);

    if (type_state() == TypeAttributeState::Time)
        return convert_time_string_to_number(input);

    if (type_state() == TypeAttributeState::LocalDateAndTime)
        return convert_local_date_and_time_string_to_number(input);

    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#month-state-(type=month):concept-input-value-number-string
static String convert_number_to_month_string(double input)
{
    // The algorithm to convert a number to a string, given a number input, is as follows: Return a valid month
    // string that represents the month that has input months between it and January 1970.
    auto months = JS::modulo(input, 12);
    auto year = 1970 + (input - months) / 12;

    return MUST(String::formatted("{:04d}-{:02d}", static_cast<int>(year), static_cast<int>(months) + 1));
}

// https://html.spec.whatwg.org/multipage/input.html#week-state-(type=week):concept-input-value-string-number
static String convert_number_to_week_string(double input)
{
    // The algorithm to convert a number to a string, given a number input, is as follows: Return a valid week string that
    // that represents the week that, in UTC, is current input milliseconds after midnight UTC on the morning of 1970-01-01
    // (the time represented by the value "1970-01-01T00:00:00.0Z").

    int days_since_epoch = static_cast<int>(input / AK::ms_per_day);
    int year = 1970;

    while (true) {
        auto days = days_in_year(year);
        if (days_since_epoch < days)
            break;
        days_since_epoch -= days;
        ++year;
    }

    auto january_1_weekday = day_of_week(year, 1, 1);
    int offset_to_week_start = (january_1_weekday <= 3) ? january_1_weekday : january_1_weekday - 7;
    int week = (days_since_epoch + offset_to_week_start) / 7 + 1;

    if (week < 0) {
        --year;
        week = weeks_in_year(year) + week;
    }

    return MUST(String::formatted("{:04d}-W{:02d}", year, week));
}

// https://html.spec.whatwg.org/multipage/input.html#date-state-(type=date):concept-input-value-number-string
static String convert_number_to_date_string(double input)
{
    // The algorithm to convert a number to a string, given a number input, is as follows: Return a valid
    // date string that represents the date that, in UTC, is current input milliseconds after midnight UTC
    // on the morning of 1970-01-01 (the time represented by the value "1970-01-01T00:00:00.0Z").
    auto date = Core::DateTime::from_timestamp(input / 1000.);
    return MUST(date.to_string("%Y-%m-%d"sv, Core::DateTime::LocalTime::No));
}

// https://html.spec.whatwg.org/multipage/input.html#time-state-(type=time):concept-input-value-number-string
static String convert_number_to_time_string(double input)
{
    // The algorithm to convert a number to a string, given a number input, is as follows: Return a valid time
    // string that represents the time that is input milliseconds after midnight on a day with no time changes.
    auto seconds = JS::sec_from_time(input);
    auto milliseconds = JS::ms_from_time(input);
    if (seconds > 0) {
        if (milliseconds > 0)
            return MUST(String::formatted("{:02d}:{:02d}:{:02d}.{:3d}", JS::hour_from_time(input), JS::min_from_time(input), seconds, milliseconds));
        return MUST(String::formatted("{:02d}:{:02d}:{:02d}", JS::hour_from_time(input), JS::min_from_time(input), seconds));
    }
    return MUST(String::formatted("{:02d}:{:02d}", JS::hour_from_time(input), JS::min_from_time(input)));
}

// https://html.spec.whatwg.org/multipage/input.html#local-date-and-time-state-(type=datetime-local):concept-input-value-number-string
static String convert_number_to_local_date_and_time_string(double input)
{
    // The algorithm to convert a number to a string, given a number input, is as follows: Return a valid
    // normalized local date and time string that represents the date and time that is input milliseconds
    // after midnight on the morning of 1970-01-01 (the time represented by the value "1970-01-01T00:00:00.0").
    auto year = JS::year_from_time(input);
    auto month = JS::month_from_time(input) + 1; // Adjust for zero-based month
    auto day = JS::date_from_time(input);
    auto hour = JS::hour_from_time(input);
    auto minutes = JS::min_from_time(input);
    auto seconds = JS::sec_from_time(input);
    auto milliseconds = JS::ms_from_time(input);

    if (seconds > 0) {
        if (milliseconds > 0)
            return MUST(String::formatted("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}", year, month, day, hour, minutes, seconds, milliseconds));
        return MUST(String::formatted("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}", year, month, day, hour, minutes, seconds));
    }

    return MUST(String::formatted("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}", year, month, day, hour, minutes));
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-value-string-number
String HTMLInputElement::convert_number_to_string(double input) const
{
    // https://html.spec.whatwg.org/multipage/input.html#number-state-(type=number):concept-input-value-number-string
    if (type_state() == TypeAttributeState::Number)
        return String::number(input);

    // https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range):concept-input-value-number-string
    if (type_state() == TypeAttributeState::Range)
        return String::number(input);

    if (type_state() == TypeAttributeState::Month)
        return convert_number_to_month_string(input);

    if (type_state() == TypeAttributeState::Week)
        return convert_number_to_week_string(input);

    if (type_state() == TypeAttributeState::Date)
        return convert_number_to_date_string(input);

    if (type_state() == TypeAttributeState::Time)
        return convert_number_to_time_string(input);

    if (type_state() == TypeAttributeState::LocalDateAndTime)
        return convert_number_to_local_date_and_time_string(input);

    dbgln("HTMLInputElement::convert_number_to_string() not implemented for input type {}", type());
    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-value-string-date
WebIDL::ExceptionOr<GC::Ptr<JS::Date>> HTMLInputElement::convert_string_to_date(StringView input) const
{
    // https://html.spec.whatwg.org/multipage/input.html#date-state-(type=date):concept-input-value-string-date
    if (type_state() == TypeAttributeState::Date) {
        // If parsing a date from input results in an error, then return an error;
        auto maybe_date = parse_a_date_string(input);
        if (!maybe_date.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Can't parse date string"sv };
        auto date = maybe_date.value();

        // otherwise, return a new Date object representing midnight UTC on the morning of the parsed date.
        return JS::Date::create(realm(), JS::make_date(JS::make_day(date.year, date.month - 1, date.day), 0));
    }

    // https://html.spec.whatwg.org/multipage/input.html#time-state-(type=time):concept-input-value-string-date
    if (type_state() == TypeAttributeState::Time) {
        // If parsing a time from input results in an error, then return an error;
        auto maybe_time = parse_time_string(realm(), input);
        if (maybe_time.is_exception())
            return maybe_time.exception();

        // otherwise, return a new Date object representing the parsed time in UTC on 1970-01-01.
        return maybe_time.value();
    }

    dbgln("HTMLInputElement::convert_string_to_date() not implemented for input type {}", type());
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-value-date-string
String HTMLInputElement::covert_date_to_string(GC::Ref<JS::Date> input) const
{
    // https://html.spec.whatwg.org/multipage/input.html#date-state-(type=date):concept-input-value-date-string
    if (type_state() == TypeAttributeState::Date) {
        // Return a valid date string that represents the date current at the time represented by input in the UTC time zone.
        // https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-date-string
        return convert_number_to_date_string(input->date_value());
    }

    // https://html.spec.whatwg.org/multipage/input.html#time-state-(type=time):concept-input-value-string-date
    if (type_state() == TypeAttributeState::Time) {
        // Return a valid time string that represents the UTC time component that is represented by input.
        // https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-time-string
        return convert_number_to_time_string(input->date_value());
    }

    dbgln("HTMLInputElement::covert_date_to_string() not implemented for input type {}", type());
    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-min
Optional<double> HTMLInputElement::min() const
{
    // If the element has a min attribute, and the result of applying the algorithm to convert a string to a number to
    // the value of the min attribute is a number, then that number is the element's minimum; otherwise, if the type
    // attribute's current state defines a default minimum, then that is the minimum; otherwise, the element has no minimum.
    if (auto min_string = get_attribute(HTML::AttributeNames::min); min_string.has_value()) {
        if (auto min = convert_string_to_number(*min_string); min.has_value())
            return *min;
    }

    // https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range):concept-input-min-default
    if (type_state() == TypeAttributeState::Range)
        return 0;

    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-max
Optional<double> HTMLInputElement::max() const
{
    // If the element has a max attribute, and the result of applying the algorithm to convert a string to a number to the
    // value of the max attribute is a number, then that number is the element's maximum; otherwise, if the type attribute's
    // current state defines a default maximum, then that is the maximum; otherwise, the element has no maximum.
    if (auto max_string = get_attribute(HTML::AttributeNames::max); max_string.has_value()) {
        if (auto max = convert_string_to_number(*max_string); max.has_value())
            return *max;
    }

    // https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range):concept-input-max-default
    if (type_state() == TypeAttributeState::Range)
        return 100;

    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-step-default
double HTMLInputElement::default_step() const
{
    // https://html.spec.whatwg.org/multipage/input.html#number-state-(type=number):concept-input-step-default
    if (type_state() == TypeAttributeState::Number)
        return 1;

    // https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range):concept-input-step-default
    if (type_state() == TypeAttributeState::Range)
        return 1;

    // https://html.spec.whatwg.org/multipage/input.html#time-state-(type=time):concept-input-step-default
    if (type_state() == TypeAttributeState::Time)
        return 60;

    dbgln("HTMLInputElement::default_step() not implemented for input type {}", type());
    return 0;
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-step-scale
double HTMLInputElement::step_scale_factor() const
{
    // https://html.spec.whatwg.org/multipage/input.html#number-state-(type=number):concept-input-step-scale
    if (type_state() == TypeAttributeState::Number)
        return 1;

    // https://html.spec.whatwg.org/multipage/input.html#range-state-(type=range):concept-input-step-scale
    if (type_state() == TypeAttributeState::Range)
        return 1;

    // https://html.spec.whatwg.org/multipage/input.html#time-state-(type=time):concept-input-step-scale
    if (type_state() == TypeAttributeState::Time)
        return 1000;

    dbgln("HTMLInputElement::step_scale_factor() not implemented for input type {}", type());
    return 0;
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-step
Optional<double> HTMLInputElement::allowed_value_step() const
{
    // 1. If the attribute does not apply, then there is no allowed value step.
    if (!step_applies())
        return {};

    // 2. Otherwise, if the attribute is absent, then the allowed value step is the default step multiplied by the step scale factor.
    auto maybe_step_string = get_attribute(AttributeNames::step);
    if (!maybe_step_string.has_value())
        return default_step() * step_scale_factor();
    auto step_string = *maybe_step_string;

    // 3. Otherwise, if the attribute's value is an ASCII case-insensitive match for the string "any", then there is no allowed value step.
    if (Infra::is_ascii_case_insensitive_match(step_string, "any"_string))
        return {};

    // 4. Otherwise, if the rules for parsing floating-point number values, when they are applied to the attribute's value, return an error,
    // zero, or a number less than zero, then the allowed value step is the default step multiplied by the step scale factor.
    auto maybe_step = parse_floating_point_number(step_string);
    if (!maybe_step.has_value() || *maybe_step == 0 || *maybe_step < 0)
        return default_step() * step_scale_factor();

    // 5. Otherwise, the allowed value step is the number returned by the rules for parsing floating-point number values when they are applied
    // to the attribute's value, multiplied by the step scale factor.
    return *maybe_step * step_scale_factor();
}

// https://html.spec.whatwg.org/multipage/input.html#concept-input-min-zero
double HTMLInputElement::step_base() const
{
    // 1. If the element has a min content attribute, and the result of applying the algorithm to convert a string to a number to the value of
    // the min content attribute is not an error, then return that result.
    if (auto min = this->min(); min.has_value())
        return *min;

    // 2. If the element has a value content attribute, and the result of applying the algorithm to convert a string to a number to the value of
    // the value content attribute is not an error, then return that result.
    if (auto value = get_attribute(HTML::AttributeNames::value); value.has_value()) {
        if (auto value_as_number = convert_string_to_number(value.value()); value_as_number.has_value())
            return value_as_number.value();
    }

    // 3. If a default step base is defined for this element given its type attribute's state, then return it.
    if (type_state() == TypeAttributeState::Week) {
        // The default step base is −259,200,000 (the start of week 1970-W01).
        return -259'200'000;
    }

    // 4. Return zero.
    return 0;
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-valueasdate
JS::Object* HTMLInputElement::value_as_date() const
{
    // On getting, if the valueAsDate attribute does not apply, as defined for the input element's type attribute's current state, then return null.
    if (!value_as_date_applies())
        return nullptr;

    // Otherwise, run the algorithm to convert a string to a Date object defined for that state to the element's value;
    // if the algorithm returned a Date object, then return it, otherwise, return null.
    auto maybe_date = convert_string_to_date(value());
    if (!maybe_date.is_exception())
        return maybe_date.value().ptr();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-valueasdate
WebIDL::ExceptionOr<void> HTMLInputElement::set_value_as_date(Optional<GC::Root<JS::Object>> const& value)
{
    // On setting, if the valueAsDate attribute does not apply, as defined for the input element's type attribute's current state, then throw an "InvalidStateError" DOMException;
    if (!value_as_date_applies())
        return WebIDL::InvalidStateError::create(realm(), "valueAsDate: Invalid input type used"_string);

    // otherwise, if the new value is not null and not a Date object throw a TypeError exception;
    if (value.has_value() && !is<JS::Date>(**value))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "valueAsDate: input is not a Date"sv };

    // otherwise if the new value is null or a Date object representing the NaN time value, then set the value of the element to the empty string;
    if (!value.has_value()) {
        TRY(set_value(String {}));
        return {};
    }
    auto& date = static_cast<JS::Date&>(**value);
    if (!isfinite(date.date_value())) {
        TRY(set_value(String {}));
        return {};
    }

    // otherwise, run the algorithm to convert a Date object to a string, as defined for that state, on the new value, and set the value of the element to the resulting string.
    TRY(set_value(covert_date_to_string(date)));
    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-valueasnumber
double HTMLInputElement::value_as_number() const
{
    // On getting, if the valueAsNumber attribute does not apply, as defined for the input element's type attribute's current state, then return a Not-a-Number (NaN) value.
    if (!value_as_number_applies())
        return NAN;

    // Otherwise, run the algorithm to convert a string to a number defined for that state to the element's value;
    // if the algorithm returned a number, then return it, otherwise, return a Not-a-Number (NaN) value.
    return convert_string_to_number(value()).value_or(NAN);
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-valueasnumber
WebIDL::ExceptionOr<void> HTMLInputElement::set_value_as_number(double value)
{
    // On setting, if the new value is infinite, then throw a TypeError exception.
    if (!isfinite(value))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "valueAsNumber: Value is infinite"sv };

    // Otherwise, if the valueAsNumber attribute does not apply, as defined for the input element's type attribute's current state, then throw an "InvalidStateError" DOMException.
    if (!value_as_number_applies())
        return WebIDL::InvalidStateError::create(realm(), "valueAsNumber: Invalid input type used"_string);

    // Otherwise, if the new value is a Not-a-Number (NaN) value, then set the value of the element to the empty string.
    if (value == NAN) {
        TRY(set_value(String {}));
        return {};
    }

    // Otherwise, run the algorithm to convert a number to a string, as defined for that state, on the new value, and set the value of the element to the resulting string.
    TRY(set_value(convert_number_to_string(value)));
    return {};
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-stepup
WebIDL::ExceptionOr<void> HTMLInputElement::step_up(WebIDL::Long n)
{
    return step_up_or_down(false, n);
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-stepdown
WebIDL::ExceptionOr<void> HTMLInputElement::step_down(WebIDL::Long n)
{
    return step_up_or_down(true, n);
}

// https://html.spec.whatwg.org/multipage/input.html#dom-input-stepup
WebIDL::ExceptionOr<void> HTMLInputElement::step_up_or_down(bool is_down, WebIDL::Long n)
{
    // 1. If the stepDown() and stepUp() methods do not apply, as defined for the input element's type attribute's current state, then throw an "InvalidStateError" DOMException.
    if (!step_up_or_down_applies())
        return WebIDL::InvalidStateError::create(realm(), MUST(String::formatted("{}: Invalid input type used", is_down ? "stepDown()" : "stepUp()")));

    // 2. If the element has no allowed value step, then throw an "InvalidStateError" DOMException.
    auto maybe_allowed_value_step = allowed_value_step();
    if (!maybe_allowed_value_step.has_value())
        return WebIDL::InvalidStateError::create(realm(), "element has no allowed value step"_string);
    double allowed_value_step = *maybe_allowed_value_step;

    // 3. If the element has a minimum and a maximum and the minimum is greater than the maximum, then return.
    auto maybe_minimum = min();
    auto maybe_maximum = max();
    if (maybe_minimum.has_value() && maybe_maximum.has_value() && *maybe_minimum > *maybe_maximum)
        return {};

    // FIXME: 4. If the element has a minimum and a maximum and there is no value greater than or equal to the element's minimum and less than
    // or equal to the element's maximum that, when subtracted from the step base, is an integral multiple of the allowed value step, then return.

    // 5. If applying the algorithm to convert a string to a number to the string given by the element's value does not result in an error,
    // then let value be the result of that algorithm. Otherwise, let value be zero.
    double value = convert_string_to_number(this->value()).value_or(0);

    // 6. Let valueBeforeStepping be value.
    double value_before_stepping = value;

    // 7. If value subtracted from the step base is not an integral multiple of the allowed value step, then set value to the nearest value that,
    // when subtracted from the step base, is an integral multiple of the allowed value step, and that is less than value if the method invoked was the stepDown() method, and more than value otherwise.
    if (fmod(step_base() - value, allowed_value_step) != 0) {
        double diff = step_base() - value;
        if (is_down) {
            value = diff - fmod(diff, allowed_value_step);
        } else {
            value = diff + fmod(diff, allowed_value_step);
        }
    } else {
        // 1. Let n be the argument.
        // 2. Let delta be the allowed value step multiplied by n.
        double delta = allowed_value_step * n;

        // 3. If the method invoked was the stepDown() method, negate delta.
        if (is_down)
            delta = -delta;

        // 4. Let value be the result of adding delta to value.
        value += delta;
    }

    // 8. If the element has a minimum, and value is less than that minimum, then set value to the smallest value that,
    //    when subtracted from the step base, is an integral multiple of the allowed value step, and that is more than
    //    or equal to that minimum.
    if (maybe_minimum.has_value() && value < *maybe_minimum) {
        value = AK::max(value, *maybe_minimum);
    }

    // 9. If the element has a maximum, and value is greater than that maximum, then set value to the largest value that,
    //    when subtracted from the step base, is an integral multiple of the allowed value step, and that is less than
    //    or equal to that maximum.
    if (maybe_maximum.has_value() && value > *maybe_maximum) {
        value = AK::min(value, *maybe_maximum);
    }

    // 10. If either the method invoked was the stepDown() method and value is greater than valueBeforeStepping,
    // or the method invoked was the stepUp() method and value is less than valueBeforeStepping, then return.
    if (is_down) {
        if (value > value_before_stepping)
            return {};
    } else {
        if (value < value_before_stepping)
            return {};
    }

    // 11. Let value as string be the result of running the algorithm to convert a number to a string,
    // as defined for the input element's type attribute's current state, on value.
    auto value_as_string = convert_number_to_string(value);

    // 12. Set the value of the element to value as string.
    TRY(set_value(value_as_string));
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-willvalidate
bool HTMLInputElement::will_validate()
{
    // The willValidate attribute's getter must return true, if this element is a candidate for constraint validation
    return is_candidate_for_constraint_validation();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-checkvalidity
WebIDL::ExceptionOr<bool> HTMLInputElement::check_validity()
{
    return check_validity_steps();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-reportvalidity
WebIDL::ExceptionOr<bool> HTMLInputElement::report_validity()
{
    dbgln("(STUBBED) HTMLInputElement::report_validity(). Called on: {}", debug_description());
    return true;
}

Optional<ARIA::Role> HTMLInputElement::default_role() const
{
    // http://wpt.live/html-aam/roles-dynamic-switch.tentative.window.html "Disconnected <input type=checkbox switch>"
    if (!is_connected())
        return {};
    // https://www.w3.org/TR/html-aria/#el-input-button
    if (type_state() == TypeAttributeState::Button)
        return ARIA::Role::button;
    // https://www.w3.org/TR/html-aria/#el-input-checkbox
    if (type_state() == TypeAttributeState::Checkbox) {
        // https://github.com/w3c/html-aam/issues/496
        if (has_attribute(HTML::AttributeNames::switch_))
            return ARIA::Role::switch_;
        return ARIA::Role::checkbox;
    }
    // https://www.w3.org/TR/html-aria/#el-input-email
    if (type_state() == TypeAttributeState::Email && !has_attribute(AttributeNames::list))
        return ARIA::Role::textbox;
    // https://www.w3.org/TR/html-aria/#el-input-image
    if (type_state() == TypeAttributeState::ImageButton)
        return ARIA::Role::button;
    // https://www.w3.org/TR/html-aria/#el-input-number
    if (type_state() == TypeAttributeState::Number)
        return ARIA::Role::spinbutton;
    // https://www.w3.org/TR/html-aria/#el-input-radio
    if (type_state() == TypeAttributeState::RadioButton)
        return ARIA::Role::radio;
    // https://www.w3.org/TR/html-aria/#el-input-range
    if (type_state() == TypeAttributeState::Range)
        return ARIA::Role::slider;
    // https://www.w3.org/TR/html-aria/#el-input-reset
    if (type_state() == TypeAttributeState::ResetButton)
        return ARIA::Role::button;
    // https://www.w3.org/TR/html-aria/#el-input-text-list
    if ((type_state() == TypeAttributeState::Text
            || type_state() == TypeAttributeState::Search
            || type_state() == TypeAttributeState::Telephone
            || type_state() == TypeAttributeState::URL
            || type_state() == TypeAttributeState::Email)
        && has_attribute(AttributeNames::list))
        return ARIA::Role::combobox;
    // https://www.w3.org/TR/html-aria/#el-input-search
    if (type_state() == TypeAttributeState::Search && !has_attribute(AttributeNames::list))
        return ARIA::Role::searchbox;
    // https://www.w3.org/TR/html-aria/#el-input-submit
    if (type_state() == TypeAttributeState::SubmitButton)
        return ARIA::Role::button;
    // https://www.w3.org/TR/html-aria/#el-input-tel
    if (type_state() == TypeAttributeState::Telephone)
        return ARIA::Role::textbox;
    // https://www.w3.org/TR/html-aria/#el-input-text
    if (type_state() == TypeAttributeState::Text && !has_attribute(AttributeNames::list))
        return ARIA::Role::textbox;
    // https://www.w3.org/TR/html-aria/#el-input-url
    if (type_state() == TypeAttributeState::URL && !has_attribute(AttributeNames::list))
        return ARIA::Role::textbox;

    // https://www.w3.org/TR/html-aria/#el-input-color
    // https://www.w3.org/TR/html-aria/#el-input-date
    // https://www.w3.org/TR/html-aria/#el-input-datetime-local
    // https://www.w3.org/TR/html-aria/#el-input-file
    // https://www.w3.org/TR/html-aria/#el-input-hidden
    // https://www.w3.org/TR/html-aria/#el-input-month
    // https://www.w3.org/TR/html-aria/#el-input-password
    // https://www.w3.org/TR/html-aria/#el-input-time
    // https://www.w3.org/TR/html-aria/#el-input-week
    return {};
}

bool HTMLInputElement::is_button() const
{
    // https://html.spec.whatwg.org/multipage/input.html#submit-button-state-(type=submit):concept-button
    // https://html.spec.whatwg.org/multipage/input.html#image-button-state-(type=image):concept-button
    // https://html.spec.whatwg.org/multipage/input.html#reset-button-state-(type=reset):concept-button
    // https://html.spec.whatwg.org/multipage/input.html#button-state-(type=button):concept-button
    return type_state() == TypeAttributeState::SubmitButton
        || type_state() == TypeAttributeState::ImageButton
        || type_state() == TypeAttributeState::ResetButton
        || type_state() == TypeAttributeState::Button;
}

bool HTMLInputElement::is_submit_button() const
{
    // https://html.spec.whatwg.org/multipage/input.html#submit-button-state-(type=submit):concept-submit-button
    // https://html.spec.whatwg.org/multipage/input.html#image-button-state-(type=image):concept-submit-button
    return type_state() == TypeAttributeState::SubmitButton
        || type_state() == TypeAttributeState::ImageButton;
}

// https://html.spec.whatwg.org/multipage/input.html#text-(type=text)-state-and-search-state-(type=search)
// https://html.spec.whatwg.org/multipage/input.html#password-state-(type=password)
// "one line plain text edit control"
bool HTMLInputElement::is_single_line() const
{
    // NOTE: For web compatibility reasons, we consider other types
    //       in addition to Text, Search, and Password as single line inputs.
    return type_state() == TypeAttributeState::Text
        || type_state() == TypeAttributeState::Search
        || type_state() == TypeAttributeState::Password
        || type_state() == TypeAttributeState::Email
        || type_state() == TypeAttributeState::Telephone
        || type_state() == TypeAttributeState::Number;
}

bool HTMLInputElement::has_activation_behavior() const
{
    return true;
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:activation-behaviour
void HTMLInputElement::activation_behavior(DOM::Event const& event)
{
    // The activation behavior for input elements are these steps:

    // 1. If element is not mutable, and element's type attribute is neither in the Checkbox nor in the Radio state, then return.
    if (!is_mutable() && !first_is_one_of(m_type, TypeAttributeState::Checkbox, TypeAttributeState::RadioButton))
        return;

    // 2. Run element's input activation behavior, if any, and do nothing otherwise.
    run_input_activation_behavior(event).release_value_but_fixme_should_propagate_errors();

    // 3. If element has a form owner and element's type attribute is not in the Button state, then return.
    if (form() != nullptr && type_state() != TypeAttributeState::Button)
        return;

    // 4. Run the popover target attribute activation behavior given element and event's target.
    if (event.target() && event.target()->is_dom_node())
        PopoverInvokerElement::popover_target_activation_behaviour(*this, as<DOM::Node>(*event.target()));
}

bool HTMLInputElement::has_input_activation_behavior() const
{
    switch (type_state()) {
    case TypeAttributeState::Checkbox:
    case TypeAttributeState::Color:
    case TypeAttributeState::FileUpload:
    case TypeAttributeState::ImageButton:
    case TypeAttributeState::RadioButton:
    case TypeAttributeState::ResetButton:
    case TypeAttributeState::SubmitButton:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#do-not-apply
bool HTMLInputElement::select_applies() const
{
    switch (type_state()) {
    case TypeAttributeState::Button:
    case TypeAttributeState::Checkbox:
    case TypeAttributeState::Hidden:
    case TypeAttributeState::ImageButton:
    case TypeAttributeState::RadioButton:
    case TypeAttributeState::Range:
    case TypeAttributeState::ResetButton:
    case TypeAttributeState::SubmitButton:
        return false;
    default:
        return true;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#do-not-apply
bool HTMLInputElement::selection_or_range_applies() const
{
    return selection_or_range_applies_for_type_state(type_state());
}

// https://html.spec.whatwg.org/multipage/input.html#do-not-apply
bool HTMLInputElement::selection_direction_applies() const
{
    switch (type_state()) {
    case TypeAttributeState::Text:
    case TypeAttributeState::Search:
    case TypeAttributeState::Telephone:
    case TypeAttributeState::URL:
    case TypeAttributeState::Password:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#do-not-apply
bool HTMLInputElement::pattern_applies() const
{
    switch (type_state()) {
    case TypeAttributeState::Text:
    case TypeAttributeState::Search:
    case TypeAttributeState::Telephone:
    case TypeAttributeState::URL:
    case TypeAttributeState::Email:
    case TypeAttributeState::Password:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#do-not-apply
bool HTMLInputElement::multiple_applies() const
{
    switch (type_state()) {
    case TypeAttributeState::Email:
    case TypeAttributeState::FileUpload:
        return true;
    default:
        return false;
    }
}

bool HTMLInputElement::has_selectable_text() const
{
    // Potential FIXME: Date, Month, Week, Time and LocalDateAndTime are rendered as a basic text input for now,
    // thus they have selectable text, this need to change when we will have a visual date/time selector.

    switch (type_state()) {
    case TypeAttributeState::Text:
    case TypeAttributeState::Search:
    case TypeAttributeState::Telephone:
    case TypeAttributeState::URL:
    case TypeAttributeState::Password:
    case TypeAttributeState::Date:
    case TypeAttributeState::Month:
    case TypeAttributeState::Week:
    case TypeAttributeState::Time:
    case TypeAttributeState::LocalDateAndTime:
    case TypeAttributeState::Number:
        return true;
    default:
        return false;
    }
}

bool HTMLInputElement::selection_or_range_applies_for_type_state(TypeAttributeState type_state)
{
    switch (type_state) {
    case TypeAttributeState::Text:
    case TypeAttributeState::Search:
    case TypeAttributeState::Telephone:
    case TypeAttributeState::URL:
    case TypeAttributeState::Password:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:event-change-2
bool HTMLInputElement::change_event_applies() const
{
    switch (type_state()) {
    case TypeAttributeState::Checkbox:
    case TypeAttributeState::Color:
    case TypeAttributeState::Date:
    case TypeAttributeState::Email:
    case TypeAttributeState::FileUpload:
    case TypeAttributeState::LocalDateAndTime:
    case TypeAttributeState::Month:
    case TypeAttributeState::Number:
    case TypeAttributeState::Password:
    case TypeAttributeState::RadioButton:
    case TypeAttributeState::Range:
    case TypeAttributeState::Search:
    case TypeAttributeState::Telephone:
    case TypeAttributeState::Text:
    case TypeAttributeState::Time:
    case TypeAttributeState::URL:
    case TypeAttributeState::Week:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:dom-input-valueasdate-3
bool HTMLInputElement::value_as_date_applies() const
{
    switch (type_state()) {
    case TypeAttributeState::Date:
    case TypeAttributeState::Month:
    case TypeAttributeState::Week:
    case TypeAttributeState::Time:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:dom-input-valueasnumber-3
bool HTMLInputElement::value_as_number_applies() const
{
    switch (type_state()) {
    case TypeAttributeState::Date:
    case TypeAttributeState::Month:
    case TypeAttributeState::Week:
    case TypeAttributeState::Time:
    case TypeAttributeState::LocalDateAndTime:
    case TypeAttributeState::Number:
    case TypeAttributeState::Range:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:attr-input-step-3
bool HTMLInputElement::step_applies() const
{
    return value_as_number_applies();
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:dom-input-stepup-3
bool HTMLInputElement::step_up_or_down_applies() const
{
    return value_as_number_applies();
}

// https://html.spec.whatwg.org/multipage/input.html#the-input-element:dom-input-value-2
HTMLInputElement::ValueAttributeMode HTMLInputElement::value_attribute_mode_for_type_state(TypeAttributeState type_state)
{
    switch (type_state) {
    case TypeAttributeState::Text:
    case TypeAttributeState::Search:
    case TypeAttributeState::Telephone:
    case TypeAttributeState::URL:
    case TypeAttributeState::Email:
    case TypeAttributeState::Password:
    case TypeAttributeState::Date:
    case TypeAttributeState::Month:
    case TypeAttributeState::Week:
    case TypeAttributeState::Time:
    case TypeAttributeState::LocalDateAndTime:
    case TypeAttributeState::Number:
    case TypeAttributeState::Range:
    case TypeAttributeState::Color:
        return ValueAttributeMode::Value;

    case TypeAttributeState::Hidden:
    case TypeAttributeState::SubmitButton:
    case TypeAttributeState::ImageButton:
    case TypeAttributeState::ResetButton:
    case TypeAttributeState::Button:
        return ValueAttributeMode::Default;

    case TypeAttributeState::Checkbox:
    case TypeAttributeState::RadioButton:
        return ValueAttributeMode::DefaultOn;

    case TypeAttributeState::FileUpload:
        return ValueAttributeMode::Filename;
    }

    VERIFY_NOT_REACHED();
}

HTMLInputElement::ValueAttributeMode HTMLInputElement::value_attribute_mode() const
{
    return value_attribute_mode_for_type_state(type_state());
}

bool HTMLInputElement::is_focusable() const
{
    return m_type != TypeAttributeState::Hidden && enabled();
}

// https://html.spec.whatwg.org/multipage/input.html#has-a-reversed-range
bool HTMLInputElement::has_reversed_range() const
{
    auto minimum = min();
    if (!minimum.has_value())
        return false;
    auto maximum = max();
    if (!maximum.has_value())
        return false;
    // An element has a reversed range if it has a periodic domain and its maximum is less than its minimum.
    return has_periodic_domain() && maximum.value() < minimum.value();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-being-missing
bool HTMLInputElement::suffering_from_being_missing() const
{
    bool has_checkedness_false_for_all_elements_in_group = true;
    bool has_required_element_in_group = false;
    switch (type_state()) {
    case TypeAttributeState::Checkbox:
        // https://html.spec.whatwg.org/multipage/input.html#checkbox-state-(type%3Dcheckbox)%3Asuffering-from-being-missing
        // If the element is required and its checkedness is false, then the element is suffering from being missing.
        if (has_attribute(HTML::AttributeNames::required) && !checked())
            return true;
        break;
    case TypeAttributeState::RadioButton:
        // https://html.spec.whatwg.org/multipage/input.html#radio-button-state-(type%3Dradio)%3Asuffering-from-being-missing
        // If an element in the radio button group is required, and all of the input elements in the radio button group
        // have a checkedness that is false, then the element is suffering from being missing.
        root().for_each_in_inclusive_subtree_of_type<HTML::HTMLInputElement>([&](auto& element) {
            if (is_in_same_radio_button_group(*this, element)) {
                if (element.checked())
                    has_checkedness_false_for_all_elements_in_group = false;
                if (has_attribute(HTML::AttributeNames::required))
                    has_required_element_in_group = true;
            }
            return TraversalDecision::Continue;
        });
        if (has_checkedness_false_for_all_elements_in_group && has_required_element_in_group)
            return true;
        break;
    case TypeAttributeState::FileUpload:
        // https://html.spec.whatwg.org/multipage/input.html#file-upload-state-(type%3Dfile)%3Asuffering-from-being-missing
        // If the element is required and the list of selected files is empty, then the element is suffering from being missing.
        if (has_attribute(HTML::AttributeNames::required) && const_cast<HTMLInputElement&>(*this).files()->length() == 0)
            return true;
        break;
    default:
        break;
    }

    // https://html.spec.whatwg.org/multipage/input.html#the-required-attribute%3Asuffering-from-being-missing
    // If the element is required, and its value IDL attribute applies and is in the mode value, and the element is mutable, and the element's value is the empty
    // string, then the element is suffering from being missing.
    if (has_attribute(HTML::AttributeNames::required) && value_attribute_mode() == ValueAttributeMode::Value && is_mutable() && m_value.is_empty())
        return true;

    return false;
}

// https://html.spec.whatwg.org/multipage/input.html#valid-e-mail-address
static Regex<ECMA262> const valid_email_address_regex = Regex<ECMA262>("^[a-zA-Z0-9.!#$%&'*+\\/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$");

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-a-type-mismatch
bool HTMLInputElement::suffering_from_a_type_mismatch() const
{
    auto input = value();
    switch (type_state()) {
    case TypeAttributeState::URL:
        // https://html.spec.whatwg.org/multipage/input.html#url-state-(type%3Durl)%3Asuffering-from-a-type-mismatch
        // While the value of the element is neither the empty string nor a valid absolute URL, the element is suffering from a type mismatch.
        // AD-HOC: https://github.com/whatwg/html/issues/11083 and https://github.com/web-platform-tests/wpt/pull/51011
        //         We intentionally don't check if the value is a "valid absolute URL", because that's not what other
        //         engines actually do. So we instead just implement what matches the behavior in existing engines.
        return !input.is_empty() && !URL::Parser::basic_parse(input).has_value();
    case TypeAttributeState::Email:
        // https://html.spec.whatwg.org/multipage/input.html#email-state-(type%3Demail)%3Asuffering-from-a-type-mismatch
        // When the multiple attribute is not specified on the element: While the value of the element is neither the
        // empty string nor a single valid email address, the element is suffering from a type mismatch.
        if (!has_attribute(HTML::AttributeNames::multiple))
            return !input.is_empty() && !valid_email_address_regex.match(input).success;
        // When the multiple attribute is specified on the element: While the value of the element is not a valid email
        // address list, the element is suffering from a type mismatch.
        // https://html.spec.whatwg.org/multipage/input.html#valid-e-mail-address-list
        // A valid email address list is a set of comma-separated tokens, where each token is itself a valid email
        // address. To obtain the list of tokens from a valid email address list, an implementation must split the
        // string on commas.
        for (auto& address : MUST(input.split(','))) {
            if (!valid_email_address_regex.match(address).success)
                return true;
        }
        break;
    default:
        break;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/input.html#the-pattern-attribute%3Asuffering-from-a-pattern-mismatch
bool HTMLInputElement::suffering_from_a_pattern_mismatch() const
{
    // If the element's value is not the empty string, and either the element's multiple attribute is not specified or it does not apply to the input element given its
    // type attribute's current state, and the element has a compiled pattern regular expression but that regular expression does not match the element's value, then the element is
    // suffering from a pattern mismatch.

    // FIXME: If the element's value is not the empty string, and the element's multiple attribute is specified and applies to the input element,
    //        and the element has a compiled pattern regular expression but that regular expression does not match each of the element's values,
    //        then the element is suffering from a pattern mismatch.

    if (!pattern_applies())
        return false;

    auto value = this->value();
    if (value.is_empty())
        return false;

    if (has_attribute(HTML::AttributeNames::multiple) && multiple_applies())
        return false;

    auto regexp_object = compiled_pattern_regular_expression();
    if (!regexp_object.has_value())
        return false;

    return !regexp_object->match(value).success;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-an-underflow
bool HTMLInputElement::suffering_from_an_underflow() const
{
    // and the result of applying the algorithm to convert a string to a number to the string given by the element's
    // value is a number
    auto result = convert_string_to_number(value());
    if (!result.has_value())
        return false;
    auto number = result.value();
    // https://html.spec.whatwg.org/multipage/input.html#the-min-and-max-attributes%3Asuffering-from-an-underflow-2
    // When the element has a minimum and does not have a reversed range,
    auto minimum = min();
    if (minimum.has_value() && !has_reversed_range()) {
        // and the number obtained from that algorithm is less than the minimum, the element is suffering from an underflow.
        return number < minimum.value();
    }

    if (!minimum.has_value())
        return false;
    auto maximum = max();
    if (!maximum.has_value())
        return false;
    // https://html.spec.whatwg.org/multipage/input.html#the-min-and-max-attributes%3Asuffering-from-an-underflow-3
    // When an element has a reversed range, and the number obtained from that algorithm is more than the maximum and
    // less than the minimum, the element is simultaneously suffering from an underflow and suffering from an overflow.
    return has_reversed_range() && number > maximum.value() && number < minimum.value();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-an-overflow
bool HTMLInputElement::suffering_from_an_overflow() const
{
    // and the result of applying the algorithm to convert a string to a number to the string given by the element's
    // value is a number
    auto result = convert_string_to_number(value());
    if (!result.has_value())
        return false;
    auto number = result.value();
    auto maximum = max();
    // https://html.spec.whatwg.org/multipage/input.html#the-min-and-max-attributes%3Asuffering-from-an-overflow-2
    // When the element has a maximum and does not have a reversed range,
    if (maximum.has_value() && !has_reversed_range()) {
        // and the number obtained from that algorithm is more than the maximum, the element is suffering from an overflow.
        return number > maximum.value();
    }

    if (!maximum.has_value())
        return false;
    auto minimum = min();
    if (!minimum.has_value())
        return false;
    // https://html.spec.whatwg.org/multipage/input.html#the-min-and-max-attributes%3Asuffering-from-an-underflow-3
    // When an element has a reversed range, and the number obtained from that algorithm is more than the maximum and
    // less than the minimum, the element is simultaneously suffering from an underflow and suffering from an overflow.
    return has_reversed_range() && number > maximum.value() && number < minimum.value();
}

// https://html.spec.whatwg.org/multipage/input.html#the-step-attribute%3Asuffering-from-a-step-mismatch
bool HTMLInputElement::suffering_from_a_step_mismatch() const
{
    // When the element has an allowed value step,
    auto maybe_allowed_value_step = allowed_value_step();
    if (!maybe_allowed_value_step.has_value())
        return false;
    double allowed_value_step = *maybe_allowed_value_step;
    // and the result of applying the algorithm to convert a string to a number to the string given by the element's
    // value is a number,
    auto maybe_number = convert_string_to_number(value());
    if (!maybe_number.has_value())
        return false;
    double number = maybe_number.value();
    // and that number subtracted from the step base is not an integral multiple of the allowed value step, the element
    // is suffering from a step mismatch.
    return fmod(step_base() - number, allowed_value_step) != 0;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#suffering-from-bad-input
bool HTMLInputElement::suffering_from_bad_input() const
{
    switch (type_state()) {
    case TypeAttributeState::Email:
        // https://html.spec.whatwg.org/multipage/input.html#email-state-(type%3Demail)%3Asuffering-from-bad-input
        // While the user interface is representing input that the user agent cannot convert to punycode, the control is suffering from bad input.
        // FIXME: Implement this.

        // https://html.spec.whatwg.org/multipage/input.html#email-state-(type%3Demail)%3Asuffering-from-bad-input-2
        // While the user interface describes a situation where an individual value contains a U+002C COMMA (,) or is representing input that the user agent
        // cannot convert to punycode, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::Date:
        // https://html.spec.whatwg.org/multipage/input.html#date-state-(type%3Ddate)%3Asuffering-from-bad-input
        // While the user interface describes input that the user agent cannot convert to a valid date string, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::Month:
        // https://html.spec.whatwg.org/multipage/input.html#month-state-(type%3Dmonth)%3Asuffering-from-bad-input
        // While the user interface describes input that the user agent cannot convert to a valid month string, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::Week:
        // https://html.spec.whatwg.org/multipage/input.html#week-state-(type%3Dweek)%3Asuffering-from-bad-input
        // While the user interface describes input that the user agent cannot convert to a valid week string, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::Time:
        // https://html.spec.whatwg.org/multipage/#time-state-(type=time):suffering-from-bad-input
        // While the user interface describes input that the user agent cannot convert to a valid time string, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::LocalDateAndTime:
        // https://html.spec.whatwg.org/multipage/input.html#local-date-and-time-state-(type%3Ddatetime-local)%3Asuffering-from-bad-input
        // While the user interface describes input that the user agent cannot convert to a valid normalized local date and time string, the control is suffering from bad
        // input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::Number:
        // https://html.spec.whatwg.org/multipage/input.html#number-state-(type%3Dnumber)%3Asuffering-from-bad-input
        // While the user interface describes input that the user agent cannot convert to a valid floating-point number, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::Range:
        // https://html.spec.whatwg.org/multipage/input.html#range-state-(type%3Drange)%3Asuffering-from-bad-input
        // While the user interface describes input that the user agent cannot convert to a valid floating-point number, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    case TypeAttributeState::Color:
        // https://html.spec.whatwg.org/multipage/input.html#color-state-(type%3Dcolor)%3Asuffering-from-bad-input
        // While the element's value is not the empty string and parsing it returns failure, the control is suffering from bad input.
        // FIXME: Implement this.
        break;
    default:
        break;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/input.html#input-support-picker
bool HTMLInputElement::supports_a_picker() const
{
    // The input element can support a picker. A picker is a user interface element that allows the end user to choose a value.
    // Whether an input element supports a picker depends on the type attribute state and implementation-defined behavior.
    // An input element must support a picker when its type attribute is in the File Upload state.
    return first_is_one_of(type_state(), TypeAttributeState::FileUpload, TypeAttributeState::Color);
}

void HTMLInputElement::set_is_open(bool is_open)
{
    if (is_open == m_is_open)
        return;

    m_is_open = is_open;
    invalidate_style(DOM::StyleInvalidationReason::HTMLInputElementSetIsOpen);
}

}
