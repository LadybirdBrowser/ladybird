/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Debug.h>
#include <AK/Forward.h>
#include <AK/UnicodeCodePointView.h>

namespace AK {

// FIXME: Remove this when clang on BSD distributions fully support consteval (specifically in the context of default parameter initialization).
//        Note that this is fixed in clang-15, but is not yet picked up by all downstream distributions.
//        See: https://github.com/llvm/llvm-project/issues/48230
//        Additionally, oss-fuzz currently ships an llvm-project commit that is a pre-release of 15.0.0.
//        See: https://github.com/google/oss-fuzz/issues/9989
//        Android currently doesn't ship clang-15 in any NDK
#if defined(AK_OS_BSD_GENERIC) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_UTF8_MIXIN_CONSTEVAL constexpr
#else
#    define AK_UTF8_MIXIN_CONSTEVAL consteval
#endif

// https://www.unicode.org/versions/Unicode16.0.0/UnicodeStandard-16.0.pdf#page=168
// https://encoding.spec.whatwg.org/#utf-8-decoder
template<typename Self>
class Utf8Mixin {
public:
    enum class AllowOverlong {
        No,
        Yes,
    };
    enum class AllowSurrogates {
        No,
        Yes,
    };
    enum class AllowedCodePoints {
        UnicodeOnly,          // U+0000..U+D800 & U+E000..U+10FFFF
        UnicodeAndSurrogates, // U+0000..U+10FFFF
        All,                  // U+0000..U+7FFFFFFF
    };
    enum class ReplacementCharacterSubstitution { // On an invalid encoding, replace [...] with U+FFFD
        ByteByByte,                               // The first byte
        MaximalSubparts,                          // Up to and including the first invalid byte (NOTE: Behaves like MaximalContinuation when overlong encodings are allowed!)
        MaximalContinuation,                      // Up to and including the last continuation byte
    };

    template<AllowedCodePoints allowed_code_points>
    static AK_UTF8_MIXIN_CONSTEVAL bool consteval_validate(ReadonlySpan<char8_t> const& sv)
    {
        size_t i = 0;
        while (i < sv.size()) {
            if (sv[i] <= 0x7F) {
                i += 1;
                continue;
            }
            if (is_continuation_byte(sv[i]))
                return false;
            if (auto code_unit_length = determine_code_unit_length_from_leading_byte(sv[i], allowed_code_points == AllowedCodePoints::All)) {
                for (size_t j = 1; j < code_unit_length; j++) {
                    if (i + j >= sv.size())
                        return false;
                    if (!is_continuation_byte(sv[i + j]))
                        return false;
                }
                auto cp = code_point_from_code_units(sv.slice(i, code_unit_length));
                switch (allowed_code_points) {
                case AllowedCodePoints::UnicodeOnly:
                    if (cp >= 0xD800 && cp <= 0xDFFF)
                        return false;
                    [[fallthrough]];
                case AllowedCodePoints::UnicodeAndSurrogates:
                    if (cp > 0x10FFFF)
                        return false;
                    [[fallthrough]];
                case AllowedCodePoints::All:
                    break;
                }
                i += code_unit_length;
            } else {
                return false;
            }
        }
        return true;
    }

    template<AllowedCodePoints allowed_code_points, AllowOverlong allow_overlong = AllowOverlong::No, ReplacementCharacterSubstitution replacement_character_substitution = ReplacementCharacterSubstitution::MaximalSubparts>
    Optional<u32> chomp_one_extended_utf8_codepoint_left()
    {
        auto* self = static_cast<Self*>(this);
        if (self->is_empty())
            return {};
        auto const* code_units = reinterpret_cast<char8_t const*>(self->m_code_units);

        if (self->m_code_point_length.has_value())
            --*self->m_code_point_length;

        auto replacement_character = [this](size_t n = 1) {
            if constexpr (replacement_character_substitution == ReplacementCharacterSubstitution::ByteByByte) {
                n = 1;
            }
            chomp_code_units_left(n);
            return UnicodeCodePoint::REPLACEMENT_CHARACTER;
        };

        if (code_units[0] <= 0x7F) { // 0x00 to 0x7F
            // OPTIMIZATION: Fast path for ASCII
            chomp_code_units_left(1);
            return UnicodeCodePoint::unchecked(code_units[0]);
        } else if (code_units[0] <= 0xBF) { // 0x80 to 0xBF
            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[0]);
            return replacement_character();
        }

