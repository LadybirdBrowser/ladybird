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
    m_frame_stack.take_last();
    m_depth--;
    m_locals_base = m_frame_stack.is_empty() ? nullptr : m_frame_stack.unchecked_last().locals().data();
    m_arguments_base = m_frame_stack.is_empty() ? nullptr : m_frame_stack.unchecked_last().arguments().data();
}

Result Configuration::call(Interpreter& interpreter, FunctionAddress address, Vector<Value, 8> arguments)
{
    if (auto fn = TRY(prepare_call(address, arguments)); fn.has_value())
        return fn->function()(*this, arguments.span());
    m_ip = 0;
    return execute(interpreter);
}

ErrorOr<Optional<HostFunction&>, Trap> Configuration::prepare_call(FunctionAddress address, Vector<Value, 8>& arguments, bool is_tailcall)
{
    auto* function = m_store.get(address);
    if (!function)
        return Trap::from_string("Attempt to call nonexistent function by address");

    if (auto* wasm_function = function->get_pointer<WasmFunction>()) {
        if (is_tailcall)
            unwind_impl(); // Unwind the current frame, the "return" in the tail-called function will unwind the frame we're gonna push now.
        Vector<Value, 8> locals;
        locals.ensure_capacity(wasm_function->code().func().total_local_count());
        for (auto& local : wasm_function->code().func().locals()) {
            for (size_t i = 0; i < local.n(); ++i)
                locals.unchecked_append(Value(local.type()));
        }

        set_frame(Frame {
                      wasm_function->module(),
                      move(arguments),
                      move(locals),
                      wasm_function->code().func().body(),
                      wasm_function->type().results().size(),
                  },
            is_tailcall);
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
