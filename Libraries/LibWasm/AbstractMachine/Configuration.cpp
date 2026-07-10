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
    m_depth--;

    Optional<Vector<Value, ArgumentsStaticSize>> released_locals;
    auto const* popped_module = &m_frame_stack.last().module();
    if (m_frame_stack.last().owns_locals())
        released_locals = m_owned_locals_stack.take_last();
    m_frame_stack.remove(m_frame_stack.size() - 1);

    if (m_frame_stack.is_empty()) {
        m_locals_base = nullptr;
        m_default_memory = nullptr;
    } else {
        auto& caller = m_frame_stack.last();
        m_locals_base = caller.owns_locals() ? m_owned_locals_stack.last().data() : caller.locals_data();
        if (&caller.module() != popped_module) {
            auto const& memories = caller.module().memories();
            m_default_memory = memories.is_empty() ? nullptr : m_store.unsafe_get(memories[0]);
        }
    }

    if (released_locals.has_value())
        release_arguments_allocation(released_locals.value(), m_locals_base != nullptr);
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
    // Tier-0 by default: don't block the call waiting for native compilation. Non-Web embedders
    // compile synchronously at instantiate time (so the JIT is already live here); the Web path
    // compiles in the background and the interpreter picks up the native entry on a later call
    // once it's published. Either way, execution falls back to the interpreter until then.
    if (is_tailcall)
        unwind_impl();

    auto inlined_locals = wasm_function.code().func().body().compiled_instructions.cranelift_inlined_locals;
    arguments.ensure_capacity(arguments.size() + wasm_function.code().func().total_local_count() + inlined_locals);
    for (auto const& local : wasm_function.code().func().locals()) {
        for (size_t i = 0; i < local.n(); ++i)
            arguments.unchecked_append(Value(local.type()));
    }
    for (u32 i = 0; i < inlined_locals; ++i)
        arguments.unchecked_append(Value(ValueType { ValueType::I32 }));

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
