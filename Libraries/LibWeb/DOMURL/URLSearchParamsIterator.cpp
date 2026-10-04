/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/URLSearchParams.h>
#include <LibWeb/DOMURL/URLSearchParamsIterator.h>

namespace Web::DOMURL {

GC_DEFINE_ALLOCATOR(URLSearchParamsIterator);

static constexpr JSHostClass url_search_params_iterator_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "URLSearchParamsIterator"sv, nullptr, nullptr, nullptr, 0);

URLSearchParamsIterator::URLSearchParamsIterator(URLSearchParams const& url_search_params, JS::Object::PropertyKind iteration_kind)
    : m_url_search_params(url_search_params)
    , m_iteration_kind(iteration_kind)
{
}

URLSearchParamsIterator::~URLSearchParamsIterator() = default;

WebIDL::ExceptionOr<GC::Ref<JS::HostObject>> URLSearchParamsIterator::create(JS::Realm& realm, URLSearchParams const& url_search_params, JS::Object::PropertyKind iteration_kind)
{
    static auto const& prototype_name = "URLSearchParamsIterator"_utf16_fly_string;
    auto iterator = realm.create<URLSearchParamsIterator>(url_search_params, iteration_kind);
    auto& prototype = Bindings::ensure_web_prototype<Bindings::URLSearchParamsIteratorPrototype>(realm, prototype_name);
    return JS::HostObject::create(realm, url_search_params_iterator_host_class, prototype, {}, iterator);
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
