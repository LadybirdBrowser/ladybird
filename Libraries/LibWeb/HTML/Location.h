/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>

namespace Web::HTML {

class Location final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Location, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Location);

public:
    virtual ~Location() override;

    [[nodiscard]] Window& window() { return m_window; }
    [[nodiscard]] Window const& window() const { return m_window; }

    WebIDL::ExceptionOr<Utf16String> href() const;
    WebIDL::ExceptionOr<void> set_href(Utf16String const&);

    WebIDL::ExceptionOr<Utf16String> origin() const;

    WebIDL::ExceptionOr<Utf16String> protocol() const;
    WebIDL::ExceptionOr<void> set_protocol(Utf16String const&);

    WebIDL::ExceptionOr<Utf16String> host() const;
    WebIDL::ExceptionOr<void> set_host(Utf16String const&);

    WebIDL::ExceptionOr<Utf16String> hostname() const;
    WebIDL::ExceptionOr<void> set_hostname(Utf16String const&);

    WebIDL::ExceptionOr<Utf16String> port() const;
    WebIDL::ExceptionOr<void> set_port(Utf16String const&);

    WebIDL::ExceptionOr<Utf16String> pathname() const;
    WebIDL::ExceptionOr<void> set_pathname(Utf16String const&);

    WebIDL::ExceptionOr<Utf16String> search() const;
    WebIDL::ExceptionOr<void> set_search(Utf16String const&);

    WebIDL::ExceptionOr<Utf16String> hash() const;
    WebIDL::ExceptionOr<void> set_hash(Utf16String const&);

    WebIDL::ExceptionOr<void> replace(Utf16String const& url);
    void reload() const;
    WebIDL::ExceptionOr<void> assign(Utf16String const& url);

private:
    explicit Location(Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual GC::Ptr<Bindings::Wrappable> relevant_global_impl() const override;

    GC::Ptr<DOM::Document> relevant_document() const;
    URL::URL url() const;
    WebIDL::ExceptionOr<void> navigate(URL::URL, NavigationHistoryBehavior = NavigationHistoryBehavior::Auto);

    GC::Ref<Window> m_window;
};

}
