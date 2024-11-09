/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/URLSearchParamsIteratorPrototype.h>
#include <LibWeb/DOMURL/URLSearchParamsIterator.h>

namespace Web::Bindings {

template<>
void Intrinsics::create_web_prototype_and_constructor<URLSearchParamsIteratorPrototype>(JS::Realm& realm)
{
    auto prototype = heap().allocate<URLSearchParamsIteratorPrototype>(realm, realm);
    m_prototypes.set("URLSearchParamsIterator"_fly_string, prototype);
}

}

namespace Web::DOMURL {

JS_DEFINE_ALLOCATOR(URLSearchParamsIterator);

WebIDL::ExceptionOr<JS::NonnullGCPtr<URLSearchParamsIterator>> URLSearchParamsIterator::create(URLSearchParams const& url_search_params, JS::Object::PropertyKind iteration_kind)
{
    return url_search_params.heap().allocate<URLSearchParamsIterator>(url_search_params.realm(), url_search_params, iteration_kind);
}

URLSearchParamsIterator::URLSearchParamsIterator(URLSearchParams const& url_search_params, JS::Object::PropertyKind iteration_kind)
    : PlatformObject(url_search_params.realm())
    , m_url_search_params(url_search_params)
    , m_iteration_kind(iteration_kind)
{
}

URLSearchParamsIterator::~URLSearchParamsIterator() = default;

void URLSearchParamsIterator::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(URLSearchParamsIterator);
}

void URLSearchParamsIterator::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_url_search_params);
}

JS::Object* URLSearchParamsIterator::next()
{
    if (m_index >= m_url_search_params->m_list.size())
        return create_iterator_result_object(vm(), JS::js_undefined(), true);

    auto& entry = m_url_search_params->m_list[m_index++];
    if (m_iteration_kind == JS::Object::PropertyKind::Key)
        return create_iterator_result_object(vm(), JS::PrimitiveString::create(vm(), entry.name), false);
    else if (m_iteration_kind == JS::Object::PropertyKind::Value)
        return create_iterator_result_object(vm(), JS::PrimitiveString::create(vm(), entry.value), false);

    return create_iterator_result_object(vm(), JS::Array::create_from(realm(), { JS::PrimitiveString::create(vm(), entry.name), JS::PrimitiveString::create(vm(), entry.value) }), false);
}

}
