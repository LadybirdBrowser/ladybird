/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <AK/QuickSort.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/Animations/PseudoElementParsing.h>
#include <LibWeb/Bindings/KeyframeEffectPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(KeyframeEffect);

template<typename T>
WebIDL::ExceptionOr<Variant<T, Vector<T>>> convert_value_to_maybe_list(JS::Realm& realm, JS::Value value, Function<WebIDL::ExceptionOr<T>(JS::Value)>& value_converter)
{
    auto& vm = realm.vm();

    if (TRY(value.is_array(vm))) {
        Vector<T> offsets;

        auto iterator = TRY(JS::get_iterator(vm, value, JS::IteratorHint::Sync));
        auto values = TRY(JS::iterator_to_list(vm, iterator));
        for (auto const& element : values) {
            if (element.is_undefined()) {
                offsets.append({});
            } else {
                offsets.append(TRY(value_converter(element)));
            }
        }

        return offsets;
    }

    return TRY(value_converter(value));
}

enum class AllowLists {
    Yes,
    No,
};

template<AllowLists AL>
using KeyframeType = Conditional<AL == AllowLists::Yes, BasePropertyIndexedKeyframe, BaseKeyframe>;

// https://www.w3.org/TR/web-animations-1/#process-a-keyframe-like-object
template<AllowLists AL>
static WebIDL::ExceptionOr<KeyframeType<AL>> process_a_keyframe_like_object(JS::Realm& realm, JS::Value keyframe_input)
{
    auto& vm = realm.vm();

    Function<WebIDL::ExceptionOr<Optional<double>>(JS::Value)> to_offset = [&vm](JS::Value value) -> WebIDL::ExceptionOr<Optional<double>> {
        if (value.is_undefined())
            return Optional<double> {};
        auto double_value = TRY(value.to_double(vm));
        if (isnan(double_value) || isinf(double_value))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Invalid offset value: {}", TRY(value.to_string(vm)))) };
        return double_value;
    };

    Function<WebIDL::ExceptionOr<String>(JS::Value)> to_string = [&vm](JS::Value value) -> WebIDL::ExceptionOr<String> {
        return TRY(value.to_string(vm));
    };

    Function<WebIDL::ExceptionOr<Bindings::CompositeOperationOrAuto>(JS::Value)> to_composite_operation = [&vm](JS::Value value) -> WebIDL::ExceptionOr<Bindings::CompositeOperationOrAuto> {
        if (value.is_undefined())
            return Bindings::CompositeOperationOrAuto::Auto;

        auto string_value = TRY(value.to_string(vm));
        if (string_value == "replace")
            return Bindings::CompositeOperationOrAuto::Replace;
        if (string_value == "add")
            return Bindings::CompositeOperationOrAuto::Add;
        if (string_value == "accumulate")
            return Bindings::CompositeOperationOrAuto::Accumulate;
        if (string_value == "auto")
            return Bindings::CompositeOperationOrAuto::Auto;

        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid composite value"sv };
    };

    // 1. Run the procedure to convert an ECMAScript value to a dictionary type with keyframe input as the ECMAScript
    //    value, and the dictionary type depending on the value of the allow lists flag as follows:
    //
    //    -> If allow lists is true, use the following dictionary type: <BasePropertyIndexedKeyframe>.
    //    -> Otherwise, use the following dictionary type: <BaseKeyframe>.
    //
    //    Store the result of this procedure as keyframe output.

    KeyframeType<AL> keyframe_output;
    if (keyframe_input.is_nullish())
        return keyframe_output;

    auto& keyframe_object = keyframe_input.as_object();
    auto composite = TRY(keyframe_object.get("composite"_fly_string));
    if (composite.is_undefined())
        composite = JS::PrimitiveString::create(vm, "auto"_string);
    auto easing = TRY(keyframe_object.get("easing"_fly_string));
    if (easing.is_undefined())
        easing = JS::PrimitiveString::create(vm, "linear"_string);
    auto offset = TRY(keyframe_object.get("offset"_fly_string));

    if constexpr (AL == AllowLists::Yes) {
        keyframe_output.composite = TRY(convert_value_to_maybe_list(realm, composite, to_composite_operation));

        auto easing_maybe_list = TRY(convert_value_to_maybe_list(realm, easing, to_string));
        easing_maybe_list.visit(
            [&](String const& value) {
                keyframe_output.easing = EasingValue { value };
            },
            [&](Vector<String> const& values) {
                Vector<EasingValue> easing_values;
                for (auto& easing_value : values)
                    easing_values.append(easing_value);
                keyframe_output.easing = move(easing_values);
            });

        keyframe_output.offset = TRY(convert_value_to_maybe_list(realm, offset, to_offset));
    } else {
        keyframe_output.composite = TRY(to_composite_operation(composite));
        keyframe_output.easing = TRY(to_string(easing));
        keyframe_output.offset = TRY(to_offset(offset));
    }

    // 2. Build up a list of animatable properties as follows:
    //
    //    1. Let animatable properties be a list of property names (including shorthand properties that have longhand
    //       sub-properties that are animatable) that can be animated by the implementation.
    //    2. Convert each property name in animatable properties to the equivalent IDL attribute by applying the
    //       animation property name to IDL attribute name algorithm.

    // 3. Let input properties be the result of calling the EnumerableOwnNames operation with keyframe input as the
    //    object.

    // 4. Make up a new list animation properties that consists of all of the properties that are in both input
    //    properties and animatable properties, or which are in input properties and conform to the
    //    <custom-property-name> production.
    auto input_properties = TRY(keyframe_object.enumerable_own_property_names(JS::Object::PropertyKind::Key));

    Vector<String> animation_properties;
    Optional<JS::Value> all_value;

    for (auto const& input_property : input_properties) {
        if (!input_property.is_string())
            continue;

        auto name = input_property.as_string().utf8_string();
        if (name == "all"sv) {
            all_value = TRY(keyframe_object.get(vm.names.all));
            for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
                auto property = static_cast<CSS::PropertyID>(i);
                if (CSS::is_animatable_property(property))
                    animation_properties.append(String { CSS::string_from_property_id(property) });
            }
        } else {
            // Handle the two special cases
            if (name == "cssFloat"sv || name == "cssOffset"sv) {
                animation_properties.append(name);
            } else if (name == "float"sv || name == "offset"sv) {
                // Ignore these property names
            } else if (auto property = CSS::property_id_from_camel_case_string(name); property.has_value()) {
                if (CSS::is_animatable_property(property.value()))
                    animation_properties.append(name);
            }
        }
    }

    // 5. Sort animation properties in ascending order by the Unicode codepoints that define each property name.
    quick_sort(animation_properties);

    // 6. For each property name in animation properties,
    for (auto const& property_name : animation_properties) {
        // 1. Let raw value be the result of calling the [[Get]] internal method on keyframe input, with property name
        //    as the property key and keyframe input as the receiver.
        // 2. Check the completion record of raw value.
        JS::PropertyKey key { property_name, JS::PropertyKey::StringMayBeNumber::No };
        auto raw_value = TRY(keyframe_object.has_property(key)) ? TRY(keyframe_object.get(key)) : *all_value;

        using PropertyValuesType = Conditional<AL == AllowLists::Yes, Vector<String>, String>;
        PropertyValuesType property_values;

        // 3. Convert raw value to a DOMString or sequence of DOMStrings property values as follows:

        // -> If allow lists is true,
        if constexpr (AL == AllowLists::Yes) {
            // Let property values be the result of converting raw value to IDL type (DOMString or sequence<DOMString>)
            // using the procedures defined for converting an ECMAScript value to an IDL value [WEBIDL].
            auto intermediate_property_values = TRY(convert_value_to_maybe_list(realm, raw_value, to_string));

            // If property values is a single DOMString, replace property values with a sequence of DOMStrings with the
            // original value of property values as the only element.
            if (intermediate_property_values.has<String>())
                property_values = Vector { intermediate_property_values.get<String>() };
            else
                property_values = intermediate_property_values.get<Vector<String>>();
        }
        // -> Otherwise,
        else {
            // Let property values be the result of converting raw value to a DOMString using the procedure for
            // converting an ECMAScript value to a DOMString [WEBIDL].
            property_values = TRY(raw_value.to_string(vm));
        }

        // 4. Calculate the normalized property name as the result of applying the IDL attribute name to animation
        //    property name algorithm to property name.
        // Note: We do not need to do this, since we did not need to do the reverse step (animation property name to IDL
        //       attribute name) in the steps above.

        // 5. Add a property to keyframe output with normalized property name as the property name, and property values
        //    as the property value.
        if constexpr (AL == AllowLists::Yes) {
            keyframe_output.properties.set(property_name, property_values);
        } else {
            keyframe_output.unparsed_properties().set(property_name, property_values);
        }
    }

    return keyframe_output;
}

