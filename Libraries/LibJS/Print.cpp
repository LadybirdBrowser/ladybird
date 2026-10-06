/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Print.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

namespace {

// Where the runtime writes the printed value: the context's builder if it has one, and its stream otherwise.
struct PrintedBytesDestination {
    PrintContext& print_context;
    // The builder receives the printed text once it is complete, because a write may end inside a code point.
    Vector<u8> bytes_for_the_builder;
    Optional<AK::Error> stream_error;
};

}

static bool append_printed_bytes(void* context, u8 const* bytes, size_t length)
{
    auto& destination = *static_cast<PrintedBytesDestination*>(context);
    if (destination.print_context.builder) {
        destination.bytes_for_the_builder.append(bytes, length);
        return true;
    }

    VERIFY(destination.print_context.stream);
    if (auto result = destination.print_context.stream->write_until_depleted({ bytes, length }); result.is_error()) {
        destination.stream_error = result.release_error();
        return false;
    }
    return true;
}

ErrorOr<void> print(Value value, PrintContext& print_context)
{
    PrintedBytesDestination destination { .print_context = print_context, .bytes_for_the_builder = {}, .stream_error = {} };
    JSByteSink sink { .context = &destination, .append = append_printed_bytes };

    if (!js_console_print_value(vm_to_abi(print_context.vm), value_to_abi(value), &sink, print_context.strip_ansi, print_context.raw_strings))
        return destination.stream_error.release_value();

    if (print_context.builder) {
        // The runtime encodes a lone surrogate as a code point of its own, which the builder appends as one code unit.
        for (auto code_point : Utf8View { StringView { destination.bytes_for_the_builder.span() } })
            print_context.builder->append_code_point(code_point);
    }
    return {};
}

}
