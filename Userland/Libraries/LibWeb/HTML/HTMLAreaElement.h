/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLHyperlinkElementUtils.h>

namespace Web::HTML {

class HTMLAreaElement final
    : public HTMLElement
    , public HTMLHyperlinkElementUtils {
    WEB_PLATFORM_OBJECT(HTMLAreaElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLAreaElement);

public:
    virtual ~HTMLAreaElement() override;
    JS::NonnullGCPtr<DOM::DOMTokenList> rel_list();

private:
    HTMLAreaElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^DOM::Element
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;
    virtual i32 default_tab_index_value() const override;

    // ^HTML::HTMLHyperlinkElementUtils
    virtual DOM::Document& hyperlink_element_utils_document() override { return document(); }
    virtual Optional<String> hyperlink_element_utils_href() const override;
    virtual WebIDL::ExceptionOr<void> set_hyperlink_element_utils_href(String) override;
    virtual Optional<String> hyperlink_element_utils_referrerpolicy() const override;
    virtual bool hyperlink_element_utils_is_html_anchor_element() const override { return false; }
    virtual bool hyperlink_element_utils_is_connected() const override { return is_connected(); }
    virtual void hyperlink_element_utils_queue_an_element_task(HTML::Task::Source source, Function<void()> steps) override
    {
        queue_an_element_task(source, move(steps));
    }
    virtual String hyperlink_element_utils_get_an_elements_target() const override
    {
        return get_an_elements_target();
    }
    virtual TokenizedFeature::NoOpener hyperlink_element_utils_get_an_elements_noopener(StringView target) const override
    {
        return get_an_elements_noopener(target);
    }

    virtual Optional<ARIA::Role> default_role() const override;

    JS::GCPtr<DOM::DOMTokenList> m_rel_list;
};

}
