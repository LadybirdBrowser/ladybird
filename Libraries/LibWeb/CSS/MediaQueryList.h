/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::CSS {

// 4.2. The MediaQueryList Interface, https://drafts.csswg.org/cssom-view/#the-mediaquerylist-interface
class MediaQueryList final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaQueryList, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(MediaQueryList);

public:
    [[nodiscard]] static JS::NonnullGCPtr<MediaQueryList> create(DOM::Document&, Vector<NonnullRefPtr<MediaQuery>>&&);

    virtual ~MediaQueryList() override = default;

    String media() const;
    bool matches() const;
    bool evaluate();

    void add_listener(JS::GCPtr<DOM::IDLEventListener>);
    void remove_listener(JS::GCPtr<DOM::IDLEventListener>);

    void set_onchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onchange();

private:
    MediaQueryList(DOM::Document&, Vector<NonnullRefPtr<MediaQuery>>&&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<DOM::Document> m_document;
    Vector<NonnullRefPtr<MediaQuery>> m_media;
};

}
