/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Format.h>
#include <AK/GenericLexer.h>
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

        // NOTE: This should only be used when deconstructing a hunk into old and new lines (context format)
        Change = '!',
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

enum class Format {
    Unified,
    Unknown,
};

struct Header {
    Format format { Format::Unknown };

    String old_file_path;
    String new_file_path;
};

struct Patch {
    Header header;
    Vector<Hunk> hunks;
};

class Parser : public GenericLexer {
public:
    using GenericLexer::GenericLexer;

    ErrorOr<Patch> parse_patch(Optional<size_t> const& strip_count = {});

    ErrorOr<Vector<Hunk>> parse_hunks();

private:
    ErrorOr<Header> parse_header(Optional<size_t> const& strip_count);

    ErrorOr<String> parse_file_line(Optional<size_t> const& strip_count);
    Optional<HunkLocation> consume_unified_location();
    bool consume_line_number(size_t& number);
};

ErrorOr<Vector<Hunk>> parse_hunks(StringView diff);

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
        TRY(builder.try_appendff("@@ -{}"sv, location.old_range.start_line));

        if (location.old_range.number_of_lines != 1)
            TRY(builder.try_appendff(",{}", location.old_range.number_of_lines));

        TRY(builder.try_appendff(" +{}", location.new_range.start_line));

        if (location.new_range.number_of_lines != 1)
            TRY(builder.try_appendff(",{}", location.new_range.number_of_lines));

        return builder.try_appendff(" @@");
    }
};
