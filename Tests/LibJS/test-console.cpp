/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Console.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>

class RecordingConsoleClient final : public JS::ConsoleClient {
    GC_CELL(RecordingConsoleClient, JS::ConsoleClient);
    GC_DECLARE_ALLOCATOR(RecordingConsoleClient);

public:
    explicit RecordingConsoleClient(JS::Console& console)
        : ConsoleClient(console)
    {
    }

    Vector<Vector<JS::Value>> const& printed() const { return m_printed; }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        for (auto const& values : m_printed)
            visitor.visit(values.span());
    }

    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel, PrinterArguments arguments) override
    {
        if (auto const* values = arguments.get_pointer<GC::RootVector<JS::Value>>())
            m_printed.append(Vector<JS::Value> { values->span() });
        return JS::js_undefined();
    }

    virtual void clear() override { }
    virtual void end_group() override { }

    Vector<Vector<JS::Value>> m_printed;
};

GC_DEFINE_ALLOCATOR(RecordingConsoleClient);

static void expect_printed(StringView method, StringView arguments, StringView expected)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto& console = realm.intrinsics().console_object()->console();
    auto client = vm->heap().allocate<RecordingConsoleClient>(console);
    console.set_client(*client);

    auto source = Utf16String::formatted("var args = [{}]; console.{}(...args); [{}];", arguments, method, expected);
    auto script_or_error = JS::Script::parse(source.utf16_view(), realm);
    VERIFY(!script_or_error.is_error());

    auto result = vm->run(*script_or_error.value());
    EXPECT(!result.is_error());
    if (result.is_error())
        return;

    auto& expected_values = result.value().as_object();
    auto expected_size = MUST(JS::length_of_array_like(*vm, expected_values));

    EXPECT_EQ(client->printed().size(), 1u);
    if (client->printed().size() != 1)
        return;

    auto const& printed = client->printed().first();
    EXPECT_EQ(printed.size(), expected_size);
    for (size_t i = 0; i < min(printed.size(), expected_size); ++i)
        EXPECT(JS::same_value(printed[i], MUST(expected_values.get(i))));
}

TEST_CASE(non_string_first_argument_is_not_a_format_string)
{
    expect_printed("log"sv, "Object.create(null), 1"sv, "...args"sv);
    expect_printed("log"sv, "Symbol('%s'), 1"sv, "...args"sv);
    expect_printed("log"sv, "{ toString() { throw new Error('ToString was called'); } }, 1"sv, "...args"sv);
    expect_printed("log"sv, "['%s'], 1"sv, "...args"sv);
    expect_printed("log"sv, "new String('%s=%d'), 'a', 1"sv, "...args"sv);
    expect_printed("log"sv, "42, '%s', 1"sv, "...args"sv);
}

TEST_CASE(non_string_first_argument_is_not_a_format_string_for_any_logger_method)
{
    for (auto method : { "debug"sv, "error"sv, "info"sv, "log"sv, "warn"sv })
        expect_printed(method, "Object.create(null), '%s', 1"sv, "...args"sv);
}

TEST_CASE(string_first_argument_is_a_format_string)
{
    expect_printed("log"sv, "'%s=%d', 'a', 1"sv, "'a=1'"sv);
    expect_printed("log"sv, "'%i and %f', '12px', '1.5em'"sv, "'12 and 1.5'"sv);
    expect_printed("log"sv, "'%o!', 5, 'rest'"sv, "'5!', 'rest'"sv);
    expect_printed("log"sv, "'%cstyled', 'color: red'"sv, "'styled'"sv);
    expect_printed("log"sv, "'no specifiers', 'a', 1"sv, "...args"sv);
}
