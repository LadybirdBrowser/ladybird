/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#normalizing-the-feature-name
static String normalize_feature_name(String const& name)
{
    // For legacy reasons, there are some aliases of some feature names. To normalize a feature name name, switch on name:

    // "screenx"
    if (name == "screenx"sv) {
        // Return "left".
        return "left"_string;
    }
    // "screeny"
    else if (name == "screeny"sv) {
        // Return "top".
        return "top"_string;
    }
    // "innerwidth"
    else if (name == "innerwidth"sv) {
        // Return "width".
        return "width"_string;
    }
    // "innerheight"
    else if (name == "innerheight") {
        // Return "height".
        return "height"_string;
    }
    // Anything else
    else {
        // Return name.
        return name;
    }
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-window-open-features-tokenize
TokenizedFeature::Map tokenize_open_features(StringView features)
{
    // 1. Let tokenizedFeatures be a new ordered map.
    TokenizedFeature::Map tokenized_features;

    // 2. Let position point at the first code point of features.
    GenericLexer lexer(features);

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#feature-separator
    auto is_feature_separator = [](auto character) {
        return Infra::is_ascii_whitespace(character) || character == '=' || character == ',';
    };

    // 3. While position is not past the end of features:
    while (!lexer.is_eof()) {
        // 1. Let name be the empty string.
        String name;

        // 2. Let value be the empty string.
        String value;

        // 3. Collect a sequence of code points that are feature separators from features given position. This skips past leading separators before the name.
        lexer.ignore_while(is_feature_separator);

        // 4. Collect a sequence of code points that are not feature separators from features given position. Set name to the collected characters, converted to ASCII lowercase.
        name = MUST(String::from_byte_string(lexer.consume_until(is_feature_separator).to_lowercase_string()));

        // 5. Set name to the result of normalizing the feature name name.
        name = normalize_feature_name(name);

        // 6. While position is not past the end of features and the code point at position in features is not U+003D (=):
        //    1. If the code point at position in features is U+002C (,), or if it is not a feature separator, then break.
        //    2. Advance position by 1.
        lexer.ignore_while(Infra::is_ascii_whitespace);

        // 7. If the code point at position in features is a feature separator:
        //    1. While position is not past the end of features and the code point at position in features is a feature separator:
        //       1. If the code point at position in features is U+002C (,), then break.
        //       2. Advance position by 1.
        lexer.ignore_while([](auto character) { return Infra::is_ascii_whitespace(character) || character == '='; });

        // 2. Collect a sequence of code points that are not feature separators code points from features given position. Set value to the collected code points, converted to ASCII lowercase.
        value = MUST(String::from_byte_string(lexer.consume_until(is_feature_separator).to_lowercase_string()));

        // 8. If name is not the empty string, then set tokenizedFeatures[name] to value.
        if (!name.is_empty())
            tokenized_features.set(move(name), move(value));
    }

    // 4. Return tokenizedFeatures.
    return tokenized_features;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-window-open-features-parse-boolean
template<Enum T>
T parse_boolean_feature(StringView value)
{
    // 1. If value is the empty string, then return true.
    if (value.is_empty())
        return T::Yes;

    // 2. If value is "yes", then return true.
    if (value == "yes"sv)
        return T::Yes;

    // 3. If value is "true", then return true.
    if (value == "true"sv)
        return T::Yes;

    // 4. Let parsed be the result of parsing value as an integer.
    auto parsed = value.to_number<i64>();

    // 5. If parsed is an error, then set it to 0.
    if (!parsed.has_value())
        parsed = 0;

    // 6. Return false if parsed is 0, and true otherwise.
    return parsed == 0 ? T::No : T::Yes;
}

template TokenizedFeature::Location parse_boolean_feature<TokenizedFeature::Location>(StringView value);
template TokenizedFeature::Menubar parse_boolean_feature<TokenizedFeature::Menubar>(StringView value);
template TokenizedFeature::NoOpener parse_boolean_feature<TokenizedFeature::NoOpener>(StringView value);
template TokenizedFeature::NoReferrer parse_boolean_feature<TokenizedFeature::NoReferrer>(StringView value);
template TokenizedFeature::Popup parse_boolean_feature<TokenizedFeature::Popup>(StringView value);
template TokenizedFeature::Resizable parse_boolean_feature<TokenizedFeature::Resizable>(StringView value);
template TokenizedFeature::Scrollbars parse_boolean_feature<TokenizedFeature::Scrollbars>(StringView value);
template TokenizedFeature::Status parse_boolean_feature<TokenizedFeature::Status>(StringView value);
template TokenizedFeature::Toolbar parse_boolean_feature<TokenizedFeature::Toolbar>(StringView value);

//  https://html.spec.whatwg.org/multipage/window-object.html#popup-window-is-requested
TokenizedFeature::Popup check_if_a_popup_window_is_requested(TokenizedFeature::Map const& tokenized_features)
{
    // 1. If tokenizedFeatures is empty, then return false.
    if (tokenized_features.is_empty())
        return TokenizedFeature::Popup::No;

    // 2. If tokenizedFeatures["popup"] exists, then return the result of parsing tokenizedFeatures["popup"] as a boolean feature.
    if (auto popup_feature = tokenized_features.get("popup"sv); popup_feature.has_value())
        return parse_boolean_feature<TokenizedFeature::Popup>(*popup_feature);

    // https://html.spec.whatwg.org/multipage/window-object.html#window-feature-is-set
    auto check_if_a_window_feature_is_set = [&]<Enum T>(StringView feature_name, T default_value) {
        // 1. If tokenizedFeatures[featureName] exists, then return the result of parsing tokenizedFeatures[featureName] as a boolean feature.
        if (auto feature = tokenized_features.get(feature_name); feature.has_value())
            return parse_boolean_feature<T>(*feature);

        // 2. Return defaultValue.
        return default_value;
    };

    // 3. Let location be the result of checking if a window feature is set, given tokenizedFeatures, "location", and false.
    auto location = check_if_a_window_feature_is_set("location"sv, TokenizedFeature::Location::No);

    // 4. Let toolbar be the result of checking if a window feature is set, given tokenizedFeatures, "toolbar", and false.
    auto toolbar = check_if_a_window_feature_is_set("toolbar"sv, TokenizedFeature::Toolbar::No);

    // 5. If location and toolbar are both false, then return true.
    if (location == TokenizedFeature::Location::No && toolbar == TokenizedFeature::Toolbar::No)
        return TokenizedFeature::Popup::Yes;

    // 6. Let menubar be the result of checking if a window feature is set, given tokenizedFeatures, menubar", and false.
    auto menubar = check_if_a_window_feature_is_set("menubar"sv, TokenizedFeature::Menubar::No);

    // 7. If menubar is false, then return true.
    if (menubar == TokenizedFeature::Menubar::No)
        return TokenizedFeature::Popup::Yes;

    // 8. Let resizable be the result of checking if a window feature is set, given tokenizedFeatures, "resizable", and true.
    auto resizable = check_if_a_window_feature_is_set("resizable"sv, TokenizedFeature::Resizable::Yes);

    // 9. If resizable is false, then return true.
    if (resizable == TokenizedFeature::Resizable::No)
        return TokenizedFeature::Popup::Yes;

    // 10. Let scrollbars be the result of checking if a window feature is set, given tokenizedFeatures, "scrollbars", and false.
    auto scrollbars = check_if_a_window_feature_is_set("scrollbars"sv, TokenizedFeature::Scrollbars::No);

    // 11. If scrollbars is false, then return true.
    if (scrollbars == TokenizedFeature::Scrollbars::No)
        return TokenizedFeature::Popup::Yes;

    // 12. Let status be the result of checking if a window feature is set, given tokenizedFeatures, "status", and false.
    auto status = check_if_a_window_feature_is_set("status"sv, TokenizedFeature::Status::No);

    // 13. If status is false, then return true.
    if (status == TokenizedFeature::Status::No)
        return TokenizedFeature::Popup::Yes;

    // 14. Return false.
    return TokenizedFeature::Popup::No;
}

}
