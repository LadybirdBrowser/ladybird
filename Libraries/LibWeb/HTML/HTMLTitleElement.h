/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLTitleElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTitleElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLTitleElement);

public:
    virtual ~HTMLTitleElement() override;

    String text() const;
    void set_text(String const& value);

private:
    HTMLTitleElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void children_changed(ChildrenChangedMetadata const*) override;

    void consider_propagate_title_update();
    void propagate_title_update();

    i64 m_first_block_at_ms { 0 };
    RefPtr<Core::Timer> m_throttle_update_timer;
};

}
