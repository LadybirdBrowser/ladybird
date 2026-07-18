/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibCore/File.h>
#include <LibTest/TestCase.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/Constants.h>

TEST_CASE(compiled_to_interpreter_call_restores_label_stack)
{
    auto file = MUST(Core::File::open("Fixtures/label-stack-cleanup.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    Wasm::FunctionType label_stack_size_type { {}, { Wasm::ValueType(Wasm::ValueType::I32) } };
    auto label_stack_size = machine.store().allocate(Wasm::HostFunction {
        [](Wasm::Configuration& configuration, Span<Wasm::Value>) -> Wasm::Result {
            Vector<Wasm::Value> result;
            result.append(Wasm::Value(static_cast<i32>(configuration.label_stack().size())));
            return Wasm::Result { move(result) };
        },
        label_stack_size_type,
        "label_stack_size" });
    VERIFY(label_stack_size.has_value());

    Vector<Wasm::ExternValue> imports;
    imports.append(*label_stack_size);
    auto instance = MUST(machine.instantiate(*module, move(imports)));

    Optional<Wasm::FunctionAddress> run;
    for (auto const& export_ : instance->exports()) {
        if (export_.name() == "run"sv)
            run = export_.value().get<Wasm::FunctionAddress>();
    }
    VERIFY(run.has_value());

    auto result = machine.invoke(*run, {});
    EXPECT(!result.is_trap());
    EXPECT_EQ(result.values().size(), 1u);
    EXPECT_EQ(result.values()[0].to<i32>(), 1);
}

TEST_CASE(compiled_memory_access_traps_out_of_bounds)
{
    auto file = MUST(Core::File::open("Fixtures/memory-guard-trap.wasm"sv, Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    FixedMemoryStream stream { bytes.bytes() };
    auto module = MUST(Wasm::Module::parse(stream));

    Wasm::AbstractMachine machine;
    auto instance = MUST(machine.instantiate(*module, {}));

    auto find_export = [&](StringView name) {
        Optional<Wasm::FunctionAddress> address;
        for (auto const& export_ : instance->exports()) {
            if (export_.name() == name)
                address = export_.value().get<Wasm::FunctionAddress>();
        }
        VERIFY(address.has_value());
        return *address;
    };
    auto load = find_export("load"sv);
    auto store = find_export("store"sv);
    auto load_high = find_export("load_high"sv);

    auto invoke = [&](Wasm::FunctionAddress address, Vector<Wasm::Value> arguments) {
        return machine.invoke(address, move(arguments));
    };

    // The memory starts at one page; accesses fully inside it should succeed.
    auto in_bounds = invoke(load, { Wasm::Value(static_cast<i32>(0)) });
    EXPECT(!in_bounds.is_trap());
    EXPECT_EQ(in_bounds.values()[0].to<i32>(), 0);

    auto store_in_bounds = invoke(store, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size - 4)), Wasm::Value(static_cast<i32>(0x1337)) });
    EXPECT(!store_in_bounds.is_trap());
    auto load_back = invoke(load, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size - 4)) });
    EXPECT(!load_back.is_trap());
    EXPECT_EQ(load_back.values()[0].to<i32>(), 0x1337);

    auto expect_oob_trap = [](Wasm::Result const& result) {
        EXPECT(result.is_trap());
        EXPECT_EQ(result.trap().format(), "Memory access out of bounds"sv);
    };

    // Loads and stores just-or-partially past the boundary should trap.
    expect_oob_trap(invoke(load, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size - 1)) }));
    expect_oob_trap(invoke(load, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size)) }));
    // A store just past the committed boundary must not write outside the memory either.
    expect_oob_trap(invoke(store, { Wasm::Value(static_cast<i32>(Wasm::Constants::page_size)), Wasm::Value(static_cast<i32>(0x42)) }));

    // base + offset can reach high into the guarded reservation; those accesses must trap too.
    expect_oob_trap(invoke(load_high, { Wasm::Value(static_cast<i32>(0)) }));
    expect_oob_trap(invoke(load_high, { Wasm::Value(static_cast<i32>(0xffffffff)) }));
}