// https://www.w3.org/TR/web-animations-1/#compute-missing-keyframe-offsets
static void compute_missing_keyframe_offsets(Vector<BaseKeyframe>& keyframes)
{
    // 1. For each keyframe, in keyframes, let the computed keyframe offset of the keyframe be equal to its keyframe
    //    offset value.
    for (auto& keyframe : keyframes)
        keyframe.computed_offset = keyframe.offset;

    // 2. If keyframes contains more than one keyframe and the computed keyframe offset of the first keyframe in
    //    keyframes is null, set the computed keyframe offset of the first keyframe to 0.
    if (keyframes.size() > 1 && !keyframes[0].computed_offset.has_value())
        keyframes[0].computed_offset = 0.0;

    // 3. If the computed keyframe offset of the last keyframe in keyframes is null, set its computed keyframe offset
    //    to 1.
    if (!keyframes.is_empty() && !keyframes.last().computed_offset.has_value())
        keyframes.last().computed_offset = 1.0;

    // 4. For each pair of keyframes A and B where:
    //    - A appears before B in keyframes, and
    //    - A and B have a computed keyframe offset that is not null, and
    //    - all keyframes between A and B have a null computed keyframe offset,
    auto find_next_index_of_keyframe_with_computed_offset = [&](size_t starting_index) -> Optional<size_t> {
        for (size_t index = starting_index; index < keyframes.size(); index++) {
            if (keyframes[index].computed_offset.has_value())
                return index;
        }

        return {};
    };

    auto maybe_index_a = find_next_index_of_keyframe_with_computed_offset(0);
    if (!maybe_index_a.has_value())
        return;

    auto index_a = maybe_index_a.value();
    auto maybe_index_b = find_next_index_of_keyframe_with_computed_offset(index_a + 1);

    while (maybe_index_b.has_value()) {
        auto index_b = maybe_index_b.value();

        // calculate the computed keyframe offset of each keyframe between A and B as follows:
        for (size_t keyframe_index = index_a + 1; keyframe_index < index_b; keyframe_index++) {
            // 1. Let offsetk be the computed keyframe offset of a keyframe k.
            auto offset_a = keyframes[index_a].computed_offset.value();
            auto offset_b = keyframes[index_b].computed_offset.value();

            // 2. Let n be the number of keyframes between and including A and B minus 1.
            auto n = static_cast<double>(index_b - index_a);

            // 3. Let index refer to the position of keyframe in the sequence of keyframes between A and B such that the
            //    first keyframe after A has an index of 1.
            auto index = static_cast<double>(keyframe_index - index_a);

            // 4. Set the computed keyframe offset of keyframe to offsetA + (offsetB − offsetA) × index / n.
            keyframes[keyframe_index].computed_offset = (offset_a + (offset_b - offset_a)) * index / n;
        }

        index_a = index_b;
        maybe_index_b = find_next_index_of_keyframe_with_computed_offset(index_b + 1);
    }
}

