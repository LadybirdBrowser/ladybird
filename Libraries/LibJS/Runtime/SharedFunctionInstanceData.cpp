/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/RustIntegration.h>
#include <LibJS/SourceCode.h>

namespace JS {

GC_DEFINE_ALLOCATOR(SharedFunctionInstanceData);

SharedFunctionInstanceData::SharedFunctionInstanceData(
    VM&,
    FunctionKind kind,
    Utf16FlyString name,
    i32 function_length,
    u32 formal_parameter_count,
    bool strict,
    bool is_arrow_function,
    bool has_simple_parameter_list,
    Vector<Utf16FlyString> parameter_names_for_mapped_arguments,
    SharedFunctionInstanceDataList& owner_shared_function_data_list,
    void* rust_function_ast)
    : m_name(move(name))
    , m_function_length(function_length)
    , m_formal_parameter_count(formal_parameter_count)
    , m_parameter_names_for_mapped_arguments(move(parameter_names_for_mapped_arguments))
    , m_kind(kind)
    , m_strict(strict)
    , m_is_arrow_function(is_arrow_function)
    , m_has_simple_parameter_list(has_simple_parameter_list)
    , m_rust_function_ast(rust_function_ast)
    , m_use_rust_compilation(true)
{
    initialize_after_construction();
    owner_shared_function_data_list.append(*this);
}

SharedFunctionInstanceData::SharedFunctionInstanceData(
    VM&,
    FunctionKind kind,
    Utf16FlyString name,
    i32 function_length,
    u32 formal_parameter_count,
    bool strict,
    bool is_arrow_function,
    bool has_simple_parameter_list,
    Vector<Utf16FlyString> parameter_names_for_mapped_arguments,
    NoSharedFunctionDataList,
    void* rust_function_ast)
    : m_name(move(name))
    , m_function_length(function_length)
    , m_formal_parameter_count(formal_parameter_count)
    , m_parameter_names_for_mapped_arguments(move(parameter_names_for_mapped_arguments))
    , m_kind(kind)
    , m_strict(strict)
    , m_is_arrow_function(is_arrow_function)
    , m_has_simple_parameter_list(has_simple_parameter_list)
    , m_rust_function_ast(rust_function_ast)
    , m_use_rust_compilation(true)
{
    initialize_after_construction();
}

void SharedFunctionInstanceData::initialize_after_construction()
{
    if (m_is_arrow_function)
        m_this_mode = ThisMode::Lexical;
    else if (m_strict)
        m_this_mode = ThisMode::Strict;
    else
        m_this_mode = ThisMode::Global;

    update_can_inline_call();
}

void SharedFunctionInstanceData::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_executable);
    visitor.visit(m_function_environment_shape);
    visitor.visit(m_var_environment_shape);
    for (auto& function : m_functions_to_initialize)
        visitor.visit(function.shared_data);
    m_class_field_initializer_name.visit([&](PropertyKey const& key) { key.visit_edges(visitor); }, [](auto&) {});
}

size_t SharedFunctionInstanceData::external_memory_size() const
{
    size_t size = utf16_string_external_memory_size(m_source_text_owner);
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_local_variables_names));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_parameter_names_for_mapped_arguments));
    size = saturating_add_external_memory_size(size, hash_map_external_memory_size(m_parameter_names));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_functions_to_initialize));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_var_names_to_initialize_binding));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_function_names_to_initialize_binding));
    size = saturating_add_external_memory_size(size, vector_external_memory_size(m_lexical_bindings));
    return size;
}

Utf16String SharedFunctionInstanceData::source_text() const
{
    if (!m_source_text_owner.is_empty())
        return m_source_text_owner;

    if (!m_source_code)
        return {};

    auto old_external_memory_size = utf16_string_external_memory_size(m_source_text_owner);
    m_source_text_owner = m_source_code->source_text_from_offsets(m_source_text_offset, m_source_text_length);
    auto new_external_memory_size = utf16_string_external_memory_size(m_source_text_owner);
    if (new_external_memory_size > old_external_memory_size)
        heap().did_allocate_external_memory(new_external_memory_size - old_external_memory_size);
    return m_source_text_owner;
}

void SharedFunctionInstanceData::set_source_text(Utf16View source_text)
{
    m_source_text_owner = Utf16String::from_utf16(source_text);
    m_source_code = nullptr;
    m_source_text_offset = 0;
    m_source_text_length = 0;
}

