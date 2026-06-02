/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Interpreter.h>
#include <LibWasm/AbstractMachine/Validator.h>
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

ErrorOr<void, Trap> Configuration::prepare_wasm_call(WasmFunction const& wasm_function, Vector<Value, ArgumentsStaticSize>& arguments, bool is_tailcall)
{
    if (auto module = wasm_function.module_ref()) {
        if (auto result = ensure_cranelift_compiled(const_cast<Module&>(*module)); result.is_error())
            return Trap::from_string(ByteString::formatted("Cranelift compilation failed: {}", result.error().error_string));
    }

    if (is_tailcall)
        unwind_impl();

    arguments.ensure_capacity(arguments.size() + wasm_function.code().func().total_local_count());
    for (auto const& local : wasm_function.code().func().locals()) {
        for (size_t i = 0; i < local.n(); ++i)
            arguments.unchecked_append(Value(local.type()));
    }

    set_frame(
        is_tailcall ? IsTailcall::Yes : IsTailcall::No,
        wasm_function.module(),
        move(arguments),
        wasm_function.code().func().body(),
        wasm_function.type().results().size());
    return {};
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