// https://www.w3.org/TR/web-animations-1/#loosely-sorted-by-offset
static bool is_loosely_sorted_by_offset(Vector<BaseKeyframe> const& keyframes)
{
    // The list of keyframes for a keyframe effect must be loosely sorted by offset which means that for each keyframe
    // in the list that has a keyframe offset that is not null, the offset is greater than or equal to the offset of the
    // previous keyframe in the list with a keyframe offset that is not null, if any.

    Optional<double> last_offset;
    for (auto const& keyframe : keyframes) {
        if (!keyframe.offset.has_value())
            continue;

        if (last_offset.has_value() && keyframe.offset.value() < last_offset.value())
            return false;

        last_offset = keyframe.offset;
    }

    return true;
}

// https://www.w3.org/TR/web-animations-1/#process-a-keyframes-argument
static WebIDL::ExceptionOr<Vector<BaseKeyframe>> process_a_keyframes_argument(JS::Realm& realm, GC::Ptr<JS::Object> object)
{
    auto& vm = realm.vm();

    // 1. If object is null, return an empty sequence of keyframes.
    if (!object)
        return Vector<BaseKeyframe> {};

    // 2. Let processed keyframes be an empty sequence of keyframes.
    Vector<BaseKeyframe> processed_keyframes;
    Vector<EasingValue> unused_easings;

    // 3. Let method be the result of GetMethod(object, @@iterator).
    // 4. Check the completion record of method.
    auto method = TRY(JS::Value(object).get_method(vm, vm.well_known_symbol_iterator()));

    // 5. Perform the steps corresponding to the first matching condition from below,

    // -> If method is not undefined,
    if (method) {
        // 1. Let iter be GetIterator(object, method).
        // 2. Check the completion record of iter.
        auto iter = TRY(JS::get_iterator_from_method(vm, object, *method));

        // 3. Repeat:
        while (true) {
            // 1. Let next be IteratorStep(iter).
            // 2. Check the completion record of next.
            auto next = TRY(JS::iterator_step(vm, iter));

            // 3. If next is false abort this loop.
            if (!next)
                break;

            // 4. Let nextItem be IteratorValue(next).
            // 5. Check the completion record of nextItem.
            auto next_item = TRY(JS::iterator_value(vm, *next));

            // 6. If Type(nextItem) is not Undefined, Null or Object, then throw a TypeError and abort these steps.
            if (!next_item.is_nullish() && !next_item.is_object())
                return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOrNull, next_item.to_string_without_side_effects());

            // 7. Append to processed keyframes the result of running the procedure to process a keyframe-like object
            //    passing nextItem as the keyframe input and with the allow lists flag set to false.
            processed_keyframes.append(TRY(process_a_keyframe_like_object<AllowLists::No>(realm, next_item)));
        }
    }
    // -> Otherwise,
    else {
        // 1. Let property-indexed keyframe be the result of running the procedure to process a keyframe-like object
        //    passing object as the keyframe input and with the allow lists flag set to true.
        auto property_indexed_keyframe = TRY(process_a_keyframe_like_object<AllowLists::Yes>(realm, object));

        // 2. For each member, m, in property-indexed keyframe, perform the following steps:
        for (auto const& [property_name, property_values] : property_indexed_keyframe.properties) {
            // 1. Let property name be the key for m.

            // 2. If property name is "composite", or "easing", or "offset", skip the remaining steps in this loop and
            //    continue from the next member in property-indexed keyframe after m.
            // Note: This will never happen, since these fields have dedicated members on BasePropertyIndexedKeyframe

            // 3. Let property values be the value for m.

            // 4. Let property keyframes be an empty sequence of keyframes.
            Vector<BaseKeyframe> property_keyframes;

            // 5. For each value, v, in property values perform the following steps:
            for (auto const& value : property_values) {
                // 1. Let k be a new keyframe with a null keyframe offset.
                BaseKeyframe keyframe;

                // 2. Add the property-value pair, property name → v, to k.
                keyframe.unparsed_properties().set(property_name, value);

                // 3. Append k to property keyframes.
                property_keyframes.append(keyframe);
            }

            // 6. Apply the procedure to compute missing keyframe offsets to property keyframes.
            compute_missing_keyframe_offsets(property_keyframes);

            // 7. Add keyframes in property keyframes to processed keyframes.
            processed_keyframes.extend(move(property_keyframes));
        }

        // 3. Sort processed keyframes by the computed keyframe offset of each keyframe in increasing order.
        quick_sort(processed_keyframes, [](auto const& a, auto const& b) {
            return a.computed_offset.value() < b.computed_offset.value();
        });

        // 4. Merge adjacent keyframes in processed keyframes when they have equal computed keyframe offsets.
        // Note: The spec doesn't specify how to merge them, but WebKit seems to just override the properties of the
        //       earlier keyframe with the properties of the later keyframe.
        for (int i = 0; i < static_cast<int>(processed_keyframes.size() - 1); i++) {
            auto& keyframe_a = processed_keyframes[i];
            auto& keyframe_b = processed_keyframes[i + 1];

            if (keyframe_a.computed_offset.value() == keyframe_b.computed_offset.value()) {
                keyframe_a.easing = keyframe_b.easing;
                keyframe_a.composite = keyframe_b.composite;
                for (auto const& [property_name, property_value] : keyframe_b.unparsed_properties())
                    keyframe_a.unparsed_properties().set(property_name, property_value);
                processed_keyframes.remove(i + 1);
                i--;
            }
        }

        // 5. Let offsets be a sequence of nullable double values assigned based on the type of the "offset" member
        //    of the property-indexed keyframe as follows:
        //
        // -> sequence<double?>,
        //    The value of "offset" as-is.
        // -> double?,
        //    A sequence of length one with the value of "offset" as its single item, i.e. « offset »,
        auto offsets = property_indexed_keyframe.offset.has<Optional<double>>()
            ? Vector { property_indexed_keyframe.offset.get<Optional<double>>() }
            : property_indexed_keyframe.offset.get<Vector<Optional<double>>>();

        // 6. Assign each value in offsets to the keyframe offset of the keyframe with corresponding position in
        //    processed keyframes until the end of either sequence is reached.
        for (size_t i = 0; i < offsets.size() && i < processed_keyframes.size(); i++)
            processed_keyframes[i].offset = offsets[i];

        // 7. Let easings be a sequence of DOMString values assigned based on the type of the "easing" member of the
        //    property-indexed keyframe as follows:
        //
        // -> sequence<DOMString>,
        //    The value of "easing" as-is.
        // -> DOMString,
        //    A sequence of length one with the value of "easing" as its single item, i.e. « easing »,
        auto easings = property_indexed_keyframe.easing.has<EasingValue>()
            ? Vector { property_indexed_keyframe.easing.get<EasingValue>() }
            : property_indexed_keyframe.easing.get<Vector<EasingValue>>();

        // 8. If easings is an empty sequence, let it be a sequence of length one containing the single value "linear",
        //    i.e. « "linear" ».
        if (easings.is_empty())
            easings.append("linear"_string);

        // 9. If easings has fewer items than processed keyframes, repeat the elements in easings successively starting
        //    from the beginning of the list until easings has as many items as processed keyframes.
        //
        //    For example, if processed keyframes has five items, and easings is the sequence « "ease-in", "ease-out" »,
        //    easings would be repeated to become « "ease-in", "ease-out", "ease-in", "ease-out", "ease-in" ».
        size_t num_easings = easings.size();
        size_t index = 0;
        while (easings.size() < processed_keyframes.size())
            easings.append(easings[index++ % num_easings]);

        // 10. If easings has more items than processed keyframes, store the excess items as unused easings.
        while (easings.size() > processed_keyframes.size())
            unused_easings.append(easings.take_last());

        // 11. Assign each value in easings to a property named "easing" on the keyframe with the corresponding position
        //     in processed keyframes until the end of processed keyframes is reached.
        for (size_t i = 0; i < processed_keyframes.size(); i++)
            processed_keyframes[i].easing = easings[i];

        // 12. If the "composite" member of the property-indexed keyframe is not an empty sequence:
        auto composite_value = property_indexed_keyframe.composite;
        if (!composite_value.has<Vector<Bindings::CompositeOperationOrAuto>>() || !composite_value.get<Vector<Bindings::CompositeOperationOrAuto>>().is_empty()) {
            // 1. Let composite modes be a sequence of CompositeOperationOrAuto values assigned from the "composite"
            //    member of property-indexed keyframe. If that member is a single CompositeOperationOrAuto value
            //    operation, let composite modes be a sequence of length one, with the value of the "composite" as its
            //    single item.
            auto composite_modes = composite_value.has<Bindings::CompositeOperationOrAuto>()
                ? Vector { composite_value.get<Bindings::CompositeOperationOrAuto>() }
                : composite_value.get<Vector<Bindings::CompositeOperationOrAuto>>();

            // 2. As with easings, if composite modes has fewer items than processed keyframes, repeat the elements in
            //    composite modes successively starting from the beginning of the list until composite modes has as
            //    many items as processed keyframes.
            size_t num_composite_modes = composite_modes.size();
            index = 0;
            while (composite_modes.size() < processed_keyframes.size())
                composite_modes.append(composite_modes[index++ % num_composite_modes]);

            // 3. Assign each value in composite modes that is not auto to the keyframe-specific composite operation on
            //    the keyframe with the corresponding position in processed keyframes until the end of processed
            //    keyframes is reached.
            for (size_t i = 0; i < processed_keyframes.size(); i++) {
                if (composite_modes[i] != Bindings::CompositeOperationOrAuto::Auto)
                    processed_keyframes[i].composite = composite_modes[i];
            }
        }
    }

    // 6. If processed keyframes is not loosely sorted by offset, throw a TypeError and abort these steps.
    if (!is_loosely_sorted_by_offset(processed_keyframes))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Keyframes are not in ascending order based on offset"sv };

    // 7. If there exist any keyframe in processed keyframes whose keyframe offset is non-null and less than zero or
    //    greater than one, throw a TypeError and abort these steps.
    for (size_t i = 0; i < processed_keyframes.size(); i++) {
        auto const& keyframe = processed_keyframes[i];
        if (!keyframe.offset.has_value())
            continue;

        auto offset = keyframe.offset.value();
        if (offset < 0.0 || offset > 1.0)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Keyframe {} has invalid offset value {}"sv, i, offset)) };
    }

    // 8. For each frame in processed keyframes, perform the following steps:
    for (auto& keyframe : processed_keyframes) {
        // 1. For each property-value pair in frame, parse the property value using the syntax specified for that
        //    property.
        //
        //    If the property value is invalid according to the syntax for the property, discard the property-value pair.
        //    User agents that provide support for diagnosing errors in content SHOULD produce an appropriate warning
        //    highlight
        BaseKeyframe::ParsedProperties parsed_properties;
        for (auto& [property_string, value_string] : keyframe.unparsed_properties()) {
            Optional<CSS::PropertyID> property_id;

            // Handle some special cases
            if (property_string == "cssFloat"sv) {
                property_id = CSS::PropertyID::Float;
            } else if (property_string == "cssOffset"sv) {
                // FIXME: Support CSS offset property
            } else if (property_string == "float"sv || property_string == "offset"sv) {
                // Ignore these properties
            } else if (auto property = CSS::property_id_from_camel_case_string(property_string); property.has_value()) {
                property_id = *property;
            }

            if (!property_id.has_value())
                continue;

            if (auto style_value = parse_css_value(CSS::Parser::ParsingParams(), value_string, *property_id)) {
                // Handle 'initial' here so we don't have to get the default value of the property every frame in StyleComputer
                if (style_value->is_initial())
                    style_value = CSS::property_initial_value(*property_id);
                parsed_properties.set(*property_id, *style_value);
            }
        }
        keyframe.properties.set(move(parsed_properties));

        // 2. Let the timing function of frame be the result of parsing the "easing" property on frame using the CSS
        //    syntax defined for the easing member of the EffectTiming dictionary.
        //
        //    If parsing the "easing" property fails, throw a TypeError and abort this procedure.
        auto easing_string = keyframe.easing.get<String>();
        auto easing_value = AnimationEffect::parse_easing_string(easing_string);

        if (!easing_value)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Invalid animation easing value: \"{}\"", easing_string)) };

        keyframe.easing.set(NonnullRefPtr<CSS::CSSStyleValue const> { *easing_value });
    }

    // 9. Parse each of the values in unused easings using the CSS syntax defined for easing member of the EffectTiming
    //    interface, and if any of the values fail to parse, throw a TypeError and abort this procedure.
    for (auto& unused_easing : unused_easings) {
        auto easing_string = unused_easing.get<String>();
        auto easing_value = AnimationEffect::parse_easing_string(easing_string);
        if (!easing_value)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Invalid animation easing value: \"{}\"", easing_string)) };
    }

    return processed_keyframes;
}

