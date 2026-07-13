/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibWeb/Bindings/HTMLElement.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/HTMLOrSVGOrMathMLElement.h>
#include <LibWeb/HTML/ToggleTaskTracker.h>
#include <LibWeb/HTML/TokenizedFeatures.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#attr-contenteditable
enum class ContentEditableState : u8 {
    True,
    False,
    PlaintextOnly,
    Inherit,
};

using TogglePopoverOptionsOrForceBoolean = Variant<Bindings::TogglePopoverOptions, bool>;

enum class ThrowExceptions {
    Yes,
    No,
};

enum class FocusPreviousElement {
    Yes,
    No,
};

enum class FireEvents {
    Yes,
    No,
};

enum class ExpectedToBeShowing {
    Yes,
    No,
};

enum class IgnoreDomState {
    Yes,
    No,
};

enum class IsPopover {
    Yes,
    No,
};

class WEB_API HTMLElement
    : public DOM::Element
    , public HTML::GlobalEventHandlers
    , public HTML::HTMLOrSVGOrMathMLElement<HTMLElement>
    , public FormAssociatedElement {
    WEB_PLATFORM_OBJECT(HTMLElement, DOM::Element);
    GC_DECLARE_ALLOCATOR(HTMLElement);

public:
    virtual ~HTMLElement() override;

    Optional<Utf16String> title() const { return attribute(HTML::AttributeNames::title); }

    bool translate() const;
    void set_translate(bool);

    Utf16FlyString dir() const;
    void set_dir(Utf16View);

    virtual bool is_focusable() const override;
    bool is_content_editable() const;
    Utf16FlyString content_editable() const;
    ContentEditableState content_editable_state() const { return m_content_editable_state; }
    WebIDL::ExceptionOr<void> set_content_editable(Utf16FlyString const&);

    Utf16String inner_text();
    void set_inner_text(Utf16View const&);

    [[nodiscard]] Utf16String outer_text();
    WebIDL::ExceptionOr<void> set_outer_text(Utf16View const&);

    int offset_top() const;
    int offset_left() const;
    int offset_width() const;
    int offset_height() const;
    GC::Ptr<Element> offset_parent() const;
    GC::Ptr<Element> scroll_parent() const;

    Variant<bool, double, Utf16String, Empty> hidden() const;
    void set_hidden(Variant<bool, double, Utf16String, Empty> const&);

    void click();

    [[nodiscard]] Utf16String access_key_label() const;

    bool spellcheck() const;
    void set_spellcheck(bool);

    Utf16FlyString writing_suggestions() const;
    void set_writing_suggestions(Utf16View);

    enum class AutocapitalizationHint {
        Default,
        None,
        Sentences,
        Words,
        Characters
    };

    AutocapitalizationHint own_autocapitalization_hint() const;
    Utf16FlyString autocapitalize() const;
    void set_autocapitalize(Utf16View);

    enum class AutocorrectionState {
        On,
        Off
    };

    AutocorrectionState used_autocorrection_state() const;
    bool autocorrect() const;
    void set_autocorrect(bool);

    bool fire_a_synthetic_pointer_event(Utf16FlyString const& type, DOM::Element& target, bool not_trusted);

    // https://html.spec.whatwg.org/multipage/forms.html#category-label
    virtual bool is_labelable() const { return is_form_associated_custom_element(); }

    GC::Ptr<DOM::NodeList> labels();

    virtual Optional<ARIA::Role> default_role() const override;

    WebIDL::ExceptionOr<GC::Ref<ElementInternals>> attach_internals();

    void set_popover(Optional<Utf16String> value);
    Optional<Utf16FlyString> popover() const;
    Optional<Utf16FlyString> opened_in_popover_mode() const { return m_opened_in_popover_mode; }

    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<DOM::Node> old_ancestor) override;

    enum class PopoverVisibilityState : u8 {
        Hidden,
        Showing,
    };
    PopoverVisibilityState popover_visibility_state() const { return m_popover_visibility_state; }

    WebIDL::ExceptionOr<void> show_popover_for_bindings(Bindings::ShowPopoverOptions const& = {});
    WebIDL::ExceptionOr<void> hide_popover_for_bindings();
    WebIDL::ExceptionOr<bool> toggle_popover(TogglePopoverOptionsOrForceBoolean const&);

    WebIDL::ExceptionOr<bool> check_popover_validity(ExpectedToBeShowing expected_to_be_showing, ThrowExceptions throw_exceptions, GC::Ptr<DOM::Document>, IgnoreDomState ignore_dom_state);
    WebIDL::ExceptionOr<void> show_popover(ThrowExceptions throw_exceptions, GC::Ptr<HTMLElement> source);
    WebIDL::ExceptionOr<void> hide_popover(FocusPreviousElement focus_previous_element, FireEvents fire_events, ThrowExceptions throw_exceptions, IgnoreDomState ignore_dom_state, GC::Ptr<HTMLElement> source);

    static void hide_all_popovers_until(Variant<GC::Ptr<HTMLElement>, GC::Ptr<DOM::Document>> endpoint, FocusPreviousElement focus_previous_element, FireEvents fire_events);
    static GC::Ptr<HTMLElement> topmost_popover_ancestor(GC::Ptr<DOM::Node> new_popover_or_top_layer_element, Vector<GC::Ref<HTMLElement>> const& popover_list, GC::Ptr<HTMLElement> source, IsPopover is_popover);

    static void light_dismiss_open_popovers(UIEvents::PointerEvent const&, GC::Ptr<DOM::Node>);

    bool is_inert() const { return m_inert; }

    bool draggable() const;
    void set_draggable(bool draggable);

    virtual bool is_valid_command(Utf16View) { return false; }
    virtual void command_steps(DOM::Element&, Utf16View) { }

    bool is_form_associated_custom_element() const;

    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    virtual bool uses_button_layout() const { return false; }

    WebIDL::UnsignedLong computed_heading_level() const;
    WebIDL::UnsignedLong computed_heading_offset() const;

