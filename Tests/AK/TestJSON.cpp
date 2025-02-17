/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>

TEST_CASE(load_form)
{
    static constexpr auto raw_form_json = R"(
    {
        "name": "Form1",
        "widgets": [
            {
                "enabled": true,
                "forecolor": "#000000ff",
                "ruler_visible": false,
                "autofill": false,
                "x": 155,
                "tooltip": null,
                "height": 121,
                "width": 126,
                "y": 10,
                "class": "GTextEditor",
                "text": "Hi",
                "backcolor": "#c0c0c0ff",
                "visible":true
            }
        ]
    })"sv;

    JsonValue form_json = JsonValue::from_string(raw_form_json).value();

    EXPECT(form_json.is_object());

    auto name = form_json.as_object().get_string("name"sv);
    EXPECT(name.has_value());

    EXPECT_EQ(name.value(), "Form1");

    auto widgets = form_json.as_object().get_array("widgets"sv);
    EXPECT(widgets.has_value());

    widgets->for_each([&](JsonValue const& widget_value) {
        auto& widget_object = widget_value.as_object();
        auto widget_class = widget_object.get_string("class"sv).value();
        widget_object.for_each_member([&]([[maybe_unused]] auto& property_name, [[maybe_unused]] JsonValue const& property_value) {
        });
    });
}

TEST_CASE(json_empty_string)
{
    auto json = JsonValue::from_string("\"\""sv).value();
    EXPECT_EQ(json.type(), JsonValue::Type::String);
    EXPECT_EQ(json.as_string().is_empty(), true);
}

TEST_CASE(json_string)
{
    auto json = JsonValue::from_string("\"A\""sv).value();
    EXPECT_EQ(json.type(), JsonValue::Type::String);
    EXPECT_EQ(json.as_string().byte_count(), size_t { 1 });
    EXPECT_EQ(json.as_string() == "A", true);
}

TEST_CASE(json_utf8_character)
{
    auto json = JsonValue::from_string("\"\\u0041\""sv).value();
    EXPECT_EQ(json.type(), JsonValue::Type::String);
    EXPECT_EQ(json.as_string().byte_count(), size_t { 1 });
    EXPECT_EQ(json.as_string() == "A", true);
}

TEST_CASE(json_encoded_surrogates)
{
    {
        auto json = JsonValue::from_string("\"\\uD83E\\uDD13\""sv).value();
        EXPECT_EQ(json.type(), JsonValue::Type::String);
        EXPECT_EQ(json.as_string().byte_count(), 4u);
        EXPECT_EQ(json.as_string(), "🤓"sv);
    }
    {
        auto json = JsonValue::from_string("\"\\uD83E\""sv).value();
        EXPECT_EQ(json.type(), JsonValue::Type::String);
        EXPECT_EQ(json.as_string().byte_count(), 3u);
        EXPECT_EQ(json.as_string(), "\xED\xA0\xBE"sv);
    }
    {
        auto json = JsonValue::from_string("\"\\uDD13\""sv).value();
        EXPECT_EQ(json.type(), JsonValue::Type::String);
        EXPECT_EQ(json.as_string().byte_count(), 3u);
        EXPECT_EQ(json.as_string(), "\xED\xB4\x93"sv);
    }
}

/*
FIXME: Parse JSON from a Utf8View

TEST_CASE(json_utf8_multibyte)
{
    auto json_or_error = JsonValue::from_string("\"š\"");
    EXPECT_EQ(json_or_error.is_error(), false);

    auto& json = json_or_error.value();
    EXPECT_EQ(json.type(), JsonValue::Type::String);
    EXPECT_EQ(json.as_string().byte_count(), size_t { 2 });
    EXPECT_EQ(json.as_string() == "š", true);
    EXPECT_EQ(json.as_string() == "\xc5\xa1", true);
}
*/

TEST_CASE(json_64_bit_value)
{
    auto big_value = 0x12345678aabbccddull;
    JsonValue big_json_value(big_value);
    EXPECT(big_json_value.is_integer<u64>());
    EXPECT_EQ(big_json_value.as_integer<u64>(), big_value);

    JsonValue big_json_value_copy = big_json_value;
    EXPECT(big_json_value.equals(big_json_value_copy));
}

