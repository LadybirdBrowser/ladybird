/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class MediaQueryList;

}

namespace Web::Bindings {

class WrapperWorld;
WEB_API JS::Realm& wrapper_realm_for_media_query_list(WrapperWorld const&, JS::Realm&, CSS::MediaQueryList&);

}

namespace Web::CSS {

// 4.2. The MediaQueryList Interface, https://drafts.csswg.org/cssom-view/#the-mediaquerylist-interface
class MediaQueryList final : public DOM::EventTarget {
    WEB_WRAPPABLE(MediaQueryList, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaQueryList);

public:
    [[nodiscard]] static GC::Ref<MediaQueryList> create(DOM::Document&, Vector<NonnullRefPtr<MediaQuery>>&&);

    virtual ~MediaQueryList() override = default;

    String media() const;
    bool matches() const;
    bool evaluate();

    void add_listener(GC::Ptr<DOM::IDLEventListener>);
    void remove_listener(GC::Ptr<DOM::IDLEventListener>);

    void set_onchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onchange();

    [[nodiscard]] Optional<bool> const& has_changed_state() const { return m_has_changed_state; }
    void set_has_changed_state(bool has_changed_state) { m_has_changed_state = has_changed_state; }

private:
    friend JS::Realm& Bindings::wrapper_realm_for_media_query_list(Bindings::WrapperWorld const&, JS::Realm&, MediaQueryList&);

    MediaQueryList(DOM::Document&, Vector<NonnullRefPtr<MediaQuery>>&&);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Document> m_document;
    Vector<NonnullRefPtr<MediaQuery>> m_media;

    mutable Optional<bool> m_has_changed_state { false };
};

}
