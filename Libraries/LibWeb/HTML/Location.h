/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class Location final : public Bindings::Wrappable {
    WEB_WRAPPABLE(Location, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Location);

public:
    virtual ~Location() override;

    WebIDL::ExceptionOr<String> href(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_href(JS::Realm&, String const&);

    WebIDL::ExceptionOr<String> origin(JS::Realm&) const;

    WebIDL::ExceptionOr<String> protocol(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_protocol(JS::Realm&, String const&);

    WebIDL::ExceptionOr<String> host(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_host(JS::Realm&, String const&);

    WebIDL::ExceptionOr<String> hostname(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_hostname(JS::Realm&, String const&);

    WebIDL::ExceptionOr<String> port(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_port(JS::Realm&, String const&);

    WebIDL::ExceptionOr<String> pathname(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_pathname(JS::Realm&, String const&);

    WebIDL::ExceptionOr<String> search(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_search(JS::Realm&, String const&);

    WebIDL::ExceptionOr<String> hash(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_hash(JS::Realm&, StringView);

    WebIDL::ExceptionOr<void> replace(JS::Realm&, String const& url);
    void reload() const;
    WebIDL::ExceptionOr<void> assign(JS::Realm&, String const& url);

private:
    explicit Location(Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<DOM::Document> relevant_document() const;
    URL::URL url() const;
    WebIDL::ExceptionOr<void> navigate(URL::URL, Bindings::NavigationHistoryBehavior = Bindings::NavigationHistoryBehavior::Auto);

    GC::Ref<Window> m_window;
};

}