TEST_CASE(json_64_bit_value_coerced_to_32_bit)
{
    {
        auto min = NumericLimits<i64>::min();
        auto max = NumericLimits<i64>::max();

        auto json = TRY_OR_FAIL(JsonValue::from_string(String::number(min)));
        EXPECT_EQ(json.get_integer<i64>(), min);
        EXPECT(!json.is_integer<i32>());

        json = TRY_OR_FAIL(JsonValue::from_string(String::number(max)));
        EXPECT_EQ(json.get_integer<i64>(), max);
        EXPECT(!json.is_integer<i32>());
    }
    {
        auto min = NumericLimits<u64>::min();
        auto max = NumericLimits<u64>::max();

        auto json = TRY_OR_FAIL(JsonValue::from_string(String::number(min)));
        EXPECT_EQ(json.get_integer<u64>(), min);
        EXPECT_EQ(json.get_integer<u32>(), min);

        json = TRY_OR_FAIL(JsonValue::from_string(String::number(max)));
        EXPECT_EQ(json.get_integer<u64>(), max);
        EXPECT(!json.is_integer<u32>());
    }
}

TEST_CASE(json_duplicate_keys)
{
    JsonObject json;
    json.set("test"sv, "foo"sv);
    json.set("test"sv, "bar"sv);
    json.set("test"sv, "baz"sv);
    EXPECT_EQ(json.serialized(), "{\"test\":\"baz\"}");
}

TEST_CASE(json_u64_roundtrip)
{
    auto big_value = 0xffffffffffffffffull;
    auto json = JsonValue(big_value).serialized();
    auto value = JsonValue::from_string(json);
    EXPECT_EQ_FORCE(value.is_error(), false);
    EXPECT_EQ(value.value().as_integer<u64>(), big_value);
}

TEST_CASE(json_parse_empty_string)
{
    auto value = JsonValue::from_string(""sv);
    EXPECT_EQ(value.is_error(), true);
}

TEST_CASE(json_parse_long_decimals)
{
    auto value = JsonValue::from_string("1644452550.6489999294281"sv);
    EXPECT_EQ(value.value().get_double_with_precision_loss(), 1644452550.6489999294281);
}

TEST_CASE(json_parse_number_with_exponent)
{
    auto value_without_fraction = JsonValue::from_string("10e5"sv);
    EXPECT_EQ(value_without_fraction.value().get_double_with_precision_loss(), 1000000.0);

    auto value_with_fraction = JsonValue::from_string("10.5e5"sv);
    EXPECT_EQ(value_with_fraction.value().get_double_with_precision_loss(), 1050000.0);
}

TEST_CASE(json_parse_special_numbers)
{
#define EXPECT_TO_MATCH_NUMBER_BIT_WISE(string_input, double_input)                                  \
    do {                                                                                             \
        auto value_or_error = JsonValue::from_string(string_input##sv);                              \
        VERIFY(!value_or_error.is_error());                                                          \
        if (value_or_error.is_error())                                                               \
            dbgln("got {}", value_or_error.error());                                                 \
        auto value = value_or_error.release_value();                                                 \
        EXPECT(value.is_number());                                                                   \
        auto value_as_double = value.get_double_with_precision_loss().value();                       \
        EXPECT_EQ(bit_cast<u64>(value_as_double), bit_cast<u64>(static_cast<double>(double_input))); \
    } while (false)

    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0", -0.);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0.0", -0.0);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0.00", -0.00);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0e0", -0e0);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0e1", -0e1);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0e2", -0e2);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0e1000", -0e1000);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-0e-1000", -0e-1000);

    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0", 0.);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0.0", 0.0);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0.00", 0.00);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0e0", 0e0);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0e1", 0e1);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0e2", 0e2);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0e1000", 0e1000);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("0e-1000", 0e-1000);

    // These technically can be non zero, but not in doubles
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("-1e-2000", -0.);
    EXPECT_TO_MATCH_NUMBER_BIT_WISE("1e-2000", 0.);

#undef EXPECT_TO_MATCH_NUMBER_BIT_WISE
}

