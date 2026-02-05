/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>

namespace HTTP {

// https://fetch.spec.whatwg.org/#concept-header-list
class HeaderList final : public RefCounted<HeaderList> {
public:
    static NonnullRefPtr<HeaderList> create(Vector<Header> = {});

    Vector<Header> const& headers() const { return m_headers; }

    [[nodiscard]] bool is_empty() const { return m_headers.is_empty(); }

    [[nodiscard]] auto begin() { return m_headers.begin(); }
    [[nodiscard]] auto begin() const { return m_headers.begin(); }

    [[nodiscard]] auto end() { return m_headers.end(); }
    [[nodiscard]] auto end() const { return m_headers.end(); }

    void clear() { m_headers.clear(); }

    [[nodiscard]] bool contains(StringView) const;
    Optional<ByteString> get(StringView) const;
    Optional<Vector<String>> get_decode_and_split(StringView) const;
    void append(Header);
    void delete_(StringView name);
    void set(Header);
    void combine(Header);
    [[nodiscard]] Vector<Header> sort_and_combine() const;

    struct ExtractHeaderParseFailure { };
    [[nodiscard]] Variant<Empty, Vector<ByteString>, ExtractHeaderParseFailure> extract_header_list_values(StringView) const;

    struct ExtractLengthFailure { };
    [[nodiscard]] Variant<Empty, u64, ExtractLengthFailure> extract_length() const;

    struct ExtractContentRangeFailure { };
    struct ContentRangeValues {
        u64 first_byte_pos { 0 };
        u64 last_byte_pos { 0 };
        Optional<u64> complete_length;
    };
    [[nodiscard]] Variant<ContentRangeValues, ExtractContentRangeFailure> extract_content_range_values() const;

    [[nodiscard]] Vector<ByteString> unique_names() const;

    template<typename Callback>
    void delete_all_matching(Callback&& callback)
    {
        m_headers.remove_all_matching(forward<Callback>(callback));
    }

    template<typename Callback>
    void for_each_header_value(StringView name, Callback&& callback) const
    {
        for (auto const& header : m_headers) {
            if (!header.name.equals_ignoring_ascii_case(name))
                continue;
            if (callback(header.value) == IterationDecision::Break)
                break;
        }
    }

    template<typename Callback>
    void for_each_vary_header(Callback&& callback) const
    {
        for_each_header_value("Vary"sv, [&](StringView value) -> IterationDecision {
            IterationDecision result;

            value.for_each_split_view(","sv, SplitBehavior::Nothing, [&](StringView header) -> IterationDecision {
                result = callback(header.trim_whitespace());
                return result;
            });

            return result;
        });
    }

private:
    explicit HeaderList(Vector<Header>);

    Vector<Header> m_headers;
};

}