// https://www.w3.org/TR/css-animations-2/#keyframe-processing
void KeyframeEffect::generate_initial_and_final_frames(RefPtr<KeyFrameSet> keyframe_set, HashTable<CSS::PropertyID> const& animated_properties)
{
    // 1. Find or create the initial keyframe, a keyframe with a keyframe offset of 0%, default timing function
    //    as its keyframe timing function, and default composite as its keyframe composite.
    KeyFrameSet::ResolvedKeyFrame* initial_keyframe;
    if (auto existing_keyframe = keyframe_set->keyframes_by_key.find(0)) {
        initial_keyframe = existing_keyframe;
    } else {
        keyframe_set->keyframes_by_key.insert(0, {});
        initial_keyframe = keyframe_set->keyframes_by_key.find(0);
    }

    // 2. For any property in animated properties that is not otherwise present in a keyframe with an offset of
    //    0% or one that would be positioned earlier in the used keyframe order, add the computed value of that
    //    property on element to initial keyframe’s keyframe values.
    for (auto property : animated_properties) {
        if (!initial_keyframe->properties.contains(property))
            initial_keyframe->properties.set(property, KeyFrameSet::UseInitial {});
    }

    // 3. If initial keyframe’s keyframe values is not empty, prepend initial keyframe to keyframes.

    // 4. Repeat for final keyframe, using an offset of 100%, considering keyframes positioned later in the used
    //    keyframe order, and appending to keyframes.
    KeyFrameSet::ResolvedKeyFrame* final_keyframe;
    if (auto existing_keyframe = keyframe_set->keyframes_by_key.find(100 * AnimationKeyFrameKeyScaleFactor)) {
        final_keyframe = existing_keyframe;
    } else {
        keyframe_set->keyframes_by_key.insert(100 * AnimationKeyFrameKeyScaleFactor, {});
        final_keyframe = keyframe_set->keyframes_by_key.find(100 * AnimationKeyFrameKeyScaleFactor);
    }

    for (auto property : animated_properties) {
        if (!final_keyframe->properties.contains(property))
            final_keyframe->properties.set(property, KeyFrameSet::UseInitial {});
    }
}