protected:
    HTMLElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) const override;
    virtual void inserted() override;

    virtual void visit_edges(Cell::Visitor&) override;

    void set_inert(bool inert) { m_inert = inert; }
    void set_subtree_inertness(bool is_inert);

    [[nodiscard]] Utf16String get_the_text_steps();

    virtual void adjust_computed_style(CSS::ComputedProperties::Builder&) override;

private:
    virtual bool is_html_element() const final { return true; }

    // ^FormAssociatedElement
    virtual HTMLElement& form_associated_element_to_html_element() override { return *this; }

    // ^HTML::GlobalEventHandlers
    virtual GC::Ptr<DOM::EventTarget> global_event_handlers_to_event_target(Utf16FlyString const&) override { return *this; }
    virtual void did_receive_focus() override;
    virtual void did_lose_focus() override;

    GC::Ref<DOM::DocumentFragment> rendered_text_fragment(Utf16View const& input);

    GC::Ptr<DOM::NodeList> m_labels;

    void queue_a_popover_toggle_event_task(Utf16FlyString old_state, Utf16FlyString new_state, GC::Ptr<HTMLElement> source);

    static Optional<Utf16FlyString> popover_value_to_state(Optional<Utf16View> value);
    void hide_popover_stack_until(Vector<GC::Ref<HTMLElement>> const& popover_list, FocusPreviousElement focus_previous_element, FireEvents fire_events);
    GC::Ptr<HTMLElement> nearest_inclusive_open_popover();
    GC::Ptr<HTMLElement> nearest_inclusive_target_popover();
    static void close_entire_popover_list(Vector<GC::Ref<HTMLElement>> const& popover_list, FocusPreviousElement focus_previous_element, FireEvents fire_events);
    static GC::Ptr<HTMLElement> topmost_clicked_popover(GC::Ptr<DOM::Node> node);
    size_t popover_stack_position();

    // https://html.spec.whatwg.org/multipage/custom-elements.html#attached-internals
    GC::Ptr<ElementInternals> m_attached_internals;

    // https://html.spec.whatwg.org/multipage/interaction.html#attr-contenteditable
    ContentEditableState m_content_editable_state { ContentEditableState::Inherit };

    // https://html.spec.whatwg.org/multipage/interaction.html#click-in-progress-flag
    bool m_click_in_progress { false };

    bool m_inert { false };

    // Popover API

    // https://html.spec.whatwg.org/multipage/popover.html#popover-visibility-state
    PopoverVisibilityState m_popover_visibility_state { PopoverVisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/popover.html#popover-showing-or-hiding
    bool m_popover_showing_or_hiding { false };

    // https://html.spec.whatwg.org/multipage/popover.html#popover-trigger
    GC::Ptr<HTMLElement> m_popover_trigger;

    // https://html.spec.whatwg.org/multipage/popover.html#the-popover-attribute:toggle-task-tracker
    Optional<ToggleTaskTracker> m_popover_toggle_task_tracker;

    // https://html.spec.whatwg.org/multipage/popover.html#popover-close-watcher
    GC::Ptr<CloseWatcher> m_popover_close_watcher;

    Optional<Utf16FlyString> m_opened_in_popover_mode;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLElement>() const { return is_html_element(); }

}

namespace JS {

template<>
inline bool Object::fast_is<Web::HTML::HTMLElement>() const
{
    return is_dom_node() && static_cast<Web::DOM::Node const&>(*this).is_html_element();
}

}
