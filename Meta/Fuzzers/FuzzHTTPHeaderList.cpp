/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <LibHTTP/HeaderList.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size > 65536)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    constexpr StringView names[] = { "Content-Length"sv, "Set-Cookie"sv, "Vary"sv, "Content-Range"sv, "Access-Control-Allow-Origin"sv, "Cache-Control"sv, "Content-Type"sv, "X-Fuzz"sv };
    auto headers = HTTP::HeaderList::create();
    for (size_t step = 0; step < 128 && !input.remaining().is_empty(); ++step) {
        auto operation = input.byte() % 5;
        auto name = names[input.byte() % array_size(names)];
        auto value_bytes = input.take(input.byte());
        ByteString value(reinterpret_cast<char const*>(value_bytes.data()), value_bytes.size());
        auto lower = ByteString(name).to_lowercase();
        HTTP::Header header { lower, value };
        switch (operation) {
        case 0:
            headers->append(move(header));
            break;
        case 1: {
            headers->set(move(header));
            auto actual = headers->get(name);
            VERIFY(actual.has_value() && actual.value() == value);
            size_t matches = 0;
            for (auto const& entry : *headers) {
                if (entry.name.equals_ignoring_ascii_case(name))
                    ++matches;
            }
            VERIFY(matches == 1);
            break;
        }
        case 2:
            headers->delete_(name);
            VERIFY(!headers->contains(lower));
            break;
        case 3:
            headers->combine(move(header));
            break;
        case 4:
            headers->clear();
            VERIFY(headers->is_empty());
            break;
        }
        VERIFY(headers->get(name) == headers->get(lower));
        (void)headers->get_decode_and_split(name);
        (void)headers->extract_header_list_values(name);
        (void)headers->extract_length();
        (void)headers->extract_content_range_values();
        auto combined = HTTP::HeaderList::create(headers->sort_and_combine());
        for (auto query : names)
            VERIFY(headers->get(query) == combined->get(query));
    }
    return 0;
}