        auto invalid_for_length = [&](size_t n) -> u32 {
            ASSERT(n <= 6);
            constexpr char const* nth[] = {
                "Zeroth (?)",
                "First",
                "Second",
                "Third",
                "Fourth",
                "Fifth",
                "Sixth (?)",
            };
            if constexpr (replacement_character_substitution == ReplacementCharacterSubstitution::MaximalSubparts) {
                switch (n) {
                case 1: // 0x00 to 0x7F
                    break;
                case 2: // 0xC0 to 0xDF
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xC0 || code_units[0] == 0xC1) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points with leading byte {:#02x} would be overlong.", code_units[0]);
                            return replacement_character(2);
                        }
                    }
                    break;
                case 3: // 0xE0 to 0xEF
                    if constexpr (allowed_code_points == AllowedCodePoints::UnicodeOnly) {
                        if (code_units[0] == 0xED && code_units[1] > 0x9F) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] encode surrogate code points, which would not be valid Unicode scalar values.", code_units[0], code_units[1]);
                            return replacement_character(2);
                        }
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xE0 && code_units[1] < 0xA0) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                            return replacement_character(2);
                        }
                    }
                    break;
                case 4: // 0xF0 to 0xF7
                    if constexpr (allowed_code_points != AllowedCodePoints::All) {
                        if (code_units[0] == 0xF4 && code_units[1] > 0x8F) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would not be valid Unicode scalar values.", code_units[0], code_units[1]);
                            return replacement_character(2);
                        }
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xF0 && code_units[1] < 0x90) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                            return replacement_character(2);
                        }
                    }
                    break;
                case 5: // 0xF8 to 0xFB
                    if constexpr (allowed_code_points != AllowedCodePoints::All) {
                        dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would not be valid Unicode scalar values.", code_units[0], code_units[1]);
                        return replacement_character();
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xF8 && code_units[1] < 0x88) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                            return replacement_character(2);
                        }
                    }
                    break;
                case 6: // 0xFC to 0xFD
                    if constexpr (allowed_code_points != AllowedCodePoints::All) {
                        dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would not be valid Unicode scalar values.", code_units[0], code_units[1]);
                        return replacement_character();
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xFC && code_units[1] < 0x84) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                            return replacement_character(2);
                        }
                    }
                    break;
                default:
                    ASSERT_NOT_REACHED();
                }
            }
            for (size_t i = 1; i < n; i++) {
                if (i >= self->m_code_unit_length) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Too few bytes remaining for code points with leading byte {:#02x} (got {}, but need {}).", code_units[0], self->m_code_unit_length, n);
                    return replacement_character(self->m_code_unit_length);
                }
                if (!is_continuation_byte(code_units[i])) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: {} byte after leading {:#02x} is not a continuation byte.", nth[i], code_units[0]);
                    return replacement_character(i + 1);
                }
            }
            return 0;
        };

        constexpr bool parse_larger_continuations = allowed_code_points == AllowedCodePoints::All || replacement_character_substitution == ReplacementCharacterSubstitution::MaximalContinuation;

        if (size_t code_unit_length = determine_code_unit_length_from_leading_byte(code_units[0], parse_larger_continuations)) {
            if (invalid_for_length(code_unit_length)) {
                return 0xFFFD;
            }

            u32 code_point = code_point_from_code_units(ReadonlySpan<char8_t> { code_units, code_unit_length });

            if constexpr (allow_overlong == AllowOverlong::No) {
                if (is_overlong_for_length(code_point, code_unit_length)) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Overlong ({} byte) encoding of U+{:04X}.", code_unit_length, code_point);
                    return replacement_character(code_unit_length);
                }
            }
            if constexpr (allowed_code_points == AllowedCodePoints::UnicodeOnly) {
                if (code_point >= 0xD800 && code_point <= 0xDFFF) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Surrogate code point U+{:04X} is not a valid Unicode scalar value.", code_point);
                    return replacement_character(code_unit_length);
                }
            } else if constexpr (allowed_code_points != AllowedCodePoints::All) {
                if (code_point > 0x10FFFF) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code point U+{:04X} is not a valid Unicode scalar value.", code_point);
                    return replacement_character(code_unit_length);
                }
            }

            chomp_code_units_left(code_unit_length);

            return UnicodeCodePoint::checked(code_point);
        }

        dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading byte {:#02x} is not valid.", code_units[0]);
        return replacement_character();
    }

    template<AllowedCodePoints allowed_code_points, AllowOverlong allow_overlong = AllowOverlong::No, ReplacementCharacterSubstitution replacement_character_substitution = ReplacementCharacterSubstitution::MaximalSubparts>
    Optional<u32> chomp_one_extended_utf8_codepoint_right()
    {
        auto* self = static_cast<Self*>(this);
        if (self->is_empty())
            return {};
        auto const* code_units = reinterpret_cast<char8_t const*>(self->m_code_units);

        if (self->m_code_point_length.has_value())
            --*self->m_code_point_length;

        auto replacement_character = [this](size_t n = 1) {
            if constexpr (replacement_character_substitution == ReplacementCharacterSubstitution::ByteByByte) {
                n = 1;
            }
            chomp_code_units_right(n);
            return UnicodeCodePoint::REPLACEMENT_CHARACTER;
        };

        if (code_units[self->m_code_unit_length - 1] <= 0x7F) { // 0x00 to 0x7F
            // OPTIMIZATION: Fast path for ASCII
            auto code_point = code_units[self->m_code_unit_length - 1];
            chomp_code_units_right(1);
            return UnicodeCodePoint::unchecked(code_point);
        }

        size_t continuation_bytes = 0;
        while (is_continuation_byte(code_units[self->m_code_unit_length - continuation_bytes - 1])) {
            continuation_bytes++;
            if (continuation_bytes >= min(self->m_code_unit_length, 7)) {
                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[self->m_code_unit_length - 1]);
                return replacement_character();
            }
        }

        code_units = &code_units[self->m_code_unit_length - continuation_bytes - 1];

        auto invalid_for_length = [&](size_t n) -> u32 {
            ASSERT(n <= 6);
            constexpr char const* nth[] = {
                "Zeroth (?)",
                "First",
                "Second",
                "Third",
                "Fourth",
                "Fifth",
                "Sixth (?)",
            };
            if constexpr (replacement_character_substitution == ReplacementCharacterSubstitution::MaximalSubparts) {
                switch (n) {
                case 1: // 0x00 to 0x7F
                    break;
                case 2: // 0xC0 to 0xDF
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xC0 || code_units[0] == 0xC1) {
                            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points with leading byte {:#02x} would be overlong.", code_units[0]);
                            return replacement_character(2);
                        }
                    }
                    break;
                case 3: // 0xE0 to 0xEF
                    if constexpr (allowed_code_points == AllowedCodePoints::UnicodeOnly) {
                        if (code_units[0] == 0xED && code_units[1] > 0x9F) {
                            if (self->m_code_unit_length == 2) {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] encode surrogate code points, which would not be valid Unicode scalar values.", code_units[0], code_units[1]);
                                return replacement_character(2);
                            } else {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
                                return replacement_character();
                            }
                        }
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xE0 && code_units[1] < 0xA0) {
                            if (self->m_code_unit_length == 2) {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                                return replacement_character(2);
                            } else {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
                                return replacement_character();
                            }
                        }
                    }
                    break;
                case 4: // 0xF0 to 0xF7
                    if constexpr (allowed_code_points != AllowedCodePoints::All) {
                        if (code_units[0] == 0xF4 && code_units[1] > 0x8F) {
                            if (self->m_code_unit_length == 2) {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would not be valid Unicode scalar values.", code_units[0], code_units[1]);
                                return replacement_character(2);
                            } else {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
                                return replacement_character();
                            }
                        }
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xF0 && code_units[1] < 0x90) {
                            if (self->m_code_unit_length == 2) {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                                return replacement_character(2);
                            } else {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
                                return replacement_character();
                            }
                        }
                    }
                    break;
                case 5: // 0xF8 to 0xFB
                    if constexpr (allowed_code_points != AllowedCodePoints::All) {
                        dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points with leading byte {:#02x} would not be valid Unicode scalar values.", code_units[0]);
                        return replacement_character();
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xF8 && code_units[1] < 0x88) {
                            if (self->m_code_unit_length == 2) {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                                return replacement_character(2);
                            } else {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
                                return replacement_character();
                            }
                        }
                    }
                    break;
                case 6: // 0xFC to 0xFD
                    if constexpr (allowed_code_points != AllowedCodePoints::All) {
                        dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points with leading byte {:#02x} would not be valid Unicode scalar values.", code_units[0]);
                        return replacement_character();
                    }
                    if constexpr (allow_overlong == AllowOverlong::No) {
                        if (code_units[0] == 0xFC && code_units[1] < 0x84) {
                            if (self->m_code_unit_length == 2) {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code points starting with [{:#02x} {:#02x}] would be overlong.", code_units[0], code_units[1]);
                                return replacement_character(2);
                            } else {
                                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
                                return replacement_character();
                            }
                        }
                    }
                    break;
                default:
                    ASSERT_NOT_REACHED();
                }
            }
            for (size_t i = 1; i < n; i++) {
                if (i >= self->m_code_unit_length) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Too few bytes remaining for code points with leading byte {:#02x} (got {}, but need {}).", code_units[0], self->m_code_unit_length, n);
                    return replacement_character(self->m_code_unit_length);
                }
                if (!is_continuation_byte(code_units[i])) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: {} byte after leading {:#02x} is not a continuation byte.", nth[i], code_units[0]);
                    return replacement_character(i + 1);
                }
            }
            return 0;
        };

        constexpr bool parse_larger_continuations = allowed_code_points == AllowedCodePoints::All || replacement_character_substitution == ReplacementCharacterSubstitution::MaximalContinuation;

        if (size_t code_unit_length = determine_code_unit_length_from_leading_byte(code_units[0], parse_larger_continuations)) {
            if (continuation_bytes + 1 > code_unit_length) {
                dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
                return replacement_character();
            }

            if (invalid_for_length(code_unit_length)) {
                return 0xFFFD;
            }

            u32 code_point = code_point_from_code_units(ReadonlySpan<char8_t> { code_units, code_unit_length });

            if constexpr (allow_overlong == AllowOverlong::No) {
                if (is_overlong_for_length(code_point, code_unit_length)) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Overlong ({} byte) encoding of U+{:04X}.", code_unit_length, code_point);
                    return replacement_character(code_unit_length);
                }
            }
            if constexpr (allowed_code_points == AllowedCodePoints::UnicodeOnly) {
                if (code_point >= 0xD800 && code_point <= 0xDFFF) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Surrogate code point U+{:04X} is not a valid Unicode scalar value.", code_point);
                    return replacement_character(code_unit_length);
                }
            } else if constexpr (allowed_code_points != AllowedCodePoints::All) {
                if (code_point > 0x10FFFF) {
                    dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Code point U+{:04X} is not a valid Unicode scalar value.", code_point);
                    return replacement_character(code_unit_length);
                }
            }

            chomp_code_units_right(code_unit_length);

            return UnicodeCodePoint::checked(code_point);
        }

        if (continuation_bytes == 0) {
            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading byte {:#02x} is not valid.", code_units[0]);
        } else {
            dbgln_if(UTF8_DEBUG, "Invalid UTF-8: Leading continuation byte: {:#02x}.", code_units[continuation_bytes]);
        }
        return replacement_character();
    }

    template<AllowSurrogates allow_surrogates, AllowOverlong allow_overlong = AllowOverlong::No, ReplacementCharacterSubstitution replacement_character_substitution = ReplacementCharacterSubstitution::MaximalSubparts>
    Optional<UnicodeCodePoint> chomp_one_utf8_codepoint_left()
    {
        return chomp_one_extended_utf8_codepoint_left < allow_surrogates == AllowSurrogates::Yes ? AllowedCodePoints::UnicodeAndSurrogates : AllowedCodePoints::UnicodeOnly, allow_overlong, replacement_character_substitution > ().map(UnicodeCodePoint::checked);
    }

    template<AllowSurrogates allow_surrogates, AllowOverlong allow_overlong = AllowOverlong::No, ReplacementCharacterSubstitution replacement_character_substitution = ReplacementCharacterSubstitution::MaximalSubparts>
    Optional<UnicodeCodePoint> chomp_one_utf8_codepoint_right()
    {
        return chomp_one_extended_utf8_codepoint_right < allow_surrogates == AllowSurrogates::Yes ? AllowedCodePoints::UnicodeAndSurrogates : AllowedCodePoints::UnicodeOnly, allow_overlong, replacement_character_substitution > ().map(UnicodeCodePoint::checked);
    }

