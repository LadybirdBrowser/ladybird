/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class WEB_API HTMLDataListElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLDataListElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLDataListElement);

public:
    virtual ~HTMLDataListElement() override;

    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::listbox; }

    GC::Ref<DOM::HTMLCollection> options();

    void notify_inputs_using_this_as_suggestions_source();

private:
    HTMLDataListElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_html_data_list_element() const override { return true; }

    virtual void inserted() override;
    virtual void children_changed(ChildrenChangedMetadata const&) override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    void notify_inputs_with_suggestions_sources_in(DOM::Node& root);

    GC::Ptr<DOM::HTMLCollection> m_options;
    bool m_input_notification_queued { false };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLDataListElement>() const { return is_html_data_list_element(); }

}
