/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/URLSearchParams.h>
#include <LibWeb/DOMURL/URLSearchParamsIterator.h>

namespace Web::Bindings {

template<>
void Intrinsics::create_web_prototype_and_constructor<URLSearchParamsIteratorPrototype>(JS::Realm& realm)
{
    auto prototype = realm.create<URLSearchParamsIteratorPrototype>(realm);
    m_prototypes.set("URLSearchParamsIterator"_utf16_fly_string, prototype);
}

static void set_url_search_params_iterator_prototype(JS::Realm& realm, DOMURL::URLSearchParamsIterator& iterator)
{
    static auto const& name = "URLSearchParamsIterator"_utf16_fly_string;
    Detail::set_prototype_for_interface_on<URLSearchParamsIteratorPrototype>(realm, iterator, name);
}

}

namespace Web::DOMURL {

GC_DEFINE_ALLOCATOR(URLSearchParamsIterator);

URLSearchParamsIterator::URLSearchParamsIterator(JS::Realm& realm, URLSearchParams const& url_search_params, JS::Object::PropertyKind iteration_kind)
    : JS::Object(realm, nullptr)
    , m_url_search_params(url_search_params)
    , m_iteration_kind(iteration_kind)
{
}

URLSearchParamsIterator::~URLSearchParamsIterator() = default;

WebIDL::ExceptionOr<GC::Ref<URLSearchParamsIterator>> URLSearchParamsIterator::create(JS::Realm& realm, URLSearchParams const& url_search_params, JS::Object::PropertyKind iteration_kind)
{
    auto iterator = realm.create<URLSearchParamsIterator>(realm, url_search_params, iteration_kind);
    Bindings::set_url_search_params_iterator_prototype(realm, iterator);
    return iterator;
}

void URLSearchParamsIterator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_url_search_params);
}

JS::Object* URLSearchParamsIterator::next(JS::Realm& realm)
{
    if (m_index >= m_url_search_params->m_list.size())
        return JS::create_iterator_result_object(realm, JS::js_undefined(), true).ptr();

    auto& entry = m_url_search_params->m_list[m_index++];
    if (m_iteration_kind == JS::Object::PropertyKind::Key)
        return JS::create_iterator_result_object(realm, JS::PrimitiveString::create(vm(), entry.name), false).ptr();
    else if (m_iteration_kind == JS::Object::PropertyKind::Value)
        return JS::create_iterator_result_object(realm, JS::PrimitiveString::create(vm(), entry.value), false).ptr();

    return JS::create_iterator_result_object(realm, JS::Array::create_from(realm, { JS::PrimitiveString::create(vm(), entry.name), JS::PrimitiveString::create(vm(), entry.value) }), false).ptr();
}

}
