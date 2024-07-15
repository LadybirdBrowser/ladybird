/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Adam Hodgen <ant1441@gmail.com>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/ColorPickerUpdateState.h>
#include <LibWeb/HTML/FileFilter.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/input.html#attr-input-type
#define ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTES                                  \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(hidden, Hidden)                     \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(text, Text)                         \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(search, Search)                     \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(tel, Telephone)                     \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(url, URL)                           \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(email, Email)                       \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(password, Password)                 \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(date, Date)                         \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(month, Month)                       \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(week, Week)                         \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(time, Time)                         \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE("datetime-local", LocalDateAndTime) \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(number, Number)                     \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(range, Range)                       \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(color, Color)                       \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(checkbox, Checkbox)                 \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(radio, RadioButton)                 \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(file, FileUpload)                   \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(submit, SubmitButton)               \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(image, ImageButton)                 \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(reset, ResetButton)                 \
    __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(button, Button)

class HTMLInputElement final
    : public HTMLElement
    , public FormAssociatedElement
    , public DOM::EditableTextNodeOwner
    , public Layout::ImageProvider {
    WEB_PLATFORM_OBJECT(HTMLInputElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLInputElement);
    FORM_ASSOCIATED_ELEMENT(HTMLElement, HTMLInputElement)

public:
    virtual ~HTMLInputElement() override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;
    virtual void adjust_computed_style(CSS::StyleProperties&) override;

    enum class TypeAttributeState {
#define __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE(_, state) state,
        ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTES
#undef __ENUMERATE_HTML_INPUT_TYPE_ATTRIBUTE
    };

    StringView type() const;
    TypeAttributeState type_state() const { return m_type; }
    WebIDL::ExceptionOr<void> set_type(String const&);

    String default_value() const { return get_attribute_value(HTML::AttributeNames::value); }

    virtual String value() const override;
    WebIDL::ExceptionOr<void> set_value(String const&);

    void commit_pending_changes();

    String placeholder() const;
    Optional<String> placeholder_value() const;

    bool checked() const { return m_checked; }
    enum class ChangeSource {
        Programmatic,
        User,
    };
    void set_checked(bool, ChangeSource = ChangeSource::Programmatic);

    bool checked_binding() const { return checked(); }
    void set_checked_binding(bool);

    bool indeterminate() const { return m_indeterminate; }
    void set_indeterminate(bool);

    bool is_mutable() const { return m_is_mutable; }

    void did_pick_color(Optional<Color> picked_color, ColorPickerUpdateState state);

    void did_select_files(Span<SelectedFile> selected_files);

    JS::GCPtr<FileAPI::FileList> files();
    void set_files(JS::GCPtr<FileAPI::FileList>);

    FileFilter parse_accept_attribute() const;

    // NOTE: User interaction
    // https://html.spec.whatwg.org/multipage/input.html#update-the-file-selection
    void update_the_file_selection(JS::NonnullGCPtr<FileAPI::FileList>);

    WebIDL::Long max_length() const;
    WebIDL::ExceptionOr<void> set_max_length(WebIDL::Long);

    WebIDL::Long min_length() const;
    WebIDL::ExceptionOr<void> set_min_length(WebIDL::Long);

    unsigned size() const;
    WebIDL::ExceptionOr<void> set_size(unsigned value);

    struct SelectedCoordinate {
        int x { 0 };
        int y { 0 };
    };
    SelectedCoordinate selected_coordinate() const { return m_selected_coordinate; }

    JS::Object* value_as_date() const;
    WebIDL::ExceptionOr<void> set_value_as_date(Optional<JS::Handle<JS::Object>> const&);

    double value_as_number() const;
    WebIDL::ExceptionOr<void> set_value_as_number(double value);

    WebIDL::ExceptionOr<void> step_up(WebIDL::Long n = 1);
    WebIDL::ExceptionOr<void> step_down(WebIDL::Long n = 1);

    WebIDL::ExceptionOr<bool> check_validity();
    WebIDL::ExceptionOr<bool> report_validity();
    void set_custom_validity(String const&);

    WebIDL::ExceptionOr<void> select();
    WebIDL::ExceptionOr<void> set_selection_range(u32 start, u32 end, Optional<String> const& direction = {});

    WebIDL::ExceptionOr<void> show_picker();

    // ^DOM::EditableTextNodeOwner
    virtual void did_edit_text_node(Badge<Navigable>) override;

    // ^EventTarget
    // https://html.spec.whatwg.org/multipage/interaction.html#the-tabindex-attribute:the-input-element
    virtual bool is_focusable() const override { return m_type != TypeAttributeState::Hidden; }

    // ^FormAssociatedElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-submit
    virtual bool is_submittable() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-reset
    virtual bool is_resettable() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-autocapitalize
    virtual bool is_auto_capitalize_inheriting() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#concept-button
    virtual bool is_button() const override;

    // https://html.spec.whatwg.org/multipage/forms.html#concept-submit-button
    virtual bool is_submit_button() const override;

    bool is_single_line() const;

    virtual void reset_algorithm() override;

    virtual void form_associated_element_was_inserted() override;
    virtual void form_associated_element_was_removed(DOM::Node*) override;
    virtual void form_associated_element_attribute_changed(FlyString const&, Optional<String> const&) override;

    JS::NonnullGCPtr<ValidityState const> validity() const;

    // ^HTMLElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-label
    virtual bool is_labelable() const override { return type_state() != TypeAttributeState::Hidden; }

    virtual Optional<ARIA::Role> default_role() const override;

    JS::GCPtr<Element> placeholder_element() { return m_placeholder_element; }
    JS::GCPtr<Element const> placeholder_element() const { return m_placeholder_element; }

    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(DOM::Event const&) override;

    bool has_input_activation_behavior() const;
    bool change_event_applies() const;
    bool value_as_date_applies() const;
    bool value_as_number_applies() const;
    bool step_applies() const;
    bool step_up_or_down_applies() const;

private:
    HTMLInputElement(DOM::Document&, DOM::QualifiedName);

    // ^DOM::Node
    virtual bool is_html_input_element() const final { return true; }

    // ^DOM::EventTarget
    virtual void did_lose_focus() override;
    virtual void did_receive_focus() override;
    virtual void legacy_pre_activation_behavior() override;
    virtual void legacy_cancelled_activation_behavior() override;
    virtual void legacy_cancelled_activation_behavior_was_not_called() override;

    // ^DOM::Element
    virtual i32 default_tab_index_value() const override;

    // https://html.spec.whatwg.org/multipage/input.html#image-button-state-(type=image):dimension-attributes
    virtual bool supports_dimension_attributes() const override { return type_state() == TypeAttributeState::ImageButton; }

    // ^Layout::ImageProvider
    virtual bool is_image_available() const override;
    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;
    virtual RefPtr<Gfx::ImmutableBitmap> current_image_bitmap(Gfx::IntSize = {}) const override;
    virtual void set_visible_in_viewport(bool) override;
    virtual JS::NonnullGCPtr<DOM::Element const> to_html_element() const override { return *this; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Optional<double> convert_string_to_number(StringView input) const;
    String convert_number_to_string(double input) const;

    WebIDL::ExceptionOr<JS::GCPtr<JS::Date>> convert_string_to_date(StringView input) const;
    String covert_date_to_string(JS::NonnullGCPtr<JS::Date> input) const;

    Optional<double> min() const;
    Optional<double> max() const;
    double default_step() const;
    double step_scale_factor() const;
    Optional<double> allowed_value_step() const;
    double step_base() const;
    WebIDL::ExceptionOr<void> step_up_or_down(bool is_down, WebIDL::Long n);

    static TypeAttributeState parse_type_attribute(StringView);
    void create_shadow_tree_if_needed();
    void update_shadow_tree();
    void create_button_input_shadow_tree();
    void create_text_input_shadow_tree();
    void create_color_input_shadow_tree();
    void create_file_input_shadow_tree();
    void create_range_input_shadow_tree();
    WebIDL::ExceptionOr<void> run_input_activation_behavior(DOM::Event const&);
    void set_checked_within_group();

    void handle_maxlength_attribute();
    void handle_readonly_attribute(Optional<String> const& value);
    WebIDL::ExceptionOr<void> handle_src_attribute(String const& value);

    void user_interaction_did_change_input_value();

    // https://html.spec.whatwg.org/multipage/input.html#value-sanitization-algorithm
    String value_sanitization_algorithm(String const&) const;

    enum class ValueAttributeMode {
        Value,
        Default,
        DefaultOn,
        Filename,
    };
    ValueAttributeMode value_attribute_mode() const;

    void update_placeholder_visibility();
    JS::GCPtr<DOM::Element> m_placeholder_element;
    JS::GCPtr<DOM::Text> m_placeholder_text_node;

    void update_text_input_shadow_tree();
    JS::GCPtr<DOM::Element> m_inner_text_element;
    JS::GCPtr<DOM::Text> m_text_node;
    bool m_checked { false };

    void update_color_well_element();
    JS::GCPtr<DOM::Element> m_color_well_element;

    void update_file_input_shadow_tree();
    JS::GCPtr<DOM::Element> m_file_button;
    JS::GCPtr<DOM::Element> m_file_label;

    void update_slider_shadow_tree_elements();
    JS::GCPtr<DOM::Element> m_slider_thumb;
    JS::GCPtr<DOM::Element> m_slider_progress_element;

    JS::GCPtr<DecodedImageData> image_data() const;
    JS::GCPtr<SharedImageRequest> m_image_request;
    SelectedCoordinate m_selected_coordinate;

    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;

    // https://html.spec.whatwg.org/multipage/input.html#dom-input-indeterminate
    bool m_indeterminate { false };

    // https://html.spec.whatwg.org/multipage/input.html#concept-input-checked-dirty-flag
    bool m_dirty_checkedness { false };

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fe-dirty
    bool m_dirty_value { false };

    // https://html.spec.whatwg.org/multipage/input.html#the-input-element:concept-fe-mutable
    bool m_is_mutable { true };

    // https://html.spec.whatwg.org/multipage/input.html#the-input-element:legacy-pre-activation-behavior
    bool m_before_legacy_pre_activation_behavior_checked { false };
    bool m_before_legacy_pre_activation_behavior_indeterminate { false };
    JS::GCPtr<HTMLInputElement> m_legacy_pre_activation_behavior_checked_element_in_group;

    // https://html.spec.whatwg.org/multipage/input.html#concept-input-type-file-selected
    JS::GCPtr<FileAPI::FileList> m_selected_files;

    TypeAttributeState m_type { TypeAttributeState::Text };
    String m_value;

    String m_last_src_value;

    bool m_has_uncommitted_changes { false };
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLInputElement>() const { return is_html_input_element(); }
}
