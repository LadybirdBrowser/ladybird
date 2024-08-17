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

void Configuration::unwind(Badge<CallFrameHandle>, CallFrameHandle const& frame_handle)
{
    auto frame = m_frame_stack.take_last();
    m_depth--;
    m_ip = frame_handle.ip;
}

Result Configuration::call(Interpreter& interpreter, FunctionAddress address, Vector<Value> arguments)
{
    auto* function = m_store.get(address);
    if (!function)
        return Trap {};
    if (auto* wasm_function = function->get_pointer<WasmFunction>()) {
        Vector<Value> locals = move(arguments);
        locals.ensure_capacity(locals.size() + wasm_function->code().func().locals().size());
        for (auto& local : wasm_function->code().func().locals()) {
            for (size_t i = 0; i < local.n(); ++i)
                locals.append(Value(local.type()));
        }

        set_frame(Frame {
            wasm_function->module(),
            move(locals),
            wasm_function->code().func().body(),
            wasm_function->type().results().size(),
        });
        m_ip = 0;
        return execute(interpreter);
    }

    // It better be a host function, else something is really wrong.
    auto& host_function = function->get<HostFunction>();
    return host_function.function()(*this, arguments);
}

Result Configuration::execute(Interpreter& interpreter)
{
    interpreter.interpret(*this);
    if (interpreter.did_trap())
        return Trap { interpreter.trap_reason() };

    Vector<Value> results;
    results.ensure_capacity(frame().arity());
    for (size_t i = 0; i < frame().arity(); ++i)
        results.unchecked_append(value_stack().take_last());

    label_stack().take_last();
    return Result { move(results) };
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
