/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

using namespace Parser::ValueParserFFI;

static FfiDeclaredProperty property_view(PropertyID property_id, StyleValue const& value, Important important, Utf16View name = {})
{
    return {
        .property_id = to_underlying(property_id),
        .important = important == Important::Yes,
        .value = value.rust_style_value_data(),
        .name = Parser::ffi_utf16_view(name),
    };
}

static StyleProperty wrap_property(FfiDeclaredProperty const& property, HashMap<void const*, NonnullRefPtr<StyleValue const>>& values)
{
    auto value = values.ensure(property.value, [&] {
        return NonnullRefPtr<StyleValue const> { StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(property.value))) };
    });
    return { property.important ? Important::Yes : Important::No, static_cast<PropertyID>(property.property_id), move(value) };
}

RustDeclarationBlockSnapshot::RustDeclarationBlockSnapshot(DeclarationBlockData const* data)
    : m_data(data)
{
    VERIFY(m_data);
}

RustDeclarationBlockSnapshot::~RustDeclarationBlockSnapshot()
{
    rust_declaration_data_release(m_data);
}

RustDeclarationBlockSnapshot::RustDeclarationBlockSnapshot(RustDeclarationBlockSnapshot&& other)
    : m_data(exchange(other.m_data, nullptr))
{
}

RustDeclarationBlockSnapshot& RustDeclarationBlockSnapshot::operator=(RustDeclarationBlockSnapshot&& other)
{
    RustDeclarationBlockSnapshot moved(move(other));
    swap(m_data, moved.m_data);
    return *this;
}

FfiDeclarationBlockDependencies RustDeclarationBlockSnapshot::dependencies() const
{
    return rust_declaration_data_dependencies(m_data);
}

RustDeclarationBlock::RustDeclarationBlock(Vector<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties)
    : m_properties(move(properties))
    , m_custom_properties(move(custom_properties))
{
    Vector<FfiDeclaredProperty> property_views;
    Vector<FfiDeclaredProperty> custom_property_views;
    for (auto const& property : m_properties)
        property_views.append(property_view(property.property_id, *property.value, property.important));
    for (auto const& property : m_custom_properties)
        custom_property_views.append(property_view(property.value.property_id, *property.value.value, property.value.important, property.key));
    m_block = rust_declaration_block_create(property_views.data(), property_views.size(), custom_property_views.data(), custom_property_views.size());
}

RustDeclarationBlock::RustDeclarationBlock(DeclarationBlock* block)
    : m_block(block)
{
    VERIFY(m_block);
}

RustDeclarationBlock::~RustDeclarationBlock()
{
    rust_declaration_block_destroy(m_block);
}

RustDeclarationBlock::RustDeclarationBlock(RustDeclarationBlock&& other)
    : m_block(exchange(other.m_block, nullptr))
    , m_properties(move(other.m_properties))
    , m_custom_properties(move(other.m_custom_properties))
    , m_view_revision(other.m_view_revision)
{
}

RustDeclarationBlock& RustDeclarationBlock::operator=(RustDeclarationBlock&& other)
{
    RustDeclarationBlock moved(move(other));
    swap(m_block, moved.m_block);
    swap(m_properties, moved.m_properties);
    swap(m_custom_properties, moved.m_custom_properties);
    swap(m_view_revision, moved.m_view_revision);
    return *this;
}

RustDeclarationBlock RustDeclarationBlock::share() const
{
    return RustDeclarationBlock { rust_declaration_block_share(m_block) };
}

RustDeclarationBlock RustDeclarationBlock::retain() const
{
    return RustDeclarationBlock { rust_declaration_block_retain(m_block) };
}

void RustDeclarationBlock::replace(RustDeclarationBlock const& source)
{
    if (this == &source)
        return;
    rust_declaration_block_replace(m_block, source.m_block);
}

u64 RustDeclarationBlock::identity() const
{
    return rust_declaration_block_identity(m_block);
}

u64 RustDeclarationBlock::revision() const
{
    return rust_declaration_block_revision(m_block);
}

bool RustDeclarationBlock::is_empty() const
{
    return rust_declaration_block_is_empty(m_block);
}

void RustDeclarationBlock::update_views() const
{
    auto current_revision = revision();
    if (m_view_revision == current_revision)
        return;

    HashMap<void const*, NonnullRefPtr<StyleValue const>> existing_values;
    for (auto const& property : m_properties)
        existing_values.set(property.value->rust_style_value_data(), property.value);
    for (auto const& property : m_custom_properties)
        existing_values.set(property.value.value->rust_style_value_data(), property.value.value);

    m_properties.clear();
    m_custom_properties.clear();
    struct Context {
        RustDeclarationBlock const& block;
        HashMap<void const*, NonnullRefPtr<StyleValue const>>& values;
    } context { *this, existing_values };
    rust_declaration_block_visit(m_block, &context, [](void* raw_context, FfiDeclaredProperty const* property) {
        auto& context = *static_cast<Context*>(raw_context);
        auto value = wrap_property(*property, context.values);
        if (property->name.length)
            context.block.m_custom_properties.set(Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(property->name.utf16), property->name.length }), move(value));
        else
            context.block.m_properties.append(move(value));
    });
    m_view_revision = current_revision;
}

Vector<StyleProperty> const& RustDeclarationBlock::properties() const
{
    update_views();
    return m_properties;
}

OrderedHashMap<Utf16FlyString, StyleProperty> const& RustDeclarationBlock::custom_properties() const
{
    update_views();
    return m_custom_properties;
}

FfiDeclarationBlockDependencies RustDeclarationBlock::dependencies() const
{
    return rust_declaration_block_dependencies(m_block);
}

size_t RustDeclarationBlock::external_memory_size() const
{
    auto size = rust_declaration_block_external_memory_size(m_block);
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_properties));
    return JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_custom_properties));
}

bool RustDeclarationBlock::set(PropertyID property_id, StyleValue const& value, Important important)
{
    auto property = property_view(property_id, value, important);
    return rust_declaration_block_set(m_block, &property);
}

void RustDeclarationBlock::append(PropertyID property_id, StyleValue const& value, Important important)
{
    auto property = property_view(property_id, value, important);
    rust_declaration_block_append(m_block, &property);
}

void RustDeclarationBlock::set_custom(Utf16FlyString const& name, StyleProperty const& property)
{
    auto view = property_view(property.property_id, *property.value, property.important, name);
    rust_declaration_block_set_custom(m_block, &view);
}

bool RustDeclarationBlock::remove(PropertyID property_id)
{
    return rust_declaration_block_remove(m_block, to_underlying(property_id));
}

bool RustDeclarationBlock::remove_custom(Utf16FlyString const& name)
{
    return rust_declaration_block_remove_custom(m_block, Parser::ffi_utf16_view(name));
}

}
