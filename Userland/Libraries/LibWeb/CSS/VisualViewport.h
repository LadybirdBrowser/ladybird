/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom-view/#visualviewport
class VisualViewport final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(VisualViewport, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(VisualViewport);

public:
    [[nodiscard]] static JS::NonnullGCPtr<VisualViewport> create(DOM::Document&);

    virtual ~VisualViewport() override = default;

    [[nodiscard]] double offset_left() const;
    [[nodiscard]] double offset_top() const;

    [[nodiscard]] double page_left() const;
    [[nodiscard]] double page_top() const;

    [[nodiscard]] double width() const;
    [[nodiscard]] double height() const;

    [[nodiscard]] double scale() const;

    void set_onresize(WebIDL::CallbackType*);
    WebIDL::CallbackType* onresize();
    void set_onscroll(WebIDL::CallbackType*);
    WebIDL::CallbackType* onscroll();
    void set_onscrollend(WebIDL::CallbackType*);
    WebIDL::CallbackType* onscrollend();

private:
    explicit VisualViewport(DOM::Document&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<DOM::Document> m_document;
};

}