// https://www.w3.org/TR/web-animations-1/#animation-composite-order
int KeyframeEffect::composite_order(GC::Ref<KeyframeEffect> a, GC::Ref<KeyframeEffect> b)
{
    // 1. Let the associated animation of an animation effect be the animation associated with the animation effect.
    auto a_animation = a->associated_animation();
    auto b_animation = b->associated_animation();

    // 2. Sort A and B by applying the following conditions in turn until the order is resolved,

    //    1. If A and B’s associated animations differ by class, sort by any inter-class composite order defined for
    //       the corresponding classes.
    auto a_class = a_animation->animation_class();
    auto b_class = b_animation->animation_class();

    // From https://www.w3.org/TR/css-animations-2/#animation-composite-order:
    // "CSS Animations with an owning element have a later composite order than CSS Transitions but an earlier
    // composite order than animations without a specific animation class."
    if (a_class != b_class)
        return to_underlying(a_class) - to_underlying(b_class);

    //    2. If A and B are still not sorted, sort by any class-specific composite order defined by the common class of
    //       A and B’s associated animations.
    if (auto order = a_animation->class_specific_composite_order(*b_animation); order.has_value())
        return order.value();

    //    3. If A and B are still not sorted, sort by the position of their associated animations in the global
    //       animation list.
    return a_animation->global_animation_list_order() - b_animation->global_animation_list_order();
}

