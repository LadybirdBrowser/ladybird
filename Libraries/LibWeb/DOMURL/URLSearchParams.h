/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOMURL {

struct QueryParam {
    String name;
    String value;
};
String url_encode(Vector<QueryParam> const&, StringView encoding = "UTF-8"sv);
Vector<QueryParam> url_decode(StringView);

class URLSearchParams : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(URLSearchParams, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(URLSearchParams);

public:
    static JS::NonnullGCPtr<URLSearchParams> create(JS::Realm&, StringView);
    static JS::NonnullGCPtr<URLSearchParams> create(JS::Realm&, Vector<QueryParam> list);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<URLSearchParams>> construct_impl(JS::Realm&, Variant<Vector<Vector<String>>, OrderedHashMap<String, String>, String> const& init);

    virtual ~URLSearchParams() override;

    size_t size() const;
    void append(String const& name, String const& value);
    void delete_(String const& name, Optional<String> const& value = {});
    Optional<String> get(String const& name);
    Vector<String> get_all(String const& name);
    bool has(String const& name, Optional<String> const& value = {});
    void set(String const& name, String const& value);

    void sort();

    String to_string() const;

    using ForEachCallback = Function<JS::ThrowCompletionOr<void>(String const&, String const&)>;
    JS::ThrowCompletionOr<void> for_each(ForEachCallback);

private:
    friend class DOMURL;
    friend class URLSearchParamsIterator;

    URLSearchParams(JS::Realm&, Vector<QueryParam> list);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void update();

    Vector<QueryParam> m_list;
    JS::GCPtr<DOMURL> m_url;
};

}
