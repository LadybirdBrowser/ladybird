/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibWeb/Bindings/FormDataIteratorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/XHR/FormDataIterator.h>

namespace Web::Bindings {

template<>
void Intrinsics::create_web_prototype_and_constructor<FormDataIteratorPrototype>(JS::Realm& realm)
{
    auto prototype = heap().allocate<FormDataIteratorPrototype>(realm, realm);
    m_prototypes.set("FormDataIterator"_fly_string, prototype);
}

}

namespace Web::XHR {

JS_DEFINE_ALLOCATOR(FormDataIterator);

JS::NonnullGCPtr<FormDataIterator> FormDataIterator::create(FormData const& form_data, JS::Object::PropertyKind iterator_kind)
{
    return form_data.heap().allocate<FormDataIterator>(form_data.realm(), form_data, iterator_kind);
}

FormDataIterator::FormDataIterator(Web::XHR::FormData const& form_data, JS::Object::PropertyKind iterator_kind)
    : PlatformObject(form_data.realm())
    , m_form_data(form_data)
    , m_iterator_kind(iterator_kind)
{
}

FormDataIterator::~FormDataIterator() = default;

void FormDataIterator::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FormDataIterator);
}

void FormDataIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_form_data);
}

JS::Object* FormDataIterator::next()
{
    auto& vm = this->vm();

    if (m_index >= m_form_data->m_entry_list.size())
        return create_iterator_result_object(vm, JS::js_undefined(), true);

    auto entry = m_form_data->m_entry_list[m_index++];
    if (m_iterator_kind == JS::Object::PropertyKind::Key)
        return create_iterator_result_object(vm, JS::PrimitiveString::create(vm, entry.name), false);

    auto entry_value = entry.value.visit(
        [&](JS::Handle<FileAPI::File> const& file) -> JS::Value {
            return file.cell();
        },
        [&](String const& string) -> JS::Value {
            return JS::PrimitiveString::create(vm, string);
        });

    if (m_iterator_kind == JS::Object::PropertyKind::Value)
        return create_iterator_result_object(vm, entry_value, false);

    return create_iterator_result_object(vm, JS::Array::create_from(realm(), { JS::PrimitiveString::create(vm, entry.name), entry_value }), false).ptr();
}

}