TEST_CASE(json_parse_fails_on_invalid_number)
{
#define EXPECT_JSON_PARSE_TO_FAIL(value) \
    EXPECT(JsonValue::from_string(value##sv).is_error());

    EXPECT_JSON_PARSE_TO_FAIL("-");
    EXPECT_JSON_PARSE_TO_FAIL("00");
    EXPECT_JSON_PARSE_TO_FAIL("01");
    EXPECT_JSON_PARSE_TO_FAIL("-01");
    EXPECT_JSON_PARSE_TO_FAIL(".1");
    EXPECT_JSON_PARSE_TO_FAIL("-.1");
    EXPECT_JSON_PARSE_TO_FAIL("-,1");
    EXPECT_JSON_PARSE_TO_FAIL(".1e1");
    EXPECT_JSON_PARSE_TO_FAIL(".1e-1");
    EXPECT_JSON_PARSE_TO_FAIL("-.1e1");
    EXPECT_JSON_PARSE_TO_FAIL("-.1e-1");
    EXPECT_JSON_PARSE_TO_FAIL("1.e1");
    EXPECT_JSON_PARSE_TO_FAIL("1.e-1");
    EXPECT_JSON_PARSE_TO_FAIL("-1.e1");
    EXPECT_JSON_PARSE_TO_FAIL("-1.e-1");
    EXPECT_JSON_PARSE_TO_FAIL("1e");
    EXPECT_JSON_PARSE_TO_FAIL("1e+");
    EXPECT_JSON_PARSE_TO_FAIL("1e-");
    EXPECT_JSON_PARSE_TO_FAIL("1e-f");
    EXPECT_JSON_PARSE_TO_FAIL("1.e");
    EXPECT_JSON_PARSE_TO_FAIL("1.e+");
    EXPECT_JSON_PARSE_TO_FAIL("1.e-");
    EXPECT_JSON_PARSE_TO_FAIL("1.e-f");
    EXPECT_JSON_PARSE_TO_FAIL("1p2");
    EXPECT_JSON_PARSE_TO_FAIL("1.p2");
    EXPECT_JSON_PARSE_TO_FAIL("0x1.0p2");
    EXPECT_JSON_PARSE_TO_FAIL("0x1");
    EXPECT_JSON_PARSE_TO_FAIL("0x7");
    EXPECT_JSON_PARSE_TO_FAIL("0xA");
    EXPECT_JSON_PARSE_TO_FAIL("0x");
    EXPECT_JSON_PARSE_TO_FAIL("-0x");
    EXPECT_JSON_PARSE_TO_FAIL("0x");
    EXPECT_JSON_PARSE_TO_FAIL("1x");
    EXPECT_JSON_PARSE_TO_FAIL("100x");
    EXPECT_JSON_PARSE_TO_FAIL("1000000000000000000000x");
    EXPECT_JSON_PARSE_TO_FAIL("0e2x");
    EXPECT_JSON_PARSE_TO_FAIL("0.1e2x");
    EXPECT_JSON_PARSE_TO_FAIL("0.1x");
    EXPECT_JSON_PARSE_TO_FAIL("1e2x");
    EXPECT_JSON_PARSE_TO_FAIL("1.2x");
    EXPECT_JSON_PARSE_TO_FAIL("1.2e2x");
    EXPECT_JSON_PARSE_TO_FAIL(".0");
    EXPECT_JSON_PARSE_TO_FAIL(".e1");
    EXPECT_JSON_PARSE_TO_FAIL("-.0");
    EXPECT_JSON_PARSE_TO_FAIL("-.e1");
    EXPECT_JSON_PARSE_TO_FAIL("+0");
    EXPECT_JSON_PARSE_TO_FAIL("+0.0");
    EXPECT_JSON_PARSE_TO_FAIL("+0.00");
    EXPECT_JSON_PARSE_TO_FAIL("+0e0");
    EXPECT_JSON_PARSE_TO_FAIL("+0e1");
    EXPECT_JSON_PARSE_TO_FAIL("+0e2");
    EXPECT_JSON_PARSE_TO_FAIL("+0e1000");
    EXPECT_JSON_PARSE_TO_FAIL("+0e-1000");

    EXPECT_JSON_PARSE_TO_FAIL("+10");
    EXPECT_JSON_PARSE_TO_FAIL("+10e1");
    EXPECT_JSON_PARSE_TO_FAIL("+10.3");
    EXPECT_JSON_PARSE_TO_FAIL("+10.3e1");

    EXPECT_JSON_PARSE_TO_FAIL("0x1");
    EXPECT_JSON_PARSE_TO_FAIL("0x2");
    EXPECT_JSON_PARSE_TO_FAIL("0xA");
    EXPECT_JSON_PARSE_TO_FAIL("0xB");
    EXPECT_JSON_PARSE_TO_FAIL("0xF");
    EXPECT_JSON_PARSE_TO_FAIL("0Xf");
    EXPECT_JSON_PARSE_TO_FAIL("0X3");

    EXPECT_JSON_PARSE_TO_FAIL("10ee1");
    EXPECT_JSON_PARSE_TO_FAIL("1e1e1");

    // These could be valid within an array but not as the top level value
    EXPECT_JSON_PARSE_TO_FAIL("0,0");
    EXPECT_JSON_PARSE_TO_FAIL(",1");
    EXPECT_JSON_PARSE_TO_FAIL("10e1,");
    EXPECT_JSON_PARSE_TO_FAIL("10e,1");
    EXPECT_JSON_PARSE_TO_FAIL("10,e1");
    EXPECT_JSON_PARSE_TO_FAIL("1,0e1");
    EXPECT_JSON_PARSE_TO_FAIL(",10e1");

#undef EXPECT_JSON_PARSE_TO_FAIL
}

struct CustomError {
};

template<typename T>
class CustomErrorOr {
public:
    CustomErrorOr(T)
        : m_is_error(false)
    {
    }

    CustomErrorOr(CustomError)
        : m_is_error(true)
    {
    }

    bool is_error() const { return m_is_error; }
    CustomError release_error() { return CustomError {}; }
    T release_value() { return T {}; }

private:
    bool m_is_error { false };
};

TEST_CASE(fallible_json_object_for_each)
{
    static constexpr auto raw_json = R"(
    {
        "name": "anon",
        "home": "/home/anon",
        "default_browser": "Ladybird"
    })"sv;

    auto json = JsonValue::from_string(raw_json).value();
    auto const& object = json.as_object();

    MUST(object.try_for_each_member([](auto const&, auto const&) -> ErrorOr<void> {
        return {};
    }));

    auto result1 = object.try_for_each_member([](auto const&, auto const&) -> ErrorOr<void> {
        return Error::from_string_literal("nanananana");
    });
    EXPECT(result1.is_error());
    EXPECT_EQ(result1.error().string_literal(), "nanananana"sv);

    auto result2 = object.try_for_each_member([](auto const&, auto const&) -> ErrorOr<void, CustomError> {
        return CustomError {};
    });
    EXPECT(result2.is_error());
    EXPECT((IsSame<decltype(result2.release_error()), CustomError>));

    auto result3 = object.try_for_each_member([](auto const&, auto const&) -> CustomErrorOr<int> {
        return 42;
    });
    EXPECT(!result3.is_error());

    auto result4 = object.try_for_each_member([](auto const&, auto const&) -> CustomErrorOr<int> {
        return CustomError {};
    });
    EXPECT(result4.is_error());
    EXPECT((IsSame<decltype(result4.release_error()), CustomError>));
}

TEST_CASE(fallible_json_array_for_each)
{
    static constexpr auto raw_json = R"(
    [
        "anon",
        "/home/anon",
        "Ladybird"
    ])"sv;

    auto json = JsonValue::from_string(raw_json).value();
    auto const& array = json.as_array();

    MUST(array.try_for_each([](auto const&) -> ErrorOr<void> {
        return {};
    }));

    auto result1 = array.try_for_each([](auto const&) -> ErrorOr<void> {
        return Error::from_string_literal("nanananana");
    });
    EXPECT(result1.is_error());
    EXPECT_EQ(result1.error().string_literal(), "nanananana"sv);

    auto result2 = array.try_for_each([](auto const&) -> ErrorOr<void, CustomError> {
        return CustomError {};
    });
    EXPECT(result2.is_error());
    EXPECT((IsSame<decltype(result2.release_error()), CustomError>));

    auto result3 = array.try_for_each([](auto const&) -> CustomErrorOr<int> {
        return 42;
    });
    EXPECT(!result3.is_error());

    auto result4 = array.try_for_each([](auto const&) -> CustomErrorOr<int> {
        return CustomError {};
    });
    EXPECT(result4.is_error());
    EXPECT((IsSame<decltype(result4.release_error()), CustomError>));
}

