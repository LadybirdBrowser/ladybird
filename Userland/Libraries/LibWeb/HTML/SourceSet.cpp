/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/SourceSet.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/Node.h>

namespace Web::HTML {

SourceSet::SourceSet()
    : m_source_size(CSS::Length::make_auto())
{
}

bool SourceSet::is_empty() const
{
    return m_sources.is_empty();
}

static double pixel_density(ImageSource const& image_source)
{
    return image_source.descriptor.get<ImageSource::PixelDensityDescriptorValue>().value;
}

// https://html.spec.whatwg.org/multipage/images.html#select-an-image-source-from-a-source-set
ImageSourceAndPixelDensity SourceSet::select_an_image_source()
{
    // 1. If an entry b in sourceSet has the same associated pixel density descriptor as an earlier entry a in sourceSet,
    //    then remove entry b.
    //    Repeat this step until none of the entries in sourceSet have the same associated pixel density descriptor
    //    as an earlier entry.

    Vector<ImageSource> unique_pixel_density_sources;
    HashMap<double, ImageSource> unique_pixel_density_sources_map;
    for (auto const& source : m_sources) {
        auto source_pixel_density = pixel_density(source);
        if (!unique_pixel_density_sources_map.contains(source_pixel_density)) {
            unique_pixel_density_sources.append(source);
            unique_pixel_density_sources_map.set(source_pixel_density, source);
        }
    }

    // 2. In an implementation-defined manner, choose one image source from sourceSet. Let this be selectedSource.
    //    In our case, select the lowest density greater than 1, otherwise the greatest density available.
    // 3. Return selectedSource and its associated pixel density.

    quick_sort(unique_pixel_density_sources, [](auto& a, auto& b) {
        return pixel_density(a) < pixel_density(b);
    });
    for (auto const& source : unique_pixel_density_sources) {
        if (pixel_density(source) >= 1) {
            return { source, pixel_density(source) };
        }
    }

    return { unique_pixel_density_sources.last(), pixel_density(unique_pixel_density_sources.last()) };
}

static StringView collect_a_sequence_of_code_points(Function<bool(u32 code_point)> condition, StringView input, size_t& position)
{
    // 1. Let result be the empty string.
    // 2. While position doesn’t point past the end of input and the code point at position within input meets the condition condition:
    //    1. Append that code point to the end of result.
    //    2. Advance position by 1.
    // 3. Return result.

    size_t start = position;
    while (position < input.length() && condition(input[position]))
        ++position;
    return input.substring_view(start, position - start);
}

// https://html.spec.whatwg.org/multipage/images.html#parse-a-srcset-attribute
SourceSet parse_a_srcset_attribute(StringView input)
{
    // 1. Let input be the value passed to this algorithm.

    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    size_t position = 0;

    // 3. Let candidates be an initially empty source set.
    SourceSet candidates;

splitting_loop:
    // 4. Splitting loop: Collect a sequence of code points that are ASCII whitespace or U+002C COMMA characters from input given position.
    //    If any U+002C COMMA characters were collected, that is a parse error.
    collect_a_sequence_of_code_points(
        [](u32 code_point) {
            if (code_point == ',') {
                // FIXME: Report a parse error somehow.
                return true;
            }
            return Infra::is_ascii_whitespace(code_point);
        },
        input, position);

    // 5. If position is past the end of input, return candidates.
    if (position >= input.length()) {
        return candidates;
    }

    // 6. Collect a sequence of code points that are not ASCII whitespace from input given position, and let that be url.
    auto url = collect_a_sequence_of_code_points(
        [](u32 code_point) { return !Infra::is_ascii_whitespace(code_point); },
        input, position);

    // 7. Let descriptors be a new empty list.
    Vector<String> descriptors;

    // 8. If url ends with U+002C (,), then:
    if (url.ends_with(',')) {
        // 1. Remove all trailing U+002C COMMA characters from url. If this removed more than one character, that is a parse error.
        while (url.ends_with(','))
            url = url.substring_view(0, url.length() - 1);
    }
    // Otherwise:
    else {
        // 1. Descriptor tokenizer: Skip ASCII whitespace within input given position.
        collect_a_sequence_of_code_points(
            [](u32 code_point) { return Infra::is_ascii_whitespace(code_point); },
            input, position);

        // 2. Let current descriptor be the empty string.
        StringBuilder current_descriptor;

        enum class State {
            InDescriptor,
            InParens,
            AfterDescriptor,
        };
        // 3. Let state be in descriptor.
        auto state = State::InDescriptor;

        // 4. Let c be the character at position. Do the following depending on the value of state.
        //    For the purpose of this step, "EOF" is a special character representing that position is past the end of input.
        for (;;) {
            Optional<u32> c;
            if (position < input.length()) {
                c = input[position];
            }

            switch (state) {
            // - In descriptor
            case State::InDescriptor:
                // Do the following, depending on the value of c:

                // - ASCII whitespace
                if (c.has_value() && Infra::is_ascii_whitespace(c.value())) {
                    // If current descriptor is not empty, append current descriptor to descriptors and let current descriptor be the empty string.
                    if (!current_descriptor.is_empty()) {
                        descriptors.append(current_descriptor.to_string().release_value_but_fixme_should_propagate_errors());
                    }
                    // Set state to after descriptor.
                    state = State::AfterDescriptor;
                }
                // U+002C COMMA (,)
                else if (c.has_value() && c.value() == ',') {
                    // Advance position to the next character in input.
                    position += 1;

                    // If current descriptor is not empty, append current descriptor to descriptors.
                    if (!current_descriptor.is_empty()) {
                        descriptors.append(current_descriptor.to_string().release_value_but_fixme_should_propagate_errors());
                    }

                    // Jump to the step labeled descriptor parser.
                    goto descriptor_parser;
                }

                // U+0028 LEFT PARENTHESIS (()
                else if (c.has_value() && c.value() == '(') {
                    // Append c to current descriptor.
                    current_descriptor.try_append_code_point(c.value()).release_value_but_fixme_should_propagate_errors();

                    // Set state to in parens.
                    state = State::InParens;
                }
                // EOF
                else if (!c.has_value()) {
                    // If current descriptor is not empty, append current descriptor to descriptors.
                    if (!current_descriptor.is_empty()) {
                        descriptors.append(current_descriptor.to_string().release_value_but_fixme_should_propagate_errors());
                    }

                    // Jump to the step labeled descriptor parser.
                    goto descriptor_parser;
                }
                // Anything else
                else {
                    // Append c to current descriptor.
                    current_descriptor.try_append_code_point(c.value()).release_value_but_fixme_should_propagate_errors();
                }
                break;

                // - In parens
            case State::InParens:
                // Do the following, depending on the value of c:
                // U+0029 RIGHT PARENTHESIS ())
                if (c.has_value() && c.value() == ')') {
                    // Append c to current descriptor.
                    current_descriptor.try_append_code_point(c.value()).release_value_but_fixme_should_propagate_errors();
                    // Set state to in descriptor.
                    state = State::InDescriptor;
                }
                // EOF
                else if (!c.has_value()) {
                    // Append current descriptor to descriptors.
                    descriptors.append(current_descriptor.to_string().release_value_but_fixme_should_propagate_errors());

                    // Jump to the step labeled descriptor parser.
                    goto descriptor_parser;
                }
                // Anything else
                else {
                    // Append c to current descriptor.
                    current_descriptor.try_append_code_point(c.value()).release_value_but_fixme_should_propagate_errors();
                }
                break;

                // - After descriptor
            case State::AfterDescriptor:
                // Do the following, depending on the value of c:
                // ASCII whitespace
                if (c.has_value() && Infra::is_ascii_whitespace(c.value())) {
                    // Stay in this state.
                }
                // EOF
                else if (!c.has_value()) {
                    // Jump to the step labeled descriptor parser.
                    goto descriptor_parser;
                }
                // Anything else
                else {
                    // Set state to in descriptor.
                    state = State::InDescriptor;
                    // Set position to the previous character in input.
                    position -= 1;
                }
                break;
            }
            // Advance position to the next character in input. Repeat this step.
            position += 1;
        }
    }
descriptor_parser:
    // 9. Descriptor parser: Let error be no.
    bool error = false;

    // 10. Let width be absent.
    Optional<int> width;

    // 11. Let density be absent.
    Optional<float> density;

    // 12. Let future-compat-h be absent.
    Optional<int> future_compat_h;

    // 13. For each descriptor in descriptors, run the appropriate set of steps from the following list:
    for (auto& descriptor : descriptors) {
        auto last_character = descriptor.bytes_as_string_view().bytes().last();
        auto descriptor_without_last_character = descriptor.bytes_as_string_view().substring_view(0, descriptor.bytes_as_string_view().length() - 1);

        auto as_int = descriptor_without_last_character.to_number<i32>();
        auto as_float = descriptor_without_last_character.to_number<float>();

        // - If the descriptor consists of a valid non-negative integer followed by a U+0077 LATIN SMALL LETTER W character
        if (last_character == 'w' && as_int.has_value()) {
            // NOOP: 1. If the user agent does not support the sizes attribute, let error be yes.

            // 2. If width and density are not both absent, then let error be yes.

            if (width.has_value() || density.has_value()) {
                error = true;
            }

            // FIXME: 3. Apply the rules for parsing non-negative integers to the descriptor.
            //           If the result is zero, let error be yes. Otherwise, let width be the result.
            width = as_int.value();
        }

        // - If the descriptor consists of a valid floating-point number followed by a U+0078 LATIN SMALL LETTER X character
        else if (last_character == 'x' && as_float.has_value()) {
            // 1. If width, density and future-compat-h are not all absent, then let error be yes.
            if (width.has_value() || density.has_value() || future_compat_h.has_value()) {
                error = true;
            }

            // FIXME: 2. Apply the rules for parsing floating-point number values to the descriptor.
            //           If the result is less than zero, let error be yes. Otherwise, let density be the result.
            density = as_float.value();
        }
        // - If the descriptor consists of a valid non-negative integer followed by a U+0068 LATIN SMALL LETTER H character
        else if (last_character == 'h' && as_int.has_value()) {
            // This is a parse error.
            // 1. If future-compat-h and density are not both absent, then let error be yes.
            if (future_compat_h.has_value() || density.has_value()) {
                error = true;
            }
            // FIXME: 2. Apply the rules for parsing non-negative integers to the descriptor.
            //           If the result is zero, let error be yes. Otherwise, let future-compat-h be the result.
            future_compat_h = as_int.value();
        }
        // - Anything else
        else {
            // Let error be yes.
            error = true;
        }
    }

    // 14. If future-compat-h is not absent and width is absent, let error be yes.
    if (future_compat_h.has_value() && !width.has_value()) {
        error = true;
    }

    // 15. If error is still no, then append a new image source to candidates whose URL is url,
    //     associated with a width width if not absent and a pixel density density if not absent.
    //     Otherwise, there is a parse error.
    if (!error) {
        ImageSource source;
        source.url = String::from_utf8(url).release_value_but_fixme_should_propagate_errors();
        if (width.has_value())
            source.descriptor = ImageSource::WidthDescriptorValue { width.value() };
        else if (density.has_value())
            source.descriptor = ImageSource::PixelDensityDescriptorValue { density.value() };
        candidates.m_sources.append(move(source));
    }

    // 16. Return to the step labeled splitting loop.
    goto splitting_loop;
}

// https://html.spec.whatwg.org/multipage/images.html#parse-a-sizes-attribute
CSS::LengthOrCalculated parse_a_sizes_attribute(DOM::Document const& document, StringView sizes)
{
    auto css_parser = CSS::Parser::Parser::create(CSS::Parser::ParsingContext { document }, sizes);
    return css_parser.parse_as_sizes_attribute();
}

// https://html.spec.whatwg.org/multipage/images.html#create-a-source-set
SourceSet SourceSet::create(DOM::Element const& element, String default_source, String srcset, String sizes)
{
    // 1. Let source set be an empty source set.
    SourceSet source_set;

    // 2. If srcset is not an empty string, then set source set to the result of parsing srcset.
    if (!srcset.is_empty())
        source_set = parse_a_srcset_attribute(srcset);

    // 3. Let source size be the result of parsing sizes.
    source_set.m_source_size = parse_a_sizes_attribute(element.document(), sizes);

    // 4. If default source is not the empty string and source set does not contain an image source
    //    with a pixel density descriptor value of 1, and no image source with a width descriptor,
    //    append default source to source set.
    if (!default_source.is_empty()) {
        bool contains_image_source_with_pixel_density_descriptor_value_of_1 = false;
        bool contains_image_source_with_width_descriptor = false;
        for (auto& source : source_set.m_sources) {
            if (source.descriptor.has<ImageSource::PixelDensityDescriptorValue>()) {
                if (source.descriptor.get<ImageSource::PixelDensityDescriptorValue>().value == 1.0)
                    contains_image_source_with_pixel_density_descriptor_value_of_1 = true;
            }
            if (source.descriptor.has<ImageSource::WidthDescriptorValue>())
                contains_image_source_with_width_descriptor = true;
        }
        if (!contains_image_source_with_pixel_density_descriptor_value_of_1 && !contains_image_source_with_width_descriptor)
            source_set.m_sources.append({ .url = default_source, .descriptor = {} });
    }

    // 5. Normalize the source densities of source set.
    source_set.normalize_source_densities(element);

    // 6. Return source set.
    return source_set;
}

// https://html.spec.whatwg.org/multipage/images.html#normalise-the-source-densities
void SourceSet::normalize_source_densities(DOM::Element const& element)
{
    // 1. Let source size be source set's source size.
    auto source_size = [&] {
        if (!m_source_size.is_calculated()) {
            // If the source size is viewport-relative, resolve it against the viewport right now.
            if (m_source_size.value().is_viewport_relative()) {
                return CSS::Length::make_px(m_source_size.value().viewport_relative_length_to_px(element.document().viewport_rect()));
            }

            // FIXME: Resolve font-relative lengths against the relevant font size.
            return m_source_size.value();
        }

        // HACK: Flush any pending layouts here so we get an up-to-date length resolution context.
        // FIXME: We should have a way to build a LengthResolutionContext for any DOM node without going through the layout tree.
        const_cast<DOM::Document&>(element.document()).update_layout();
        if (element.layout_node()) {
            auto context = CSS::Length::ResolutionContext::for_layout_node(*element.layout_node());
            return m_source_size.resolved(context);
        }
        // FIXME: This is wrong, but we don't have a better way to resolve lengths without a layout node yet.
        return CSS::Length::make_auto();
    }();

    // 2. For each image source in source set:
    for (auto& image_source : m_sources) {
        // 1. If the image source has a pixel density descriptor, continue to the next image source.
        if (image_source.descriptor.has<ImageSource::PixelDensityDescriptorValue>())
            continue;

        // 2. Otherwise, if the image source has a width descriptor,
        //    replace the width descriptor with a pixel density descriptor
        //    with a value of the width descriptor value divided by the source size and a unit of x.
        auto descriptor_value_set = false;
        if (image_source.descriptor.has<ImageSource::WidthDescriptorValue>()) {
            auto& width_descriptor = image_source.descriptor.get<ImageSource::WidthDescriptorValue>();
            if (source_size.is_absolute()) {
                auto source_size_in_pixels = source_size.absolute_length_to_px();
                if (source_size_in_pixels != 0) {
                    image_source.descriptor = ImageSource::PixelDensityDescriptorValue {
                        .value = (width_descriptor.value / source_size_in_pixels).to_double()
                    };
                    descriptor_value_set = true;
                }
            } else {
                dbgln("FIXME: Image element has unresolved relative length '{}' in sizes attribute", source_size);
            }
        }

        // 3. Otherwise, give the image source a pixel density descriptor of 1x.
        if (!descriptor_value_set) {
            image_source.descriptor = ImageSource::PixelDensityDescriptorValue {
                .value = 1.0f
            };
        }
    }
}
}