GC::Ref<KeyframeEffect> KeyframeEffect::create(JS::Realm& realm)
{
    return realm.create<KeyframeEffect>(realm);
}

// https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-keyframeeffect
WebIDL::ExceptionOr<GC::Ref<KeyframeEffect>> KeyframeEffect::construct_impl(
    JS::Realm& realm,
    GC::Root<DOM::Element> const& target,
    Optional<GC::Root<JS::Object>> const& keyframes,
    Variant<double, KeyframeEffectOptions> options)
{
    // 1. Create a new KeyframeEffect object, effect.
    auto effect = realm.create<KeyframeEffect>(realm);

    // 2. Set the target element of effect to target.
    effect->set_target(target);

    // 3. Set the target pseudo-selector to the result corresponding to the first matching condition from below.

    //    If options is a KeyframeEffectOptions object with a pseudoElement property,
    if (options.has<KeyframeEffectOptions>()) {
        // Set the target pseudo-selector to the value of the pseudoElement property.
        //
        // When assigning this property, the error-handling defined for the pseudoElement setter on the interface is
        // applied. If the setter requires an exception to be thrown, this procedure must throw the same exception and
        // abort all further steps.
        TRY(effect->set_pseudo_element(options.get<KeyframeEffectOptions>().pseudo_element));
    }
    //     Otherwise,
    else {
        // Set the target pseudo-selector to null.
        // Note: This is the default when constructed
    }

    // 4. Let timing input be the result corresponding to the first matching condition from below.
    KeyframeEffectOptions timing_input;

    //     If options is a KeyframeEffectOptions object,
    if (options.has<KeyframeEffectOptions>()) {
        // Let timing input be options.
        timing_input = options.get<KeyframeEffectOptions>();
    }
    //     Otherwise (if options is a double),
    else {
        // Let timing input be a new EffectTiming object with all members set to their default values and duration set
        // to options.
        timing_input.duration = options.get<double>();
    }

    // 5. Call the procedure to update the timing properties of an animation effect of effect from timing input.
    //    If that procedure causes an exception to be thrown, propagate the exception and abort this procedure.
    TRY(effect->update_timing(timing_input.to_optional_effect_timing()));

    // 6. If options is a KeyframeEffectOptions object, assign the composite property of effect to the corresponding
    //    value from options.
    //
    //    When assigning this property, the error-handling defined for the corresponding setter on the KeyframeEffect
    //    interface is applied. If the setter requires an exception to be thrown for the value specified by options,
    //    this procedure must throw the same exception and abort all further steps.
    if (options.has<KeyframeEffectOptions>())
        effect->set_composite(options.get<KeyframeEffectOptions>().composite);

    // 7. Initialize the set of keyframes by performing the procedure defined for setKeyframes() passing keyframes as
    //    the input.
    TRY(effect->set_keyframes(keyframes));

    return effect;
}

// https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-keyframeeffect-source
WebIDL::ExceptionOr<GC::Ref<KeyframeEffect>> KeyframeEffect::construct_impl(JS::Realm& realm, GC::Ref<KeyframeEffect> source)
{
    // 1. Create a new KeyframeEffect object, effect.
    auto effect = realm.create<KeyframeEffect>(realm);

    // 2. Set the following properties of effect using the corresponding values of source:

    //   - effect target,
    effect->m_target_element = source->target();

    //   - keyframes,
    effect->m_keyframes = source->m_keyframes;

    //   - composite operation, and
    effect->set_composite(source->composite());

    //   - all specified timing properties:

    //     - start delay,
    effect->m_start_delay = source->m_start_delay;

    //     - end delay,
    effect->m_end_delay = source->m_end_delay;

    //     - fill mode,
    effect->m_fill_mode = source->m_fill_mode;

    //     - iteration start,
    effect->m_iteration_start = source->m_iteration_start;

    //     - iteration count,
    effect->m_iteration_count = source->m_iteration_count;

    //     - iteration duration,
    effect->m_iteration_duration = source->m_iteration_duration;

    //     - playback direction, and
    effect->m_playback_direction = source->m_playback_direction;

    //     - timing function.
    effect->m_timing_function = source->m_timing_function;

    return effect;
}