TEST_CASE(json_array_is_empty)
{
    auto raw_json = "[]"sv;
    auto json_value = MUST(JsonValue::from_string(raw_json));
    auto array = json_value.as_array();
    EXPECT(array.is_empty());

    raw_json = "[1, 2]"sv;
    json_value = MUST(JsonValue::from_string(raw_json));
    array = json_value.as_array();
    EXPECT(!array.is_empty());
}

static JsonArray setup_json_array()
{
    auto raw_json = R"([1, 2, "WHF", 802.11, 16])"sv;
    auto json_value = MUST(JsonValue::from_string(raw_json));
    return json_value.as_array();
}

TEST_CASE(json_array_size)
{
    auto array = setup_json_array();
    EXPECT_EQ(array.size(), size_t { 5 });

    auto empty_json_arr_sv = "[]"sv;
    array = MUST(JsonValue::from_string(empty_json_arr_sv)).as_array();
    EXPECT_EQ(array.size(), size_t { 0 });
}

TEST_CASE(json_array_at)
{
    auto array = setup_json_array();
    auto const& element = array.at(1);
    EXPECT_EQ(element.as_integer<u8>(), 2);
}

TEST_CASE(json_array_subscript_operator)
{
    auto array = setup_json_array();
    auto const& element = array[1];
    EXPECT_EQ(element.as_integer<u8>(), 2);
}

