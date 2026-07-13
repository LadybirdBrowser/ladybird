/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/ToggleTaskTracker.h>

namespace Web::HTML {

class HTMLDialogElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLDialogElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLDialogElement);

public:
    virtual ~HTMLDialogElement() override;

    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;

    // ^EventTarget
    virtual bool is_focusable() const override
    {
        return meets_focusable_area_rendering_requirements();
    }

    Utf16String return_value() const;
    void set_return_value(Utf16String);

    static WebIDL::ExceptionOr<void> show_a_modal_dialog(HTMLDialogElement&, GC::Ptr<DOM::Element> source);

    void close_the_dialog(Optional<Utf16String> result, GC::Ptr<DOM::Element> source);
    void request_close_the_dialog(Optional<Utf16String> return_value, GC::Ptr<DOM::Element> source);

    WebIDL::ExceptionOr<void> show();
    WebIDL::ExceptionOr<void> show_modal();
    void close(Optional<Utf16String> return_value);
    void request_close(Optional<Utf16String> return_value);

    static void light_dismiss_open_dialogs(UIEvents::PointerEvent const&, GC::Ptr<DOM::Node>);

    // https://www.w3.org/TR/html-aria/#el-dialog
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::dialog; }

    bool is_modal() const { return m_is_modal; }
    void set_is_modal(bool);

    bool is_valid_command(Utf16View) override;
    void command_steps(DOM::Element&, Utf16View) override;

private:
    HTMLDialogElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void inserted() override;
    virtual void attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    void queue_a_dialog_toggle_event_task(Utf16FlyString old_state, Utf16FlyString new_state, GC::Ptr<DOM::Element> source);

    void run_dialog_setup_steps();
    void run_dialog_cleanup_steps();
    void run_dialog_focusing_steps();

    void set_close_watcher();

    static GC::Ptr<HTMLDialogElement> nearest_clicked_dialog(UIEvents::PointerEvent const&, GC::Ptr<DOM::Node>);

    Utf16String m_return_value;
    bool m_is_modal { false };
    Optional<Utf16String> m_request_close_return_value;
    GC::Ptr<DOM::Element> m_request_close_source_element;
    GC::Ptr<CloseWatcher> m_close_watcher;

    // https://html.spec.whatwg.org/multipage/interactive-elements.html#enable-close-watcher-for-requestclose()
    bool m_enable_close_watcher_for_request_close { false };

    // https://html.spec.whatwg.org/multipage/interactive-elements.html#dialog-toggle-task-tracker
    Optional<ToggleTaskTracker> m_dialog_toggle_task_tracker;

    // https://html.spec.whatwg.org/multipage/interactive-elements.html#previously-focused-element
    GC::Ptr<Node> m_previously_focused_element;
};

}