void KeyframeEffect::set_target(DOM::Element* target)
{
    if (auto animation = this->associated_animation()) {
        if (m_target_element)
            m_target_element->disassociate_with_animation(*animation);
        if (target)
            target->associate_with_animation(*animation);
    }
    m_target_element = target;
}

Optional<String> KeyframeEffect::pseudo_element() const
{
    if (!m_target_pseudo_selector.has_value())
        return {};
    return m_target_pseudo_selector->serialize();
}

// https://drafts.csswg.org/web-animations-1/#dom-keyframeeffect-pseudoelement
WebIDL::ExceptionOr<void> KeyframeEffect::set_pseudo_element(Optional<String> value)
{
    // On setting, sets the target pseudo-selector of the animation effect to the result of
    // pseudo-element parsing on the provided value, defined as the following:
    // NOTE: The actual definition is in pseudo_element_parsing().
    m_target_pseudo_selector = TRY(pseudo_element_parsing(realm(), value));
    return {};
}

Optional<CSS::PseudoElement> KeyframeEffect::pseudo_element_type() const
{
    if (!m_target_pseudo_selector.has_value())
        return {};
    return m_target_pseudo_selector->type();
}

// https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-getkeyframes
WebIDL::ExceptionOr<GC::RootVector<JS::Object*>> KeyframeEffect::get_keyframes()
{
    if (m_keyframe_objects.size() != m_keyframes.size()) {
        auto& vm = this->vm();
        auto& realm = this->realm();

        // Recalculate the keyframe objects
        VERIFY(m_keyframe_objects.size() == 0);

        for (auto& keyframe : m_keyframes) {
            auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
            TRY(object->set(vm.names.offset, keyframe.offset.has_value() ? JS::Value(keyframe.offset.value()) : JS::js_null(), ShouldThrowExceptions::Yes));
            TRY(object->set(vm.names.computedOffset, JS::Value(keyframe.computed_offset.value()), ShouldThrowExceptions::Yes));
            auto easing_value = keyframe.easing.get<NonnullRefPtr<CSS::CSSStyleValue const>>();
            TRY(object->set(vm.names.easing, JS::PrimitiveString::create(vm, easing_value->to_string(CSS::CSSStyleValue::SerializationMode::Normal)), ShouldThrowExceptions::Yes));

            if (keyframe.composite == Bindings::CompositeOperationOrAuto::Replace) {
                TRY(object->set(vm.names.composite, JS::PrimitiveString::create(vm, "replace"sv), ShouldThrowExceptions::Yes));
            } else if (keyframe.composite == Bindings::CompositeOperationOrAuto::Add) {
                TRY(object->set(vm.names.composite, JS::PrimitiveString::create(vm, "add"sv), ShouldThrowExceptions::Yes));
            } else if (keyframe.composite == Bindings::CompositeOperationOrAuto::Accumulate) {
                TRY(object->set(vm.names.composite, JS::PrimitiveString::create(vm, "accumulate"sv), ShouldThrowExceptions::Yes));
            } else {
                TRY(object->set(vm.names.composite, JS::PrimitiveString::create(vm, "auto"sv), ShouldThrowExceptions::Yes));
            }

            for (auto const& [id, value] : keyframe.parsed_properties()) {
                auto value_string = JS::PrimitiveString::create(vm, value->to_string(CSS::CSSStyleValue::SerializationMode::Normal));
                TRY(object->set(JS::PropertyKey { CSS::camel_case_string_from_property_id(id), JS::PropertyKey::StringMayBeNumber::No }, value_string, ShouldThrowExceptions::Yes));
            }

            m_keyframe_objects.append(object);
        }
    }

    GC::RootVector<JS::Object*> keyframes { heap() };
    for (auto const& keyframe : m_keyframe_objects)
        keyframes.append(keyframe);
    return keyframes;
}

