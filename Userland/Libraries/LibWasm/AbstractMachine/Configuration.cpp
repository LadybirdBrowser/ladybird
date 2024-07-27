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

Optional<size_t> Configuration::nth_label_index(size_t i)
{
    for (size_t index = m_stack.size(); index > 0; --index) {
        auto& entry = m_stack.entries()[index - 1];
        if (entry.has<Label>()) {
            if (i == 0)
                return index - 1;
            --i;
        }
    }
    return {};
}

void Configuration::unwind(Badge<CallFrameHandle>, CallFrameHandle const& frame_handle)
{
    if (m_stack.size() == frame_handle.stack_size && frame_handle.frame_index == m_current_frame_index)
        return;

    VERIFY(m_stack.size() > frame_handle.stack_size);
    m_stack.entries().remove(frame_handle.stack_size, m_stack.size() - frame_handle.stack_size);
    m_current_frame_index = frame_handle.frame_index;
    m_depth--;
    m_ip = frame_handle.ip;
    VERIFY(m_stack.size() == frame_handle.stack_size);
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
                locals.empend(local.type(), 0ull);
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

    if (stack().size() <= frame().arity() + 1)
        return Trap { "Not enough values to return from call" };

    Vector<Value> results;
    results.ensure_capacity(frame().arity());
    for (size_t i = 0; i < frame().arity(); ++i)
        results.append(move(stack().pop().get<Value>()));
    auto label = stack().pop();
    // ASSERT: label == current frame
    if (!label.has<Label>())
        return Trap { "Invalid stack configuration" };
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
    for (auto const& entry : stack().entries()) {
        entry.visit(
            [&](Value const& v) {
                print_value("    {}", v);
            },
            [&](Frame const& f) {
                dbgln("    frame({})", f.arity());
                for (auto& local : f.locals()) {
                    print_value("        {}", local);
                }
            },
            [](Label const& l) {
                dbgln("    label({}) -> {}", l.arity(), l.continuation());
            });
    }
}

}
