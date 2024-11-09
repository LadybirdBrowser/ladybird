/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/IterationDecision.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Core::Environment {

char** raw_environ();

struct Entry {
    StringView full_entry;
    StringView name;
    StringView value;

    static Entry from_chars(char const* input);
    static Entry from_string(StringView input);
};

class EntryIterator {
public:
    constexpr bool operator==(EntryIterator other) const { return m_index == other.m_index; }
    constexpr bool operator!=(EntryIterator other) const { return m_index != other.m_index; }

    constexpr EntryIterator operator++()
    {
        ++m_index;
        return *this;
    }
    constexpr EntryIterator operator++(int)
    {
        auto result = *this;
        ++m_index;
        return result;
    }

    Entry operator*() { return Entry::from_chars(raw_environ()[m_index]); }

    static EntryIterator begin();
    static EntryIterator end();

private:
    explicit constexpr EntryIterator(size_t index)
        : m_index(index)
    {
    }

    size_t m_index { 0 };
};

EntryIterator entries();

size_t size();

bool has(StringView name);
enum class SecureOnly {
    No,
    Yes,
};
Optional<StringView> get(StringView name, SecureOnly = SecureOnly::No);

enum class Overwrite {
    No,
    Yes,
};
ErrorOr<void> set(StringView name, StringView value, Overwrite);
ErrorOr<void> unset(StringView name);
ErrorOr<void> put(StringView env);

ErrorOr<void> clear();

}
