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

    WebIDL::ExceptionOr<String> href() const;
    WebIDL::ExceptionOr<void> set_href(String const&);

    WebIDL::ExceptionOr<String> origin() const;

    WebIDL::ExceptionOr<String> protocol() const;
    WebIDL::ExceptionOr<void> set_protocol(String const&);

    WebIDL::ExceptionOr<String> host() const;
    WebIDL::ExceptionOr<void> set_host(String const&);

    WebIDL::ExceptionOr<String> hostname() const;
    WebIDL::ExceptionOr<void> set_hostname(String const&);

    WebIDL::ExceptionOr<String> port() const;
    WebIDL::ExceptionOr<void> set_port(String const&);

    WebIDL::ExceptionOr<String> pathname() const;
    WebIDL::ExceptionOr<void> set_pathname(String const&);

    WebIDL::ExceptionOr<String> search() const;
    WebIDL::ExceptionOr<void> set_search(String const&);

    WebIDL::ExceptionOr<String> hash() const;
    WebIDL::ExceptionOr<void> set_hash(StringView);

    WebIDL::ExceptionOr<void> replace(String const& url);
    void reload() const;
    WebIDL::ExceptionOr<void> assign(String const& url);

private:
    explicit Location(JS::Realm&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<DOM::Document> relevant_document() const;
    URL::URL url() const;
    WebIDL::ExceptionOr<void> navigate(URL::URL, Bindings::NavigationHistoryBehavior = Bindings::NavigationHistoryBehavior::Auto);
};

}
