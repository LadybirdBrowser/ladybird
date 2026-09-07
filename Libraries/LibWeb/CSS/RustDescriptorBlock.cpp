/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/RustDescriptorBlock.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

using namespace Parser::ValueParserFFI;

static FfiDescriptor descriptor_view(DescriptorNameAndID const& name, StyleValue const& value)
{
    return { Parser::ffi_utf16_view(name.name()), to_underlying(name.id()), value.rust_style_value_data() };
}

RustDescriptorBlock::RustDescriptorBlock(Vector<Descriptor> descriptors)
    : m_descriptors(move(descriptors))
{
    Vector<FfiDescriptor> views;
    for (auto const& descriptor : m_descriptors)
        views.append(descriptor_view(descriptor.descriptor_name_and_id, *descriptor.value));
    m_block = rust_descriptor_block_create(views.data(), views.size());
}

RustDescriptorBlock::RustDescriptorBlock(FfiDescriptorBlock* block)
    : m_block(block)
{
    VERIFY(m_block);
}

RustDescriptorBlock::~RustDescriptorBlock()
{
    rust_descriptor_block_destroy(m_block);
}

RustDescriptorBlock::RustDescriptorBlock(RustDescriptorBlock&& other)
    : m_block(exchange(other.m_block, nullptr))
    , m_descriptors(move(other.m_descriptors))
    , m_view_revision(other.m_view_revision)
{
}

RustDescriptorBlock& RustDescriptorBlock::operator=(RustDescriptorBlock&& other)
{
    RustDescriptorBlock moved(move(other));
    swap(m_block, moved.m_block);
    swap(m_descriptors, moved.m_descriptors);
    swap(m_view_revision, moved.m_view_revision);
    return *this;
}

RustDescriptorBlock RustDescriptorBlock::share() const
{
    return RustDescriptorBlock { rust_descriptor_block_share(m_block) };
}

RustDescriptorBlock RustDescriptorBlock::retain() const
{
    return RustDescriptorBlock { rust_descriptor_block_retain(m_block) };
}

void RustDescriptorBlock::replace(RustDescriptorBlock const& source)
{
    if (this != &source)
        rust_descriptor_block_replace(m_block, source.m_block);
}

size_t RustDescriptorBlock::size() const
{
    return rust_descriptor_block_length(m_block);
}

Vector<Descriptor> const& RustDescriptorBlock::descriptors() const
{
    auto revision = rust_descriptor_block_revision(m_block);
    if (m_view_revision == revision)
        return m_descriptors;
    HashMap<void const*, NonnullRefPtr<StyleValue const>> existing_values;
    for (auto const& descriptor : m_descriptors)
        existing_values.set(descriptor.value->rust_style_value_data(), descriptor.value);
    auto view = rust_descriptor_block_view(m_block);
    m_descriptors.clear();
    for (auto const& descriptor : ReadonlySpan { view.descriptors, view.count }) {
        auto id = static_cast<DescriptorID>(descriptor.id);
        auto name = id == DescriptorID::Custom
            ? DescriptorNameAndID::from_custom_name(Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(descriptor.name.utf16), descriptor.name.length }))
            : DescriptorNameAndID::from_id(id);
        auto value = existing_values.ensure(descriptor.value, [&] {
            return NonnullRefPtr<StyleValue const> { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(reinterpret_cast<StyleValueFFI::StyleValueData const*>(descriptor.value))) };
        });
        m_descriptors.append({ move(name), move(value) });
    }
    m_view_revision = revision;
    return m_descriptors;
}

RefPtr<StyleValue const> RustDescriptorBlock::descriptor(DescriptorNameAndID const& name) const
{
    auto match = descriptors().first_matching([&](Descriptor const& descriptor) {
        return descriptor.descriptor_name_and_id == name;
    });
    if (match.has_value())
        return match->value;
    return nullptr;
}

RefPtr<StyleValue const> RustDescriptorBlock::descriptor_or_initial_value(AtRuleID at_rule, DescriptorNameAndID const& name) const
{
    if (auto value = descriptor(name))
        return value;
    return descriptor_initial_value(at_rule, name.id());
}

bool RustDescriptorBlock::set(DescriptorNameAndID const& name, StyleValue const& value)
{
    auto view = descriptor_view(name, value);
    return rust_descriptor_block_set(m_block, &view);
}

bool RustDescriptorBlock::remove(DescriptorNameAndID const& name)
{
    return rust_descriptor_block_remove(m_block, to_underlying(name.id()), Parser::ffi_utf16_view(name.name()));
}

size_t RustDescriptorBlock::external_memory_size() const
{
    return JS::saturating_add_external_memory_size(rust_descriptor_block_external_memory_size(m_block), JS::vector_external_memory_size(m_descriptors));
}

}