void SharedFunctionInstanceData::set_source_text_range(SourceCode const& source_code, size_t source_text_offset, size_t source_text_length)
{
    m_source_text_owner = {};
    m_source_code = &source_code;
    m_source_text_offset = source_text_offset;
    m_source_text_length = source_text_length;
}

SharedFunctionInstanceData::~SharedFunctionInstanceData() = default;

void SharedFunctionInstanceData::set_executable(GC::Ptr<Bytecode::Executable> executable)
{
    m_executable = executable;
    update_can_inline_call();
}

void SharedFunctionInstanceData::set_is_class_constructor()
{
    m_is_class_constructor = true;
    update_can_inline_call();
}

void SharedFunctionInstanceData::update_asm_call_metadata()
{
    m_asm_call_metadata = m_formal_parameter_count;
    if (m_can_inline_call)
        m_asm_call_metadata |= asm_call_metadata_can_inline_call;
    if (m_function_environment_needed || m_this_value_needs_environment_resolution)
        m_asm_call_metadata |= asm_call_metadata_needs_environment_or_this_value_resolution;
    if (m_uses_this)
        m_asm_call_metadata |= asm_call_metadata_uses_this;
    if (m_strict)
        m_asm_call_metadata |= asm_call_metadata_strict;
}

void SharedFunctionInstanceData::finalize()
{
    if (m_owner_shared_function_data_list)
        m_owner_shared_function_data_list->remove(*this);

    Base::finalize();
    RustIntegration::free_function_ast(m_rust_function_ast);
    m_rust_function_ast = nullptr;
    RustIntegration::free_cached_bytecode_executable(m_cached_bytecode_executable);
    m_cached_bytecode_executable = nullptr;
    RustIntegration::free_precompiled_bytecode_executable(m_precompiled_bytecode_executable);
    m_precompiled_bytecode_executable = nullptr;
}

void SharedFunctionInstanceData::clear_compile_inputs()
{
    clear_non_bytecode_cache_compile_inputs();
    RustIntegration::free_cached_bytecode_executable(m_cached_bytecode_executable);
    m_cached_bytecode_executable = nullptr;
}

void SharedFunctionInstanceData::clear_non_bytecode_cache_compile_inputs()
{
    m_functions_to_initialize.clear();
    m_var_names_to_initialize_binding.clear();
    m_lexical_bindings.clear();
    RustIntegration::free_function_ast(m_rust_function_ast);
    m_rust_function_ast = nullptr;
    RustIntegration::free_precompiled_bytecode_executable(m_precompiled_bytecode_executable);
    m_precompiled_bytecode_executable = nullptr;
}

void SharedFunctionInstanceData::update_can_inline_call()
{
    m_can_inline_call = m_executable && m_kind == FunctionKind::Normal && !m_is_class_constructor;
    update_asm_call_metadata();
}

SharedFunctionInstanceDataList::~SharedFunctionInstanceDataList()
{
    clear();
}

void SharedFunctionInstanceDataList::append(SharedFunctionInstanceData& shared_data)
{
    if (shared_data.m_owner_shared_function_data_list == this)
        return;
    if (shared_data.m_owner_shared_function_data_list)
        shared_data.m_owner_shared_function_data_list->remove(shared_data);
    VERIFY(!shared_data.m_script_or_module_list_node.is_in_list());

    m_list.append(shared_data);
    shared_data.m_owner_shared_function_data_list = this;
}

void SharedFunctionInstanceDataList::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto& shared_data : m_list)
        visitor.visit(shared_data);
}

void SharedFunctionInstanceDataList::clear_non_bytecode_cache_compile_inputs()
{
    for (auto& shared_data : m_list)
        shared_data.clear_non_bytecode_cache_compile_inputs();
}

bool SharedFunctionInstanceDataList::contains_rust_function_ast() const
{
    for (auto const& shared_data : m_list) {
        if (shared_data.m_rust_function_ast)
            return true;
    }
    return false;
}

bool SharedFunctionInstanceDataList::contains_precompiled_bytecode() const
{
    for (auto const& shared_data : m_list) {
        if (shared_data.m_precompiled_bytecode_executable)
            return true;
    }
    return false;
}

void SharedFunctionInstanceDataList::clear()
{
    while (!m_list.is_empty()) {
        auto* shared_data = m_list.first();
        remove(*shared_data);
    }
}

void SharedFunctionInstanceDataList::remove(SharedFunctionInstanceData& shared_data)
{
    VERIFY(shared_data.m_owner_shared_function_data_list == this);
    m_list.remove(shared_data);
    shared_data.m_owner_shared_function_data_list = nullptr;
}

}
