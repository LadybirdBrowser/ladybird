/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Interpreter.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWasm/Types.h>

namespace Wasm {

Optional<FunctionAddress> Store::allocate(ModuleInstance& instance, Module const& module, CodeSection::Code const& code, TypeIndex type_index)
{
    FunctionAddress address { m_functions.size() };
    if (type_index.value() > instance.types().size())
        return {};

    auto& type = instance.types()[type_index.value()];
    m_functions.empend(WasmFunction { type, instance, module, code });
    return address;
}

Optional<FunctionAddress> Store::allocate(HostFunction&& function)
{
    FunctionAddress address { m_functions.size() };
    m_functions.empend(HostFunction { move(function) });
    return address;
}

Optional<TableAddress> Store::allocate(TableType const& type)
{
    TableAddress address { m_tables.size() };
    Vector<Reference> elements;
    elements.ensure_capacity(type.limits().min());
    for (size_t i = 0; i < type.limits().min(); i++)
        elements.append(Wasm::Reference { Wasm::Reference::Null { type.element_type() } });
    elements.resize(type.limits().min());
    m_tables.empend(TableInstance { type, move(elements) });
    return address;
}

Optional<MemoryAddress> Store::allocate(MemoryType const& type)
{
    MemoryAddress address { m_memories.size() };
    auto instance = MemoryInstance::create(type);
    if (instance.is_error())
        return {};

    m_memories.append(instance.release_value());
    return address;
}

Optional<GlobalAddress> Store::allocate(GlobalType const& type, Value value)
{
    GlobalAddress address { m_globals.size() };
    m_globals.append(GlobalInstance { value, type.is_mutable(), type.type() });
    return address;
}

Optional<DataAddress> Store::allocate_data(Vector<u8> initializer)
{
    DataAddress address { m_datas.size() };
    m_datas.append(DataInstance { move(initializer) });
    return address;
}

Optional<ElementAddress> Store::allocate(ValueType const& type, Vector<Reference> references)
{
    ElementAddress address { m_elements.size() };
    m_elements.append(ElementInstance { type, move(references) });
    return address;
}

FunctionInstance* Store::get(FunctionAddress address)
{
    auto value = address.value();
    if (m_functions.size() <= value)
        return nullptr;
    return &m_functions[value];
}

Module const* Store::get_module_for(Wasm::FunctionAddress address)
{
    auto* function = get(address);
    if (!function || function->has<HostFunction>())
        return nullptr;
    return function->get<WasmFunction>().module_ref().ptr();
}

TableInstance* Store::get(TableAddress address)
{
    auto value = address.value();
    if (m_tables.size() <= value)
        return nullptr;
    return &m_tables[value];
}

MemoryInstance* Store::get(MemoryAddress address)
{
    auto value = address.value();
    if (m_memories.size() <= value)
        return nullptr;
    return &m_memories[value];
}

GlobalInstance* Store::get(GlobalAddress address)
{
    auto value = address.value();
    if (m_globals.size() <= value)
        return nullptr;
    return &m_globals[value];
}

ElementInstance* Store::get(ElementAddress address)
{
    auto value = address.value();
    if (m_elements.size() <= value)
        return nullptr;
    return &m_elements[value];
}

DataInstance* Store::get(DataAddress address)
{
    auto value = address.value();
    if (m_datas.size() <= value)
        return nullptr;
    return &m_datas[value];
}

ErrorOr<void, ValidationError> AbstractMachine::validate(Module& module)
{
    if (module.validation_status() != Module::ValidationStatus::Unchecked) {
        if (module.validation_status() == Module::ValidationStatus::Valid)
            return {};

        return ValidationError { module.validation_error() };
    }

    auto result = Validator {}.validate(module);
    if (result.is_error()) {
        module.set_validation_error(result.error().error_string);
        return result.release_error();
    }

    return {};
}
InstantiationResult AbstractMachine::instantiate(Module const& module, Vector<ExternValue> externs)
{
    if (auto result = validate(const_cast<Module&>(module)); result.is_error())
        return InstantiationError { ByteString::formatted("Validation failed: {}", result.error()) };

    auto main_module_instance_pointer = make<ModuleInstance>();
    auto& main_module_instance = *main_module_instance_pointer;

    main_module_instance.types() = module.type_section().types();

    Vector<Value> global_values;
    Vector<Vector<Reference>> elements;
    ModuleInstance auxiliary_instance;

    for (auto [i, import_] : enumerate(module.import_section().imports())) {
        auto extern_ = externs.at(i);
        auto invalid = import_.description().visit(
            [&](MemoryType const& mem_type) -> Optional<ByteString> {
                if (!extern_.has<MemoryAddress>())
                    return "Expected memory import"sv;
                auto other_mem_type = m_store.get(extern_.get<MemoryAddress>())->type();
                if (other_mem_type.limits().is_subset_of(mem_type.limits()))
                    return {};
                return ByteString::formatted("Memory import and extern do not match: {}-{} vs {}-{}", mem_type.limits().min(), mem_type.limits().max(), other_mem_type.limits().min(), other_mem_type.limits().max());
            },
            [&](TableType const& table_type) -> Optional<ByteString> {
                if (!extern_.has<TableAddress>())
                    return "Expected table import"sv;
                auto other_table_type = m_store.get(extern_.get<TableAddress>())->type();
                if (table_type.element_type() == other_table_type.element_type()
                    && other_table_type.limits().is_subset_of(table_type.limits()))
                    return {};

                return ByteString::formatted("Table import and extern do not match: {}-{} vs {}-{}", table_type.limits().min(), table_type.limits().max(), other_table_type.limits().min(), other_table_type.limits().max());
            },
            [&](GlobalType const& global_type) -> Optional<ByteString> {
                if (!extern_.has<GlobalAddress>())
                    return "Expected global import"sv;
                auto other_global_type = m_store.get(extern_.get<GlobalAddress>())->type();
                if (global_type.type() == other_global_type.type()
                    && global_type.is_mutable() == other_global_type.is_mutable())
                    return {};
                return "Global import and extern do not match"sv;
            },
            [&](FunctionType const& type) -> Optional<ByteString> {
                if (!extern_.has<FunctionAddress>())
                    return "Expected function import"sv;
                auto other_type = m_store.get(extern_.get<FunctionAddress>())->visit([&](WasmFunction const& wasm_func) { return wasm_func.type(); }, [&](HostFunction const& host_func) { return host_func.type(); });
                if (type.results() != other_type.results())
                    return ByteString::formatted("Function import and extern do not match, results: {} vs {}", type.results(), other_type.results());
                if (type.parameters() != other_type.parameters())
                    return ByteString::formatted("Function import and extern do not match, parameters: {} vs {}", type.parameters(), other_type.parameters());
                return {};
            },
            [&](TypeIndex type_index) -> Optional<ByteString> {
                if (!extern_.has<FunctionAddress>())
                    return "Expected function import"sv;
                auto other_type = m_store.get(extern_.get<FunctionAddress>())->visit([&](WasmFunction const& wasm_func) { return wasm_func.type(); }, [&](HostFunction const& host_func) { return host_func.type(); });
                auto& type = module.type_section().types()[type_index.value()];
                if (type.results() != other_type.results())
                    return ByteString::formatted("Function import and extern do not match, results: {} vs {}", type.results(), other_type.results());
                if (type.parameters() != other_type.parameters())
                    return ByteString::formatted("Function import and extern do not match, parameters: {} vs {}", type.parameters(), other_type.parameters());
                return {};
            });
        if (invalid.has_value())
            return InstantiationError { ByteString::formatted("{}::{}: {}", import_.module(), import_.name(), invalid.release_value()) };
    }

    for (auto& entry : externs) {
        if (auto* ptr = entry.get_pointer<GlobalAddress>())
            auxiliary_instance.globals().append(*ptr);
        else if (auto* ptr = entry.get_pointer<FunctionAddress>())
            auxiliary_instance.functions().append(*ptr);
    }

    Vector<FunctionAddress> module_functions;
    module_functions.ensure_capacity(module.function_section().types().size());

    size_t i = 0;
    for (auto& code : module.code_section().functions()) {
        auto type_index = module.function_section().types()[i];
        auto address = m_store.allocate(main_module_instance, module, code, type_index);
        VERIFY(address.has_value());
        auxiliary_instance.functions().append(*address);
        module_functions.append(*address);
        ++i;
    }

    BytecodeInterpreter interpreter(m_stack_info);

    for (auto& entry : module.global_section().entries()) {
        Configuration config { m_store };
        if (m_should_limit_instruction_count)
            config.enable_instruction_count_limit();
        config.set_frame(Frame {
            auxiliary_instance,
            Vector<Value> {},
            entry.expression(),
            1,
        });
        auto result = config.execute(interpreter).assert_wasm_result();
        if (result.is_trap())
            return InstantiationError { ByteString::formatted("Global value construction trapped: {}", result.trap().reason) };
        global_values.append(result.values().first());
    }

    if (auto result = allocate_all_initial_phase(module, main_module_instance, externs, global_values, module_functions); result.has_value())
        return result.release_value();

    for (auto& segment : module.element_section().segments()) {
        Vector<Reference> references;
        for (auto& entry : segment.init) {
            Configuration config { m_store };
            if (m_should_limit_instruction_count)
                config.enable_instruction_count_limit();
            config.set_frame(Frame {
                auxiliary_instance,
                Vector<Value> {},
                entry,
                entry.instructions().size(),
            });
            auto result = config.execute(interpreter).assert_wasm_result();
            if (result.is_trap())
                return InstantiationError { ByteString::formatted("Element construction trapped: {}", result.trap().reason) };

            for (auto& value : result.values()) {
                auto reference = value.to<Reference>();
                references.append(reference);
            }
        }
        elements.append(move(references));
    }

    if (auto result = allocate_all_final_phase(module, main_module_instance, elements); result.has_value())
        return result.release_value();

    size_t index = 0;
    for (auto& segment : module.element_section().segments()) {
        auto current_index = index;
        ++index;
        auto active_ptr = segment.mode.get_pointer<ElementSection::Active>();
        auto elem_instance = m_store.get(main_module_instance.elements()[current_index]);
        if (!active_ptr) {
            if (segment.mode.has<ElementSection::Declarative>())
                *elem_instance = ElementInstance(elem_instance->type(), {});
            continue;
        }
        Configuration config { m_store };
        if (m_should_limit_instruction_count)
            config.enable_instruction_count_limit();
        config.set_frame(Frame {
            auxiliary_instance,
            Vector<Value> {},
            active_ptr->expression,
            1,
        });
        auto result = config.execute(interpreter).assert_wasm_result();
        if (result.is_trap())
            return InstantiationError { ByteString::formatted("Element section initialisation trapped: {}", result.trap().reason) };
        auto d = result.values().first().to<i32>();
        auto table_instance = m_store.get(main_module_instance.tables()[active_ptr->index.value()]);
        if (current_index >= main_module_instance.elements().size())
            return InstantiationError { "Invalid element referenced by active element segment" };
        if (!table_instance || !elem_instance)
            return InstantiationError { "Invalid element referenced by active element segment" };

        Checked<size_t> total_size = elem_instance->references().size();
        total_size.saturating_add(d);

        if (total_size.value() > table_instance->elements().size())
            return InstantiationError { "Table instantiation out of bounds" };

        size_t i = 0;
        for (auto it = elem_instance->references().begin(); it < elem_instance->references().end(); ++i, ++it)
            table_instance->elements()[i + d] = *it;
        // Drop element
        *m_store.get(main_module_instance.elements()[current_index]) = ElementInstance(elem_instance->type(), {});
    }

    for (auto& segment : module.data_section().data()) {
        Optional<InstantiationError> result = segment.value().visit(
            [&](DataSection::Data::Active const& data) -> Optional<InstantiationError> {
                Configuration config { m_store };
                if (m_should_limit_instruction_count)
                    config.enable_instruction_count_limit();
                config.set_frame(Frame {
                    auxiliary_instance,
                    Vector<Value> {},
                    data.offset,
                    1,
                });
                auto result = config.execute(interpreter).assert_wasm_result();
                if (result.is_trap())
                    return InstantiationError { ByteString::formatted("Data section initialisation trapped: {}", result.trap().reason) };
                size_t offset = result.values().first().to<u64>();
                if (main_module_instance.memories().size() <= data.index.value()) {
                    return InstantiationError {
                        ByteString::formatted("Data segment referenced out-of-bounds memory ({}) of max {} entries",
                            data.index.value(), main_module_instance.memories().size())
                    };
                }
                auto maybe_data_address = m_store.allocate_data(data.init);
                if (!maybe_data_address.has_value()) {
                    return InstantiationError { "Failed to allocate a data instance for an active data segment"sv };
                }
                main_module_instance.datas().append(*maybe_data_address);

                auto address = main_module_instance.memories()[data.index.value()];
                auto instance = m_store.get(address);
                Checked<size_t> checked_offset = data.init.size();
                checked_offset += offset;
                if (checked_offset.has_overflow() || checked_offset > instance->size()) {
                    return InstantiationError {
                        ByteString::formatted("Data segment attempted to write to out-of-bounds memory ({}) in memory of size {}",
                            offset, instance->size())
                    };
                }
                if (!data.init.is_empty())
                    instance->data().overwrite(offset, data.init.data(), data.init.size());
                return {};
            },
            [&](DataSection::Data::Passive const& passive) -> Optional<InstantiationError> {
                auto maybe_data_address = m_store.allocate_data(passive.init);
                if (!maybe_data_address.has_value()) {
                    return InstantiationError { "Failed to allocate a data instance for a passive data segment"sv };
                }
                main_module_instance.datas().append(*maybe_data_address);
                return {};
            });
        if (result.has_value())
            return result.release_value();
    }

    if (module.start_section().function().has_value()) {
        auto& functions = main_module_instance.functions();
        auto index = module.start_section().function()->index();
        if (functions.size() <= index.value()) {
            return InstantiationError { ByteString::formatted("Start section function referenced invalid index {} of max {} entries", index.value(), functions.size()) };
        }
        auto result = invoke(functions[index.value()], {});
        if (result.is_trap())
            return InstantiationError { ByteString::formatted("Start function trapped: {}", result.trap().reason) };
    }

    return InstantiationResult { move(main_module_instance_pointer) };
}

Optional<InstantiationError> AbstractMachine::allocate_all_initial_phase(Module const& module, ModuleInstance& module_instance, Vector<ExternValue>& externs, Vector<Value>& global_values, Vector<FunctionAddress>& own_functions)
{
    Optional<InstantiationError> result;

    for (auto& entry : externs) {
        entry.visit(
            [&](FunctionAddress const& address) { module_instance.functions().append(address); },
            [&](TableAddress const& address) { module_instance.tables().append(address); },
            [&](MemoryAddress const& address) { module_instance.memories().append(address); },
            [&](GlobalAddress const& address) { module_instance.globals().append(address); });
    }

    module_instance.functions().extend(own_functions);

    // FIXME: What if this fails?

    for (auto& table : module.table_section().tables()) {
        auto table_address = m_store.allocate(table.type());
        VERIFY(table_address.has_value());
        module_instance.tables().append(*table_address);
    }

    for (auto& memory : module.memory_section().memories()) {
        auto memory_address = m_store.allocate(memory.type());
        VERIFY(memory_address.has_value());
        module_instance.memories().append(*memory_address);
    }

    size_t index = 0;
    for (auto& entry : module.global_section().entries()) {
        auto address = m_store.allocate(entry.type(), move(global_values[index]));
        VERIFY(address.has_value());
        module_instance.globals().append(*address);
        index++;
    }

    for (auto& entry : module.export_section().entries()) {
        Variant<FunctionAddress, TableAddress, MemoryAddress, GlobalAddress, Empty> address {};
        entry.description().visit(
            [&](FunctionIndex const& index) {
                if (module_instance.functions().size() > index.value())
                    address = FunctionAddress { module_instance.functions()[index.value()] };
                else
                    dbgln("Failed to export '{}', the exported address ({}) was out of bounds (min: 0, max: {})", entry.name(), index.value(), module_instance.functions().size());
            },
            [&](TableIndex const& index) {
                if (module_instance.tables().size() > index.value())
                    address = TableAddress { module_instance.tables()[index.value()] };
                else
                    dbgln("Failed to export '{}', the exported address ({}) was out of bounds (min: 0, max: {})", entry.name(), index.value(), module_instance.tables().size());
            },
            [&](MemoryIndex const& index) {
                if (module_instance.memories().size() > index.value())
                    address = MemoryAddress { module_instance.memories()[index.value()] };
                else
                    dbgln("Failed to export '{}', the exported address ({}) was out of bounds (min: 0, max: {})", entry.name(), index.value(), module_instance.memories().size());
            },
            [&](GlobalIndex const& index) {
                if (module_instance.globals().size() > index.value())
                    address = GlobalAddress { module_instance.globals()[index.value()] };
                else
                    dbgln("Failed to export '{}', the exported address ({}) was out of bounds (min: 0, max: {})", entry.name(), index.value(), module_instance.globals().size());
            });

        if (address.has<Empty>()) {
            result = InstantiationError { "An export could not be resolved" };
            continue;
        }

        module_instance.exports().append(ExportInstance {
            entry.name(),
            move(address).downcast<FunctionAddress, TableAddress, MemoryAddress, GlobalAddress>(),
        });
    }

    return result;
}

Optional<InstantiationError> AbstractMachine::allocate_all_final_phase(Module const& module, ModuleInstance& module_instance, Vector<Vector<Reference>>& elements)
{
    size_t index = 0;
    for (auto& segment : module.element_section().segments()) {
        auto address = m_store.allocate(segment.type, move(elements[index]));
        VERIFY(address.has_value());
        module_instance.elements().append(*address);
        index++;
    }

    return {};
}

Result AbstractMachine::invoke(FunctionAddress address, Vector<Value> arguments)
{
    BytecodeInterpreter interpreter(m_stack_info);
    return invoke(interpreter, address, move(arguments));
}

Result AbstractMachine::invoke(Interpreter& interpreter, FunctionAddress address, Vector<Value> arguments)
{
    Configuration configuration { m_store };
    if (m_should_limit_instruction_count)
        configuration.enable_instruction_count_limit();
    return configuration.call(interpreter, address, move(arguments));
}

void Linker::link(ModuleInstance const& instance)
{
    populate();
    if (m_unresolved_imports.is_empty())
        return;

    HashTable<Name> resolved_imports;
    for (auto& import_ : m_unresolved_imports) {
        auto it = instance.exports().find_if([&](auto& export_) { return export_.name() == import_.name; });
        if (!it.is_end()) {
            resolved_imports.set(import_);
            m_resolved_imports.set(import_, it->value());
        }
    }

    for (auto& entry : resolved_imports)
        m_unresolved_imports.remove(entry);
}

void Linker::link(HashMap<Linker::Name, ExternValue> const& exports)
{
    populate();
    if (m_unresolved_imports.is_empty())
        return;

    if (exports.is_empty())
        return;

    HashTable<Name> resolved_imports;
    for (auto& import_ : m_unresolved_imports) {
        auto export_ = exports.get(import_);
        if (export_.has_value()) {
            resolved_imports.set(import_);
            m_resolved_imports.set(import_, export_.value());
        }
    }

    for (auto& entry : resolved_imports)
        m_unresolved_imports.remove(entry);
}

AK::ErrorOr<Vector<ExternValue>, LinkError> Linker::finish()
{
    populate();
    if (!m_unresolved_imports.is_empty()) {
        if (!m_error.has_value())
            m_error = LinkError {};
        for (auto& entry : m_unresolved_imports)
            m_error->missing_imports.append(entry.name);
        return *m_error;
    }

    if (m_error.has_value())
        return *m_error;

    // Result must be in the same order as the module imports
    Vector<ExternValue> exports;
    exports.ensure_capacity(m_ordered_imports.size());
    for (auto& import_ : m_ordered_imports)
        exports.unchecked_append(*m_resolved_imports.get(import_));
    return exports;
}

void Linker::populate()
{
    if (!m_ordered_imports.is_empty())
        return;

    for (auto& import_ : m_module.import_section().imports()) {
        m_ordered_imports.append({ import_.module(), import_.name(), import_.description() });
        m_unresolved_imports.set(m_ordered_imports.last());
    }
}
}
