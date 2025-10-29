/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/AST.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/SourceRange.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Error);

static SourceRange dummy_source_range { SourceCode::create({}, {}), {}, {} };

SourceRange const& TracebackFrame::source_range() const
{
    if (!cached_source_range)
        return dummy_source_range;
    if (auto* unrealized = cached_source_range->source_range.get_pointer<UnrealizedSourceRange>()) {
        auto source_range = [&] {
            if (!unrealized->source_code)
                return dummy_source_range;
            return unrealized->realize();
        }();
        cached_source_range->source_range = move(source_range);
    }
    return cached_source_range->source_range.get<SourceRange>();
}

GC::Ref<Error> Error::create(Realm& realm)
{
    return realm.create<Error>(realm.intrinsics().error_prototype());
}

GC::Ref<Error> Error::create(Realm& realm, Utf16String message)
{
    auto error = Error::create(realm);
    error->set_message(move(message));
    return error;
}

GC::Ref<Error> Error::create(Realm& realm, StringView message)
{
    return create(realm, Utf16String::from_utf8(message));
}

Error::Error(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
    populate_stack();
}

// 20.5.8.1 InstallErrorCause ( O, options ), https://tc39.es/ecma262/#sec-installerrorcause
ThrowCompletionOr<void> Error::install_error_cause(Value options)
{
    auto& vm = this->vm();

    // 1. If Type(options) is Object and ? HasProperty(options, "cause") is true, then
    if (options.is_object() && TRY(options.as_object().has_property(vm.names.cause))) {
        // a. Let cause be ? Get(options, "cause").
        auto cause = TRY(options.as_object().get(vm.names.cause));

        // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "cause", cause).
        create_non_enumerable_data_property_or_throw(vm.names.cause, cause);
    }

    // 2. Return unused.
    return {};
}

void Error::set_message(Utf16String message)
{
    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_direct_property(vm.names.message, PrimitiveString::create(vm, move(message)), attr);
}

void Error::populate_stack()
{
    auto stack_trace = vm().stack_trace();
    m_traceback.ensure_capacity(stack_trace.size());
    for (auto& element : stack_trace) {
        auto* context = element.execution_context;
        TracebackFrame frame {
            .function_name = context->function ? context->function->name_for_call_stack() : ""_utf16,
            .cached_source_range = element.source_range,
        };

        m_traceback.append(move(frame));
    }
}

String Error::stack_string(CompactTraceback compact) const
{
    if (m_traceback.is_empty())
        return {};

    StringBuilder stack_string_builder;

    // Note: We roughly follow V8's formatting
    auto append_frame = [&](TracebackFrame const& frame) {
        auto function_name = frame.function_name;
        auto source_range = frame.source_range();
        // Note: Since we don't know whether we have a valid SourceRange here we just check for some default values.
        if (!source_range.filename().is_empty() || source_range.start.offset != 0 || source_range.end.offset != 0) {

            if (function_name.is_empty())
                stack_string_builder.appendff("    at {}:{}:{}\n", source_range.filename(), source_range.start.line, source_range.start.column);
            else
                stack_string_builder.appendff("    at {} ({}:{}:{})\n", function_name, source_range.filename(), source_range.start.line, source_range.start.column);
        } else {
            stack_string_builder.appendff("    at {}\n", function_name.is_empty() ? "<unknown>"_utf16 : function_name);
        }
    };

    auto is_same_frame = [](TracebackFrame const& a, TracebackFrame const& b) {
        if (a.function_name.is_empty() && b.function_name.is_empty()) {
            auto source_range_a = a.source_range();
            auto source_range_b = b.source_range();
            return source_range_a.filename() == source_range_b.filename() && source_range_a.start.line == source_range_b.start.line;
        }
        return a.function_name == b.function_name;
    };

    // Note: We don't want to capture the global execution context, so we omit the last frame
    // Note: The error's name and message get prepended by ErrorPrototype::stack
    // FIXME: We generate a stack-frame for the Errors constructor, other engines do not
    unsigned repetitions = 0;
    size_t used_frames = m_traceback.size() - 1;
    for (size_t i = 0; i < used_frames; ++i) {
        auto const& frame = m_traceback[i];
        if (compact == CompactTraceback::Yes && i + 1 < used_frames) {
            auto const& next_traceback_frame = m_traceback[i + 1];
            if (is_same_frame(frame, next_traceback_frame)) {
                repetitions++;
                continue;
            }
        }
        if (repetitions > 4) {
            // If more than 5 (1 + >4) consecutive function calls with the same name, print
            // the name only once and show the number of repetitions instead. This prevents
            // printing ridiculously large call stacks of recursive functions.
            append_frame(frame);
            stack_string_builder.appendff("    {} more calls\n", repetitions);
        } else {
            for (size_t j = 0; j < repetitions + 1; j++)
                append_frame(frame);
        }
        repetitions = 0;
    }
    for (size_t j = 0; j < repetitions; j++)
        append_frame(m_traceback[used_frames - 1]);

    return MUST(stack_string_builder.to_string());
}

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    GC_DEFINE_ALLOCATOR(ClassName);                                                      \
    GC::Ref<ClassName> ClassName::create(Realm& realm)                                   \
    {                                                                                    \
        return realm.create<ClassName>(realm.intrinsics().snake_name##_prototype());     \
    }                                                                                    \
                                                                                         \
    GC::Ref<ClassName> ClassName::create(Realm& realm, Utf16String message)              \
    {                                                                                    \
        auto error = ClassName::create(realm);                                           \
        error->set_message(move(message));                                               \
        return error;                                                                    \
    }                                                                                    \
                                                                                         \
    GC::Ref<ClassName> ClassName::create(Realm& realm, StringView message)               \
    {                                                                                    \
        return create(realm, Utf16String::from_utf8(message));                           \
    }                                                                                    \
                                                                                         \
    ClassName::ClassName(Object& prototype)                                              \
        : Error(prototype)                                                               \
    {                                                                                    \
    }

JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE

}
