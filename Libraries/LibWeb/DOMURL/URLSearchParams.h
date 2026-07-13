/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOMURL {

struct QueryParam {
    Utf16String name;
    Utf16String value;
};
String url_encode(Vector<QueryParam> const&, StringView encoding = "UTF-8"sv);
Vector<QueryParam> url_decode(StringView);
Vector<QueryParam> url_decode(Utf16View);

class URLSearchParams : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(URLSearchParams, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(URLSearchParams);

public:
    static GC::Ref<URLSearchParams> create(JS::Realm&, Utf16View);
    static GC::Ref<URLSearchParams> create(JS::Realm&, Vector<QueryParam> list);
    static WebIDL::ExceptionOr<GC::Ref<URLSearchParams>> construct_impl(JS::Realm&, Variant<Vector<Vector<Utf16String>>, OrderedHashMap<Utf16String, Utf16String>, Utf16String> const& init);

    virtual ~URLSearchParams() override;

    size_t size() const;
    void append(Utf16String const& name, Utf16String const& value);
    void delete_(Utf16String const& name, Optional<Utf16String> const& value = {});
    Optional<Utf16String> get(Utf16String const& name);
    Vector<Utf16String> get_all(Utf16String const& name);
    bool has(Utf16String const& name, Optional<Utf16String> const& value = {});
    void set(Utf16String const& name, Utf16String const& value);

    void sort();

    String serialize_to_byte_string() const;
    Utf16String to_string() const;

    using ForEachCallback = Function<JS::ThrowCompletionOr<void>(Utf16String const&, Utf16String const&)>;
    JS::ThrowCompletionOr<void> for_each(ForEachCallback);

private:
    friend class DOMURL;
    friend class URLSearchParamsIterator;

    static GC::Ref<URLSearchParams> create_from_byte_string(JS::Realm&, StringView);

    URLSearchParams(JS::Realm&, Vector<QueryParam> list);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void update();

    Vector<QueryParam> m_list;
    GC::Ptr<DOMURL> m_url;
};

}
