/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>
#include <LibUnicode/PluralRules.h>

namespace Unicode {

enum class NumberFormatStyle {
    Decimal,
    Percent,
    Currency,
    Unit,
};
NumberFormatStyle number_format_style_from_string(StringView);
StringView number_format_style_to_string(NumberFormatStyle);

enum class SignDisplay {
    Auto,
    Never,
    Always,
    ExceptZero,
    Negative,
};
SignDisplay sign_display_from_string(StringView);
StringView sign_display_to_string(SignDisplay);

enum class Notation {
    Standard,
    Scientific,
    Engineering,
    Compact,
};
Notation notation_from_string(StringView);
StringView notation_to_string(Notation);

enum class CompactDisplay {
    Short,
    Long,
};
CompactDisplay compact_display_from_string(StringView);
StringView compact_display_to_string(CompactDisplay);

enum class Grouping {
    Always,
    Auto,
    Min2,
    False,
};
Grouping grouping_from_string(StringView);
StringView grouping_to_string(Grouping);

enum class CurrencyDisplay {
    Code,
    Symbol,
    NarrowSymbol,
    Name,
};
CurrencyDisplay currency_display_from_string(StringView);
StringView currency_display_to_string(CurrencyDisplay);

enum class CurrencySign {
    Standard,
    Accounting,
};
CurrencySign currency_sign_from_string(StringView);
StringView currency_sign_to_string(CurrencySign);

struct DisplayOptions {
    NumberFormatStyle style { NumberFormatStyle::Decimal };
    SignDisplay sign_display { SignDisplay::Auto };

    Notation notation { Notation::Standard };
    Optional<CompactDisplay> compact_display;

    Grouping grouping { Grouping::Always };

    Optional<String> currency;
    Optional<CurrencyDisplay> currency_display;
    Optional<CurrencySign> currency_sign;

    Optional<String> unit;
    Optional<Style> unit_display;
};

enum class RoundingType {
    SignificantDigits,
    FractionDigits,
    MorePrecision,
    LessPrecision,
};
RoundingType rounding_type_from_string(StringView);
StringView rounding_type_to_string(RoundingType);

enum class RoundingMode {
    Ceil,
    Expand,
    Floor,
    HalfCeil,
    HalfEven,
    HalfExpand,
    HalfFloor,
    HalfTrunc,
    Trunc,
};
RoundingMode rounding_mode_from_string(StringView);
StringView rounding_mode_to_string(RoundingMode);

enum class TrailingZeroDisplay {
    Auto,
    StripIfInteger,
};
TrailingZeroDisplay trailing_zero_display_from_string(StringView);
StringView trailing_zero_display_to_string(TrailingZeroDisplay);

struct RoundingOptions {
    RoundingType type { RoundingType::MorePrecision };
    RoundingMode mode { RoundingMode::HalfExpand };
    TrailingZeroDisplay trailing_zero_display { TrailingZeroDisplay::Auto };

    Optional<int> min_significant_digits;
    Optional<int> max_significant_digits;

    Optional<int> min_fraction_digits;
    Optional<int> max_fraction_digits;

    int min_integer_digits { 0 };
    int rounding_increment { 1 };
};

class NumberFormat {
public:
    static NonnullOwnPtr<NumberFormat> create(
        StringView locale,
        DisplayOptions const&,
        RoundingOptions const&);

    virtual ~NumberFormat() = default;

    struct Partition {
        StringView type;
        String value;
        StringView source;
    };

    using Value = Variant<double, String>;

    virtual String format(Value const&) const = 0;
    virtual String format_to_decimal(Value const&) const = 0;
    virtual Vector<Partition> format_to_parts(Value const&) const = 0;

    virtual String format_range(Value const&, Value const&) const = 0;
    virtual Vector<Partition> format_range_to_parts(Value const&, Value const&) const = 0;

    virtual void create_plural_rules(PluralForm) = 0;
    virtual PluralCategory select_plural(double) const = 0;
    virtual PluralCategory select_plural_range(double, double) const = 0;
    virtual Vector<PluralCategory> available_plural_categories() const = 0;

protected:
    NumberFormat() = default;
};

}
