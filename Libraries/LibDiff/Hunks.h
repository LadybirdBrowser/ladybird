/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Diff {

struct Range {
    size_t start_line { 0 };
    size_t number_of_lines { 0 };
};

struct HunkLocation {
    Range old_range;
    Range new_range;
};

struct Line {
    enum class Operation {
        Addition = '+',
        Removal = '-',
        Context = ' ',
    };

    static constexpr Operation operation_from_symbol(char symbol)
    {
        switch (symbol) {
        case '+':
            return Operation::Addition;
        case '-':
            return Operation::Removal;
        case ' ':
            return Operation::Context;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    Operation operation;
    String content;
};

struct Hunk {
    HunkLocation location;
    Vector<Line> lines;
};

}

template<>
struct AK::Formatter<Diff::Line::Operation> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Diff::Line::Operation operation)
    {
        return Formatter<FormatString>::format(builder, "{}"sv, static_cast<char>(operation));
    }
};

template<>
struct AK::Formatter<Diff::Line> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Diff::Line const& line)
    {
        return Formatter<FormatString>::format(builder, "{}{}"sv, line.operation, line.content);
    }
};

template<>
struct AK::Formatter<Diff::HunkLocation> : Formatter<FormatString> {
    static ErrorOr<void> format(FormatBuilder& format_builder, Diff::HunkLocation const& location)
    {
        auto& builder = format_builder.builder();
        TRY(builder.try_appendff("@@ -{}", location.old_range.start_line));

        if (location.old_range.number_of_lines != 1)
            TRY(builder.try_appendff(",{}", location.old_range.number_of_lines));

        TRY(builder.try_appendff(" +{}", location.new_range.start_line));

        if (location.new_range.number_of_lines != 1)
            TRY(builder.try_appendff(",{}", location.new_range.number_of_lines));

        return builder.try_appendff(" @@");
    }
};
