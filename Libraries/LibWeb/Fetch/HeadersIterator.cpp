/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/Headers.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Fetch/HeadersIterator.h>

namespace Web::Bindings {

template<>
void Intrinsics::create_web_prototype_and_constructor<HeadersIteratorPrototype>(JS::Realm& realm)
{
    auto prototype = realm.create<HeadersIteratorPrototype>(realm);
    m_prototypes.set("HeadersIterator"_utf16_fly_string, prototype);
}

static void set_headers_iterator_prototype(JS::Realm& realm, Fetch::HeadersIterator& iterator)
{
    static auto const& name = "HeadersIterator"_utf16_fly_string;
    Detail::set_prototype_for_interface_on<HeadersIteratorPrototype>(realm, iterator, name);
}

}

namespace Web::Fetch {

GC_DEFINE_ALLOCATOR(HeadersIterator);

HeadersIterator::HeadersIterator(JS::Realm& realm, Headers const& headers, JS::Object::PropertyKind iteration_kind)
    : JS::Object(realm, nullptr)
    , m_headers(headers)
    , m_iteration_kind(iteration_kind)
{
}

HeadersIterator::~HeadersIterator() = default;

GC::Ref<HeadersIterator> HeadersIterator::create(JS::Realm& realm, Headers const& headers, JS::Object::PropertyKind iteration_kind)
{
    auto iterator = realm.create<HeadersIterator>(realm, headers, iteration_kind);
    Bindings::set_headers_iterator_prototype(realm, iterator);
    return iterator;
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
