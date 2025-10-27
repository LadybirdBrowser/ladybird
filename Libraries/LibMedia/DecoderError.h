/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/SourceLocation.h>
#include <LibMedia/Forward.h>
#include <errno.h>

namespace Media {

template<typename T>
using DecoderErrorOr = ErrorOr<T, DecoderError>;

enum class DecoderErrorCategory : u32 {
    Unknown,
    IO,
    NeedsMoreInput,
    EndOfStream,
    Memory,
    // The input is corrupted.
    Corrupted,
    // Invalid call.
    Invalid,
    // The input uses features that are not yet implemented.
    NotImplemented,
};

class DecoderError {
public:
    template<OneOf<StringView, ByteString> T>
    static DecoderError with_description(DecoderErrorCategory category, T description)
    {
        return DecoderError(category, move(description));
    }

    template<typename... Parameters>
    static DecoderError format(DecoderErrorCategory category, CheckedFormatString<Parameters...>&& format_string, Parameters const&... parameters)
    {
        AK::VariadicFormatParams<AK::AllowDebugOnlyFormatters::No, Parameters...> variadic_format_params { parameters... };
        return DecoderError::with_description(category, ByteString::vformatted(format_string.view(), variadic_format_params));
    }

    static DecoderError from_source_location(DecoderErrorCategory category, StringView description, SourceLocation location = SourceLocation::current())
    {
        return DecoderError::format(category, "[{} @ {}:{}]: {}", location.function_name(), location.filename(), location.line_number(), description);
    }

    static DecoderError corrupted(StringView description, SourceLocation location = SourceLocation::current())
    {
        return DecoderError::from_source_location(DecoderErrorCategory::Corrupted, description, location);
    }

    static DecoderError not_implemented(SourceLocation location = SourceLocation::current())
    {
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "{} is not implemented", location.function_name());
    }

    DecoderErrorCategory category() const { return m_category; }
    StringView description() const LIFETIME_BOUND
    {
        return m_description.visit([](StringView x) { return x; }, [](ByteString const& x) { return x.view(); });
    }
    // For compatibility with AK::Error
    StringView string_literal() const LIFETIME_BOUND { return description(); }

private:
    template<OneOf<StringView, ByteString> T>
    DecoderError(DecoderErrorCategory category, T description)
        : m_category(category)
        , m_description(move(description))
    {
    }

    DecoderErrorCategory m_category { DecoderErrorCategory::Unknown };
    Variant<StringView, ByteString> m_description;
};

#define DECODER_TRY(category, expression)                                                  \
    ({                                                                                     \
        auto&& _result = ((expression));                                                   \
        if (_result.is_error()) [[unlikely]] {                                             \
            auto _error_string = _result.error().string_literal();                         \
            return DecoderError::from_source_location(                                     \
                ((category)), _error_string, SourceLocation::current());                   \
        }                                                                                  \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_result.release_value())>, \
            "Do not return a reference from a fallible expression");                       \
        _result.release_value();                                                           \
    })

#define DECODER_TRY_ALLOC(expression) DECODER_TRY(DecoderErrorCategory::Memory, expression)

}

namespace AK {

template<>
struct Formatter<Media::DecoderError> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Media::DecoderError const& decoder_error)
    {
        return Formatter<FormatString>::format(builder, "[DecoderError]: {}"sv, decoder_error.description());
    }
};

}
