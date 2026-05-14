/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/EnvironmentShape.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

GC_DEFINE_ALLOCATOR(EnvironmentShape);

EnvironmentShape::EnvironmentShape(Vector<BindingDescriptor> bindings, HashMap<Utf16FlyString, size_t> binding_indices)
    : m_bindings(move(bindings))
    , m_binding_indices(move(binding_indices))
{
}

GC::Ref<EnvironmentShape> EnvironmentShape::create(VM& vm, ReadonlySpan<Utf16FlyString> names, ReadonlySpan<u8> flags)
{
    VERIFY(names.size() == flags.size());

    Vector<BindingDescriptor> bindings;
    bindings.ensure_capacity(names.size());

    HashMap<Utf16FlyString, size_t> binding_indices;
    binding_indices.ensure_capacity(names.size());

    for (size_t i = 0; i < names.size(); ++i) {
        bindings.unchecked_append({ names[i], flags[i] });

        if (!names[i].is_empty())
            binding_indices.set(names[i], i);
    }

    return vm.heap().allocate<EnvironmentShape>(move(bindings), move(binding_indices));
}

Optional<size_t> EnvironmentShape::find_binding(Utf16FlyString const& name) const
{
    auto it = m_binding_indices.find(name);
    if (it == m_binding_indices.end())
        return {};
    return it->value;
}

size_t EnvironmentShape::external_memory_size() const
{
    auto size = vector_external_memory_size(m_bindings);
    size = saturating_add_external_memory_size(size, hash_map_external_memory_size(m_binding_indices));
    return size;
}

}