private:
    void chomp_code_units_left(size_t n)
    {
        auto* self = static_cast<Self*>(this);
        self->m_code_units = reinterpret_cast<char8_t const*>(self->m_code_units) + n;
        self->m_code_unit_length -= n;
    }

    void chomp_code_units_right(size_t n)
    {
        auto* self = static_cast<Self*>(this);
        self->m_code_unit_length -= n;
    }

    static constexpr bool is_continuation_byte(u8 value)
    {
        return (value & 0xC0) == 0x80;
    }

    static constexpr bool is_overlong_for_length(u32 code_point, size_t code_unit_length)
    {
        switch (code_unit_length) {
        case 2:
            return code_point <= 0x7F;
        case 3:
            return code_point <= 0x7FF;
        case 4:
            return code_point <= 0xFFFF;
        case 5:
            return code_point <= 0x1FFFFF;
        case 6:
            return code_point <= 0x3FFFFFF;
        default:
            ASSERT_NOT_REACHED();
        }
    }

    static constexpr size_t determine_code_unit_length_from_leading_byte(u8 leading_byte, bool parse_larger_continuations = false)
    {
        ASSERT(leading_byte >= 0xC0);
        if (leading_byte <= 0xDF) // 0xC2 to 0xDF (+ 0xC0 & 0xC1)
            return 2;
        if (leading_byte <= 0xEF) // 0xE0 to 0xEF
            return 3;
        if (leading_byte <= 0xF4) // 0xF0 to 0xF4
            return 4;
        // 0xF5 to 0xFF
        if (parse_larger_continuations) {
            if (leading_byte <= 0xF7) // 0xF5 to 0xF7
                return 4;
            if (leading_byte <= 0xFB) // 0xF8 to 0xFB
                return 5;
            if (leading_byte <= 0xFD) // 0xFC & 0xFD
                return 6;
            // 0xFE & 0xFF
        }
        return 0;
    }

    static constexpr u32 code_point_from_code_units(ReadonlySpan<char8_t> code_units)
    {
        switch (code_units.size()) {
        case 2:
            return (code_units[0] & 0x1F) << 6 * 1
                | (code_units[1] & 0x3F) << 6 * 0;
        case 3:
            return (code_units[0] & 0x0F) << 6 * 2
                | (code_units[1] & 0x3F) << 6 * 1
                | (code_units[2] & 0x3F) << 6 * 0;
        case 4:
            return (code_units[0] & 0x07) << 6 * 3
                | (code_units[1] & 0x3F) << 6 * 2
                | (code_units[2] & 0x3F) << 6 * 1
                | (code_units[3] & 0x3F) << 6 * 0;
        case 5:
            return (code_units[0] & 0x03) << 6 * 4
                | (code_units[1] & 0x3F) << 6 * 3
                | (code_units[2] & 0x3F) << 6 * 2
                | (code_units[3] & 0x3F) << 6 * 1
                | (code_units[4] & 0x3F) << 6 * 0;
        case 6:
            return (code_units[0] & 0x01) << 6 * 5
                | (code_units[1] & 0x3F) << 6 * 4
                | (code_units[2] & 0x3F) << 6 * 3
                | (code_units[3] & 0x3F) << 6 * 2
                | (code_units[4] & 0x3F) << 6 * 1
                | (code_units[5] & 0x3F) << 6 * 0;
        default:
            ASSERT_NOT_REACHED();
        }
    }
};

}
