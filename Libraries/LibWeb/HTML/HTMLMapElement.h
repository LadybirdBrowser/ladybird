/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLMapElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLMapElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLMapElement);

public:
    virtual ~HTMLMapElement() override;

    GC::Ref<DOM::HTMLCollection> areas();

private:
    HTMLMapElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<DOM::HTMLCollection> m_areas;
};

}