TEST_CASE(json_array_take)
{
    auto array = setup_json_array();
    auto const& element = array.take(2);
    EXPECT_EQ(array.size(), size_t { 4 });
    EXPECT_EQ(element.as_string(), "WHF");
}

TEST_CASE(json_array_must_append)
{
    auto array = setup_json_array();
    array.must_append(MUST(JsonValue::from_string("32"sv)));
    EXPECT_EQ(array.size(), size_t { 6 });
    EXPECT_EQ(array.at(array.size() - 1).as_integer<u8>(), 32);
}

TEST_CASE(json_array_try_append)
{
    auto array = setup_json_array();
    MUST(array.append(MUST(JsonValue::from_string("32"sv))));
    EXPECT_EQ(array.size(), size_t { 6 });
    EXPECT_EQ(array.at(array.size() - 1).as_integer<u8>(), 32);
}

TEST_CASE(json_array_clear)
{
    auto array = setup_json_array();
    array.clear();
    EXPECT(array.is_empty());
}

TEST_CASE(json_array_set)
{
    auto array = setup_json_array();
    array.set(1, MUST(JsonValue::from_string("-32"sv)));
    EXPECT_EQ(array.size(), size_t { 5 });
    EXPECT_EQ(array.at(1).as_integer<i8>(), -32);
}

TEST_CASE(json_array_ensure_capacity)
{
    auto array = setup_json_array();
    size_t new_capacity { 16 };
    array.ensure_capacity(new_capacity);
    EXPECT_EQ(array.values().capacity(), new_capacity);
}

TEST_CASE(json_array_for_each)
{
    auto raw_json = "[1, 2, 3, 4]"sv;
    auto json_value = MUST(JsonValue::from_string(raw_json));
    auto array = json_value.as_array();
    size_t count { 0 };
    array.for_each([&count](JsonValue const& value) {
        EXPECT_EQ(value.as_integer<u8>(), ++count);
    });
    EXPECT_EQ(array.size(), count);
}