// https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-setkeyframes
WebIDL::ExceptionOr<void> KeyframeEffect::set_keyframes(Optional<GC::Root<JS::Object>> const& keyframe_object)
{
    m_keyframe_objects.clear();
    m_keyframes = TRY(process_a_keyframes_argument(realm(), keyframe_object.has_value() ? GC::Ptr { keyframe_object->ptr() } : GC::Ptr<Object> {}));
    // FIXME: After processing the keyframe argument, we need to turn the set of keyframes into a set of computed
    //        keyframes using the procedure outlined in the second half of
    //        https://www.w3.org/TR/web-animations-1/#calculating-computed-keyframes. For now, just compute the
    //        missing keyframe offsets
    compute_missing_keyframe_offsets(m_keyframes);

    auto keyframe_set = adopt_ref(*new KeyFrameSet);
    m_target_properties.clear();
    auto target = this->target();

    for (auto& keyframe : m_keyframes) {
        Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame resolved_keyframe;

        auto key = static_cast<u64>(keyframe.computed_offset.value() * 100 * AnimationKeyFrameKeyScaleFactor);

        for (auto [property_id, property_value] : keyframe.parsed_properties()) {
            if (property_value->is_unresolved() && target)
                property_value = CSS::Parser::Parser::resolve_unresolved_style_value(CSS::Parser::ParsingParams { target->document() }, *target, pseudo_element_type(), property_id, property_value->as_unresolved());
            CSS::StyleComputer::for_each_property_expanding_shorthands(property_id, property_value, CSS::StyleComputer::AllowUnresolved::Yes, [&](CSS::PropertyID shorthand_id, CSS::CSSStyleValue const& shorthand_value) {
                m_target_properties.set(shorthand_id);
                resolved_keyframe.properties.set(shorthand_id, NonnullRefPtr<CSS::CSSStyleValue const> { shorthand_value });
            });
        }

        keyframe_set->keyframes_by_key.insert(key, resolved_keyframe);
    }

    generate_initial_and_final_frames(keyframe_set, m_target_properties);
    m_key_frame_set = keyframe_set;

    return {};
}

KeyframeEffect::KeyframeEffect(JS::Realm& realm)
    : AnimationEffect(realm)
{
}

void KeyframeEffect::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(KeyframeEffect);
}

void KeyframeEffect::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target_element);
    visitor.visit(m_keyframe_objects);
}

static CSS::RequiredInvalidationAfterStyleChange compute_required_invalidation(HashMap<CSS::PropertyID, NonnullRefPtr<CSS::CSSStyleValue const>> const& old_properties, HashMap<CSS::PropertyID, NonnullRefPtr<CSS::CSSStyleValue const>> const& new_properties)
{
    CSS::RequiredInvalidationAfterStyleChange invalidation;
    auto old_and_new_properties = MUST(Bitmap::create(to_underlying(CSS::last_property_id) + 1, 0));
    for (auto const& [property_id, _] : old_properties)
        old_and_new_properties.set(to_underlying(property_id), 1);
    for (auto const& [property_id, _] : new_properties)
        old_and_new_properties.set(to_underlying(property_id), 1);
    for (auto i = to_underlying(CSS::first_property_id); i <= to_underlying(CSS::last_property_id); ++i) {
        if (!old_and_new_properties.get(i))
            continue;
        auto property_id = static_cast<CSS::PropertyID>(i);
        auto const* old_value = old_properties.get(property_id).value_or({});
        auto const* new_value = new_properties.get(property_id).value_or({});
        if (!old_value && !new_value)
            continue;
        invalidation |= compute_property_invalidation(property_id, old_value, new_value);
    }
    return invalidation;
}

void KeyframeEffect::update_computed_properties()
{
    auto target = this->target();
    if (!target || !target->is_connected())
        return;

    if (target->has_inclusive_ancestor_with_display_none()) {
        // FIXME: Reaching this point means we failed to cancel animation for an element that started
        //        being nested in "display: none".
        //        For now this hack is needed to avoid lots of unnecessary work.
        return;
    }

    GC::Ptr<CSS::ComputedProperties> style = {};
    if (!pseudo_element_type().has_value())
        style = target->computed_properties();
    else
        style = target->pseudo_element_computed_properties(pseudo_element_type().value());

    if (!style)
        return;

    auto animated_properties_before_update = style->animated_property_values();

    auto& document = target->document();
    document.style_computer().collect_animation_into(*target, pseudo_element_type(), *this, *style, CSS::StyleComputer::AnimationRefresh::Yes);

    auto invalidation = compute_required_invalidation(animated_properties_before_update, style->animated_property_values());

    if (invalidation.is_none())
        return;

    // Traversal of the subtree is necessary to update the animated properties inherited from the target element.
    target->for_each_in_subtree_of_type<DOM::Element>([&](auto& element) {
        auto element_invalidation = element.recompute_inherited_style();
        if (element_invalidation.is_none())
            return TraversalDecision::SkipChildrenAndContinue;
        invalidation |= element_invalidation;
        return TraversalDecision::Continue;
    });

    if (!pseudo_element_type().has_value()) {
        if (target->layout_node())
            target->layout_node()->apply_style(*style);
    } else {
        auto pseudo_element_node = target->get_pseudo_element_node(pseudo_element_type().value());
        if (auto* node_with_style = dynamic_cast<Layout::NodeWithStyle*>(pseudo_element_node.ptr())) {
            node_with_style->apply_style(*style);
        }
    }

    if (invalidation.relayout)
        target->set_needs_layout_update(DOM::SetNeedsLayoutReason::KeyframeEffect);
    if (invalidation.rebuild_layout_tree) {
        // We mark layout tree for rebuild starting from parent element to correctly invalidate
        // "display" property change to/from "contents" value.
        if (auto* parent_element = target->parent_element()) {
            parent_element->set_needs_layout_tree_update(true);
        } else {
            target->set_needs_layout_tree_update(true);
        }
    }
    if (invalidation.repaint) {
        document.set_needs_display();
        document.set_needs_to_resolve_paint_only_properties();
    }
    if (invalidation.rebuild_stacking_context_tree)
        document.invalidate_stacking_context_tree();
}

}
