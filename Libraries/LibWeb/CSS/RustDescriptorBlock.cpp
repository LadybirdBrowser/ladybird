/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
{
    Vector<FfiDescriptor> views;
    for (auto const& descriptor : descriptors)
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
{
}

RustDescriptorBlock& RustDescriptorBlock::operator=(RustDescriptorBlock&& other)
{
    RustDescriptorBlock moved(move(other));
    swap(m_block, moved.m_block);
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

Utf16String RustDescriptorBlock::item(size_t index) const
{
    return Utf16String::adopt_raw(rust_descriptor_block_item(m_block, index));
}

Utf16String RustDescriptorBlock::serialized() const
{
    size_t result = 0;
    VERIFY(rust_descriptor_block_serialize(m_block, &result));
    return Utf16String::adopt_raw(result);
}

Utf16String RustDescriptorBlock::property_value(DescriptorNameAndID const& name) const
{
    auto* value = rust_descriptor_block_get(m_block, to_underlying(name.id()), Parser::ffi_utf16_view(name.name()));
    if (!value)
        return {};
    auto text = StyleValueFFI::rust_style_value_serialize(value, to_underlying(SerializationMode::Normal));
    VERIFY(text);
    return Utf16String::adopt_raw(text);
}

RefPtr<StyleValue const> RustDescriptorBlock::descriptor(DescriptorNameAndID const& name) const
{
    auto* value = rust_descriptor_block_get(m_block, to_underlying(name.id()), Parser::ffi_utf16_view(name.name()));
    if (!value)
        return nullptr;
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(value)));
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
    return rust_descriptor_block_external_memory_size(m_block);
}

}
