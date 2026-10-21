/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Tag.h>
#include <LibWeb/WebAssembly/BindingsGlue.h>
#include <LibWeb/WebAssembly/Global.h>
#include <LibWeb/WebAssembly/Tag.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Tag);

// https://webassembly.github.io/spec/js-api/#dom-tag-tag
GC::Ref<Tag> Tag::create(ReadonlySpan<Wasm::Runtime::ValueType> parameters)
{
    // 4. Let store be the surrounding agent's associated store.
    // 5. Let (store, tagAddress) be tag_alloc(store, « wasmParameters » → « »).
    // 6. Set the surrounding agent's associated store to store.
    auto tag = Detail::WebAssemblyCache::the().store().allocate_tag(parameters);

    // 7. Initialize this from tagAddress.
    return from_address(tag);
}

// https://webassembly.github.io/spec/js-api/#create-a-tag-object
GC::Ref<Tag> Tag::from_address(Wasm::Runtime::Entity tag)
{
    // 1. Let map be the surrounding agent's associated tag object cache.
    // 2. If map[tagAddress] exists,
    //     1. Return map[tagAddress].
    auto& cache = Detail::WebAssemblyCache::the();
    if (auto object = cache.tag_object(tag))
        return *object;

    // 3. Let tag be a new Tag.
    // 4. Initialize tag from tagAddress.
    // https://webassembly.github.io/spec/js-api/#initialize-a-tag-object
    // 3. Set tag.[[Address]] to tagAddress.
    auto object = GC::Heap::the().allocate<Tag>(tag);

    // 4. Set map[tagAddress] to tag.
    cache.set_tag_object(tag, object);

    // 5. Return tag.
    return object;
}

Tag::Tag(Wasm::Runtime::Entity tag)
    : m_tag(tag)
{
}

void Tag::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_tag);
}

}

namespace Web::Bindings {

// https://webassembly.github.io/spec/js-api/#dom-tag-tag
WebIDL::ExceptionOr<GC::Ref<WebAssembly::Tag>> construct_tag(JS::Realm&, TagType const& type)
{
    // 1. Let parameters be type["parameters"].
    // 2. Let wasmParameters be «».
    Vector<Wasm::Runtime::ValueType> parameters;
    parameters.ensure_capacity(type.parameters.size());

    // 3. For each type of parameters,
    for (auto parameter : type.parameters) {
        // 1. Append ToValueType(type) to wasmParameters.
        parameters.unchecked_append(to_value_type(parameter));
    }

    return WebAssembly::Tag::create(parameters);
}

WebAssembly::Tag* tag_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<WebAssembly::Tag>(&value.as_object()) : nullptr;
}

}
