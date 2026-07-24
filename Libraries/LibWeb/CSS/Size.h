/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::CSS {

class Size : public ComputedValuesFFI::ComputedSize {
public:
    using Type = ComputedValuesFFI::ComputedSizeKind;

    Size(Size const& other)
        : ComputedSize { other.kind, { other.value.pointer ? StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(other.value.pointer)) : nullptr } }
    {
    }

    Size(Size&& other)
        : ComputedSize { other.kind, { exchange(other.value.pointer, nullptr) } }
    {
    }

    Size& operator=(Size other)
    {
        swap(kind, other.kind);
        swap(value.pointer, other.value.pointer);
        return *this;
    }

    ~Size()
    {
        StyleValueFFI::rust_style_value_release(static_cast<StyleValueFFI::StyleValueData const*>(value.pointer));
    }

    static Size make_auto() { return Size { Type::Auto }; }
    static Size make_px(CSSPixels px) { return make_length(Length::make_px(px)); }
    static Size make_length(Length length) { return Size { Type::Length, move(length) }; }
    static Size make_percentage(Percentage percentage) { return Size { Type::Percentage, move(percentage) }; }
    static Size make_calculated(NonnullRefPtr<CalculatedStyleValue const> calculated) { return Size { Type::Calculated, move(calculated) }; }
    static Size make_min_content() { return Size { Type::MinContent }; }
    static Size make_max_content() { return Size { Type::MaxContent }; }
    static Size make_fit_content(LengthPercentage available_space) { return Size { Type::FitContent, move(available_space) }; }
    static Size make_fit_content() { return Size { Type::FitContent }; }
    static Size make_none() { return Size { Type::None }; }

    static Size from_style_value(NonnullRefPtr<StyleValue const> const& value)
    {
        if (value->is_keyword()) {
            switch (value->to_keyword()) {
            case Keyword::Auto:
                return make_auto();
            case Keyword::FitContent:
                return make_fit_content();
            case Keyword::MinContent:
                return make_min_content();
            case Keyword::MaxContent:
                return make_max_content();
            case Keyword::None:
                return make_none();
            default:
                VERIFY_NOT_REACHED();
            }
        }
        if (value->is_function() && value->as_function().name() == "fit-content"_utf16_fly_string)
            return make_fit_content(LengthPercentage::from_style_value(value->as_function().value()));
        if (value->is_calculated())
            return make_calculated(value->as_calculated());
        if (value->is_percentage())
            return make_percentage(value->as_percentage().percentage());
        if (value->is_length())
            return make_length(value->as_length().length());

        // FIXME: Support `anchor-size(..)`
        if (value->is_anchor_size())
            return make_none();

        dbgln("FIXME: Unsupported size value: `{}`, treating as `auto`", value->to_string(SerializationMode::Normal));
        return make_auto();
    }

    bool is_auto() const { return kind == Type::Auto; }
    bool is_calculated() const { return kind == Type::Calculated; }
    bool is_length() const { return kind == Type::Length; }
    bool is_percentage() const { return kind == Type::Percentage; }
    bool is_min_content() const { return kind == Type::MinContent; }
    bool is_max_content() const { return kind == Type::MaxContent; }
    bool is_fit_content() const { return kind == Type::FitContent; }
    bool is_none() const { return kind == Type::None; }
    Type type() const { return kind; }

    bool is_intrinsic_sizing_constraint() const { return is_min_content() || is_max_content() || is_fit_content(); }
    bool is_length_percentage() const { return is_length() || is_percentage() || is_calculated(); }

    [[nodiscard]] CSSPixels to_px(CSSPixels reference_value) const
    {
        if (!value.pointer)
            return 0;
        return length_percentage().resolved(reference_value).absolute_length_to_px();
    }

    bool contains_percentage() const
    {
        switch (kind) {
        case Type::Auto:
        case Type::MinContent:
        case Type::MaxContent:
        case Type::None:
            return false;
        case Type::FitContent:
            return value.pointer && length_percentage().contains_percentage();
        default:
            return length_percentage().contains_percentage();
        }
    }

    ValueComparingNonnullRefPtr<CalculatedStyleValue const> calculated() const
    {
        VERIFY(is_calculated());
        return length_percentage().calculated();
    }

    Length length() const
    {
        VERIFY(is_length());
        return length_percentage().length();
    }

    Percentage percentage() const
    {
        VERIFY(is_percentage());
        return length_percentage().percentage();
    }

    LengthPercentage const& length_percentage() const
    {
        VERIFY(value.pointer);
        return LengthPercentage::view(value);
    }

    Optional<LengthPercentage> fit_content_available_space() const
    {
        VERIFY(is_fit_content());
        if (!value.pointer)
            return {};
        return length_percentage();
    }

    void serialize(StringBuilder& builder, SerializationMode mode) const
    {
        switch (kind) {
        case Type::Auto:
            builder.append("auto"sv);
            break;
        case Type::Calculated:
        case Type::Length:
        case Type::Percentage:
            length_percentage().serialize(builder, mode);
            break;
        case Type::MinContent:
            builder.append("min-content"sv);
            break;
        case Type::MaxContent:
            builder.append("max-content"sv);
            break;
        case Type::FitContent:
            if (!value.pointer) {
                builder.append("fit-content"sv);
            } else {
                builder.append("fit-content("sv);
                length_percentage().serialize(builder, mode);
                builder.append(")"sv);
            }
            break;
        case Type::None:
            builder.append("none"sv);
            break;
        }
    }

    String to_string(SerializationMode mode) const
    {
        StringBuilder builder;
        serialize(builder, mode);
        return MUST(builder.to_string());
    }
    bool operator==(Size const& other) const
    {
        if (kind != other.kind)
            return false;
        if (!value.pointer || !other.value.pointer)
            return value.pointer == other.value.pointer;
        return length_percentage() == other.length_percentage();
    }

    static Size const& view(ComputedValuesFFI::ComputedSize const& size)
    {
        static_assert(sizeof(Size) == sizeof(size));
        return reinterpret_cast<Size const&>(size);
    }

    static void replace(ComputedValuesFFI::ComputedSize& target, Size replacement)
    {
        swap(target.kind, replacement.kind);
        swap(target.value.pointer, replacement.value.pointer);
    }

private:
    explicit Size(Type type, Optional<LengthPercentage> length_percentage = {})
        : ComputedSize { type, { length_percentage.has_value() ? length_percentage->leak_data() : nullptr } }
    {
    }
};

}

template<>
struct AK::Formatter<Web::CSS::Size> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Size const& size)
    {
        return Formatter<StringView>::format(builder, size.to_string(Web::CSS::SerializationMode::Normal));
    }
};