TEST_CASE(json_array_serialized)
{
    auto raw_json = R"(["Hello",2,3.14,4,"World"])"sv;
    auto json_value = MUST(JsonValue::from_string(raw_json));
    auto array = json_value.as_array();
    auto const& serialized_json = array.serialized();
    EXPECT_EQ(serialized_json, raw_json);
}

TEST_CASE(json_array_serialize)
{
    auto raw_json = R"(["Hello",2,3.14,4,"World"])"sv;
    auto json_value = MUST(JsonValue::from_string(raw_json));
    auto array = json_value.as_array();
    StringBuilder builder {};
    array.serialize(builder);
    EXPECT_EQ(builder.string_view(), raw_json);
}

TEST_CASE(json_array_values)
{
    auto raw_json = "[1, 2, 3, 4]"sv;
    auto json_value = MUST(JsonValue::from_string(raw_json));
    auto array = json_value.as_array();
    auto const& values = array.values();
    EXPECT_EQ(values.size(), size_t { 4 });

    for (size_t i = 0; i < values.size(); i++)
        EXPECT_EQ(array.at(i).as_integer<u8>(), i + 1);
}

TEST_CASE(json_value_as_integer)
{
    // is_integer() should validate based on the value, not the underlying type.
    JsonValue value_int { static_cast<int>(42) };
    JsonValue value_unsigned { static_cast<unsigned>(42) };
    JsonValue value_long { static_cast<long>(42) };
    JsonValue value_long_unsigned { static_cast<long unsigned>(42) };
    JsonValue value_long_long { static_cast<long long>(42) };
    JsonValue value_long_long_unsigned { static_cast<long long unsigned>(42) };

    auto check_is_valid_for_all_types = [](JsonValue& value) {
        EXPECT(value.is_integer<u8>());
        EXPECT_EQ(value.as_integer<u8>(), static_cast<u8>(42));
        EXPECT(value.is_integer<u16>());
        EXPECT_EQ(value.as_integer<u16>(), static_cast<u16>(42));
        EXPECT(value.is_integer<u32>());
        EXPECT_EQ(value.as_integer<u32>(), static_cast<u32>(42));
        EXPECT(value.is_integer<u64>());
        EXPECT_EQ(value.as_integer<u64>(), static_cast<u64>(42));
        EXPECT(value.is_integer<i8>());
        EXPECT_EQ(value.as_integer<i8>(), static_cast<i8>(42));
        EXPECT(value.is_integer<i16>());
        EXPECT_EQ(value.as_integer<i16>(), static_cast<i16>(42));
        EXPECT(value.is_integer<i32>());
        EXPECT_EQ(value.as_integer<i32>(), static_cast<i32>(42));
        EXPECT(value.is_integer<i64>());
        EXPECT_EQ(value.as_integer<i64>(), static_cast<i64>(42));
    };

    check_is_valid_for_all_types(value_int);
    check_is_valid_for_all_types(value_unsigned);
    check_is_valid_for_all_types(value_long);
    check_is_valid_for_all_types(value_long_unsigned);
    check_is_valid_for_all_types(value_long_long);
    check_is_valid_for_all_types(value_long_long_unsigned);

    // Negative values should only fit in signed types.
    JsonValue negative_value { -42 };
    EXPECT(!negative_value.is_integer<u8>());
    EXPECT(!negative_value.is_integer<u16>());
    EXPECT(!negative_value.is_integer<u32>());
    EXPECT(!negative_value.is_integer<u64>());
    EXPECT(negative_value.is_integer<i8>());
    EXPECT(negative_value.is_integer<i16>());
    EXPECT(negative_value.is_integer<i32>());
    EXPECT(negative_value.is_integer<i64>());

    // 64-bit only
    JsonValue very_large_value { INT64_MAX };
    EXPECT(!very_large_value.is_integer<u8>());
    EXPECT(!very_large_value.is_integer<u16>());
    EXPECT(!very_large_value.is_integer<u32>());
    EXPECT(very_large_value.is_integer<u64>());
    EXPECT(!very_large_value.is_integer<i8>());
    EXPECT(!very_large_value.is_integer<i16>());
    EXPECT(!very_large_value.is_integer<i32>());
    EXPECT(very_large_value.is_integer<i64>());
}
