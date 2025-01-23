/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/ToggleTaskTracker.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class HTMLDetailsElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLDetailsElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLDetailsElement);

public:
    virtual ~HTMLDetailsElement() override;

    // https://www.w3.org/TR/html-aria/#el-details
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::group; }

private:
    HTMLDetailsElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void inserted() override;
    virtual void removed_from(DOM::Node* old_parent, DOM::Node& old_root) override;
    virtual void children_changed() override;
    virtual void attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    void queue_a_details_toggle_event_task(String old_state, String new_state);
    void ensure_details_exclusivity_by_closing_other_elements_if_needed();
    void ensure_details_exclusivity_by_closing_the_given_element_if_needed();

    WebIDL::ExceptionOr<void> create_shadow_tree_if_needed();
    void update_shadow_tree_slots();
    void update_shadow_tree_style();

    // https://html.spec.whatwg.org/multipage/interactive-elements.html#details-toggle-task-tracker
    Optional<ToggleTaskTracker> m_details_toggle_task_tracker;

    GC::Ptr<HTML::HTMLSlotElement> m_summary_slot;
    GC::Ptr<HTML::HTMLSlotElement> m_descendants_slot;
};

}
