/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Interpreter.h>
#include <LibWasm/Printer/Printer.h>

namespace Wasm {

void Configuration::unwind_impl()
{
    if (m_compiled_direct_call_depth > 0) {
        m_compiled_direct_call_depth--;
        m_depth--;
        return;
    }
    auto last_frame = m_frame_stack.take_last();
    m_depth--;

    m_locals_base = m_frame_stack.is_empty() ? nullptr : m_frame_stack.last().locals_data();
    if (m_frame_stack.is_empty()) {
        m_default_memory = nullptr;
        m_default_memory_base = nullptr;
    } else {
        auto const& memories = m_frame_stack.last().module().memories();
        m_default_memory = memories.is_empty() ? nullptr : m_store.unsafe_get(memories[0]);
        m_default_memory_base = m_default_memory ? m_default_memory->data().data() : nullptr;
    }

    if (!last_frame.owns_locals()) {
        // Non-owning frame: just restore the caller's runtime state.
        return;
    }

    // Owning frame: full cleanup.
    release_arguments_allocation(last_frame.owned_locals(), m_locals_base != nullptr);
}

Result Configuration::call(Interpreter& interpreter, FunctionAddress address, Vector<Value, ArgumentsStaticSize>& arguments)
{
    if (auto fn = TRY(prepare_call(address, arguments)); fn.has_value())
        return fn->function()(*this, arguments.span());
    m_ip = 0;
    return execute(interpreter);
}

ErrorOr<Optional<HostFunction&>, Trap> Configuration::prepare_call(FunctionAddress address, Vector<Value, ArgumentsStaticSize>& arguments, bool is_tailcall)
{
    auto* function = m_store.get(address);
    if (!function)
        return Trap::from_string("Attempt to call nonexistent function by address");

    if (auto* wasm_function = function->get_pointer<WasmFunction>()) {
        TRY(prepare_wasm_call(*wasm_function, arguments, is_tailcall));
        return OptionalNone {};
    }

    return function->get<HostFunction>();
}

Result Configuration::execute(Interpreter& interpreter)
{
    interpreter.interpret(*this);
    if (interpreter.did_trap())
        return interpreter.trap();

    Vector<Value> results { value_stack().span().slice_from_end(frame().arity()) };
    value_stack().shrink(value_stack().size() - results.size(), true);
    results.reverse();

    // If we reached here from a tailcall -> return, we might not have a label to pop (because the return already popped it)
    if (!label_stack().is_empty())
        label_stack().take_last();

    return Result { move(results) };
}

ErrorOr<void, Trap> Configuration::execute_for_compiled_call(Interpreter& interpreter, Value* single_result)
{
    interpreter.interpret(*this);
    if (interpreter.did_trap())
        return interpreter.trap();

    VERIFY(frame().arity() <= 1);
    if (frame().arity() == 1) {
        auto result = value_stack().unsafe_take_last();
        if (single_result)
            *single_result = result;
    }

    if (!label_stack().is_empty())
        label_stack().take_last();

    return {};
}

void Configuration::build_compiled_function_table()
{
    if (m_frame_stack.is_empty())
        return;
    auto const* current_module = &frame().module();
    if (m_compiled_fn_table_module == current_module && !m_compiled_fn_table.is_empty())
        return;

    m_compiled_fn_table.clear();
    m_compiled_fn_table_module = current_module;

    auto const& functions = frame().module().functions();
    auto count = functions.size();
    if (count == 0)
        return;

    for (size_t i = 0; i < count; i++) {
        auto* instance = m_store.unsafe_get(functions[i]);
        auto* wasm_fn = instance->get_pointer<WasmFunction>();
        if (!wasm_fn)
            continue;
        auto& ci = wasm_fn->code().func().body().compiled_instructions;
        if (!ci.cranelift_compiled)
            continue;
        CompiledFunctionEntry entry;
        entry.handler_ptr = ci.dispatches[0].handler_ptr;
        entry.dispatches_ptr = bit_cast<FlatPtr>(ci.dispatches.data());
        entry.src_dst_ptr = bit_cast<FlatPtr>(ci.src_dst_mappings.data());
        entry.first_insn = ci.dispatches[0].instruction;
        entry.expression = &wasm_fn->code().func().body();
        entry.module = &wasm_fn->module();
        entry.total_local_count = static_cast<u32>(wasm_fn->code().func().total_local_count());
        entry.arity = static_cast<u32>(wasm_fn->type().results().size());
        entry.max_call_rec_size = static_cast<u32>(ci.max_call_rec_size);
        m_compiled_fn_table.set(static_cast<u32>(i), entry);
    }
}

void Configuration::dump_stack()
{
    auto print_value = []<typename... Ts>(CheckedFormatString<Ts...> format, Ts... vs) {
        AllocatingMemoryStream memory_stream;
        Printer { memory_stream }.print(vs...);
        auto buffer = ByteBuffer::create_uninitialized(memory_stream.used_buffer_size()).release_value_but_fixme_should_propagate_errors();
        memory_stream.read_until_filled(buffer).release_value_but_fixme_should_propagate_errors();
        dbgln(format.view(), StringView(buffer).trim_whitespace());
    };
    for (auto const& value : value_stack()) {
        print_value("    {}", value);
    }
}

}
