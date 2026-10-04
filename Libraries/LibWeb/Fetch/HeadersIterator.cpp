/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/Headers.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Fetch/HeadersIterator.h>

namespace Web::Fetch {

GC_DEFINE_ALLOCATOR(HeadersIterator);

static constexpr JSHostClass headers_iterator_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "HeadersIterator"sv, nullptr, nullptr, nullptr, 0);

HeadersIterator::HeadersIterator(Headers const& headers, JS::Object::PropertyKind iteration_kind)
    : m_headers(headers)
    , m_iteration_kind(iteration_kind)
{
}

HeadersIterator::~HeadersIterator() = default;

GC::Ref<JS::HostObject> HeadersIterator::create(JS::Realm& realm, Headers const& headers, JS::Object::PropertyKind iteration_kind)
{
    static auto const& prototype_name = "HeadersIterator"_utf16_fly_string;
    auto iterator = realm.create<HeadersIterator>(headers, iteration_kind);
    auto& prototype = Bindings::ensure_web_prototype<Bindings::HeadersIteratorPrototype>(realm, prototype_name);
    return JS::HostObject::create(realm, headers_iterator_host_class, prototype, {}, iterator);
}

void HeadersIterator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_headers);
}

// https://webidl.spec.whatwg.org/#es-iterable, Step 2
GC::Ref<JS::Object> HeadersIterator::next(JS::Realm& realm)
{
    // The value pairs to iterate over are the return value of running sort and combine with this’s header list.
    auto value_pairs_to_iterate_over = [&]() {
        return m_headers->m_header_list->sort_and_combine();
    };

    auto pairs = value_pairs_to_iterate_over();

    if (m_index >= pairs.size())
        return JS::create_iterator_result_object(realm, JS::js_undefined(), true);

    auto const& pair = pairs[m_index++];
    auto pair_name = TextCodec::isomorphic_decode(pair.name);
    auto pair_value = TextCodec::isomorphic_decode(pair.value);

    switch (m_iteration_kind) {
    case JS::Object::PropertyKind::Key:
        return JS::create_iterator_result_object(realm, JS::PrimitiveString::create(vm(), Utf16String::from_utf8(pair_name)), false);
    case JS::Object::PropertyKind::Value:
        return JS::create_iterator_result_object(realm, JS::PrimitiveString::create(vm(), Utf16String::from_utf8(pair_value)), false);
    case JS::Object::PropertyKind::KeyAndValue: {
        auto array = JS::Array::create_from(realm, { JS::PrimitiveString::create(vm(), Utf16String::from_utf8(pair_name)), JS::PrimitiveString::create(vm(), Utf16String::from_utf8(pair_value)) });
        return JS::create_iterator_result_object(realm, array, false);
    }
    default:
        VERIFY_NOT_REACHED();
        return JS::create_iterator_result_object(realm, JS::js_undefined(), true);
    }
}

}
