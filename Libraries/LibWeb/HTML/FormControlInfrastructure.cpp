/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/Random.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/FormControlInfrastructure.h>
#include <LibWeb/HTML/FormDataEvent.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#create-an-entry
WebIDL::ExceptionOr<XHR::FormDataEntry> create_entry(JS::Realm& realm, String const& name, Variant<GC::Ref<FileAPI::Blob>, String> const& value, Optional<String> const& filename)
{
    auto& vm = realm.vm();

    // 1. Set name to the result of converting name into a scalar value string.
    auto entry_name = TRY_OR_THROW_OOM(vm, Infra::convert_to_scalar_value_string(name));

    auto entry_value = TRY(value.visit(
        // 2. If value is a string, then set value to the result of converting value into a scalar value string.
        [&](String const& string) -> WebIDL::ExceptionOr<Variant<GC::Root<FileAPI::File>, String>> {
            return TRY_OR_THROW_OOM(vm, Infra::convert_to_scalar_value_string(string));
        },
        // 3. Otherwise:
        [&](GC::Ref<FileAPI::Blob> blob) -> WebIDL::ExceptionOr<Variant<GC::Root<FileAPI::File>, String>> {
            // 1. If value is not a File object, then set value to a new File object, representing the same bytes, whose
            //    name attribute value is "blob".
            if (!is<FileAPI::File>(*blob)) {
                FileAPI::FilePropertyBag options {};
                options.type = blob->type();

                blob = TRY(FileAPI::File::create(realm, { GC::make_root(*blob) }, "blob"_string, move(options)));
            }

            // 2. If filename is given, then set value to a new File object, representing the same bytes, whose name
            //    attribute is filename.
            if (filename.has_value()) {
                FileAPI::FilePropertyBag options {};
                options.type = blob->type();

                blob = TRY(FileAPI::File::create(realm, { GC::make_root(*blob) }, *filename, move(options)));
            }

            return GC::make_root(as<FileAPI::File>(*blob));
        }));

    // 4. Return an entry whose name is name and whose value is value.
    return XHR::FormDataEntry {
        .name = move(entry_name),
        .value = move(entry_value),
    };
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constructing-the-form-data-set
WebIDL::ExceptionOr<Optional<Vector<XHR::FormDataEntry>>> construct_entry_list(JS::Realm& realm, HTMLFormElement& form, GC::Ptr<HTMLElement> submitter, Optional<String> encoding)
{
    // 1. If form's constructing entry list is true, then return null.
    if (form.constructing_entry_list())
        return Optional<Vector<XHR::FormDataEntry>> {};

    // 2. Set form's constructing entry list to true.
    form.set_constructing_entry_list(true);

    // 3. Let controls be a list of all the submittable elements whose form owner is form, in tree order.
    auto controls = form.get_submittable_elements();

    // 4. Let entry list be a new empty entry list.
    Vector<XHR::FormDataEntry> entry_list;

    // 5. For each element field in controls, in tree order:
    for (auto const& control : controls) {
        auto const* control_as_form_associated_element = dynamic_cast<HTML::FormAssociatedElement const*>(control.ptr());
        VERIFY(control_as_form_associated_element);

        // 1. If any of the following is true, then continue:
        // - The field element has a datalist element ancestor.
        if (control->first_ancestor_of_type<HTML::HTMLDataListElement>())
            continue;
        // - The field element is disabled.
        if (control->is_actually_disabled())
            continue;
        // - The field element is a button but it is not submitter.
        if (control_as_form_associated_element->is_button() && control.ptr() != submitter.ptr())
            continue;
        // - The field element is an input element whose type attribute is in the Checkbox state and whose checkedness is false.
        // - The field element is an input element whose type attribute is in the Radio Button state and whose checkedness is false.
        if (auto* input_element = dynamic_cast<HTML::HTMLInputElement*>(control.ptr())) {
            if ((input_element->type_state() == HTMLInputElement::TypeAttributeState::Checkbox || input_element->type_state() == HTMLInputElement::TypeAttributeState::RadioButton) && !input_element->checked())
                continue;
        }

        // 2. If the field element is an input element whose type attribute is in the Image Button state, then:
        if (auto* input_element = dynamic_cast<HTML::HTMLInputElement*>(control.ptr()); input_element && input_element->type_state() == HTMLInputElement::TypeAttributeState::ImageButton) {
            // 1. If the field element is not submitter, then continue.
            if (input_element != submitter.ptr())
                continue;

            // 2. If the field element has a name attribute specified and its value is not the empty string, let name be
            //    that value followed by U+002E (.). Otherwise, let name be the empty string.
            String name;
            if (auto value = input_element->get_attribute(AttributeNames::name); value.has_value() && !value->is_empty())
                name = MUST(String::formatted("{}.", *value));

            // 3. Let namex be the concatenation of name and U+0078 (x).
            auto name_x = MUST(String::formatted("{}x", name));

            // 4. Let namey be the concatenation of name and U+0079 (y).
            auto name_y = MUST(String::formatted("{}y", name));

            // 5. Let (x, y) be the selected coordinate.
            auto [x, y] = input_element->selected_coordinate();

            // 6. Create an entry with namex and x, and append it to entry list.
            entry_list.append(XHR::FormDataEntry { .name = move(name_x), .value = String::number(x) });

            // 7. Create an entry with namey and y, and append it to entry list.
            entry_list.append(XHR::FormDataEntry { .name = move(name_y), .value = String::number(y) });

            // 8. Continue.
            continue;
        }

        // FIXME: 3. If the field is a form-associated custom element, then perform the entry construction algorithm given field and entry list, then continue.

        // 4. If either the field element does not have a name attribute specified, or its name attribute's value is the empty string, then continue.
        if (!control->name().has_value() || control->name()->is_empty())
            continue;

        // 5. Let name be the value of the field element's name attribute.
        auto name = control->name().value();

        // 6. If the field element is a select element, then for each option element in the select element's list of options whose selectedness is true and that is not disabled, create an entry with name and the value of the option element, and append it to entry list.
        if (auto* select_element = dynamic_cast<HTML::HTMLSelectElement*>(control.ptr())) {
            for (auto const& option_element : select_element->list_of_options()) {
                if (option_element->selected() && !option_element->disabled()) {
                    entry_list.append(XHR::FormDataEntry { .name = name.to_string(), .value = option_element->value() });
                }
            }
        }
        // 7. Otherwise, if the field element is an input element whose type attribute is in the Checkbox state or the Radio Button state, then:
        else if (auto* checkbox_or_radio_element = dynamic_cast<HTML::HTMLInputElement*>(control.ptr()); checkbox_or_radio_element && (checkbox_or_radio_element->type_state() == HTMLInputElement::TypeAttributeState::Checkbox || checkbox_or_radio_element->type_state() == HTMLInputElement::TypeAttributeState::RadioButton) && checkbox_or_radio_element->checked()) {
            //  1. If the field element has a value attribute specified, then let value be the value of that attribute; otherwise, let value be the string "on".
            auto value = checkbox_or_radio_element->value();
            if (value.is_empty())
                value = "on"_string;

            // 2. Create an entry with name and value, and append it to entry list.
            auto checkbox_or_radio_element_name = checkbox_or_radio_element->name();
            entry_list.append(XHR::FormDataEntry { .name = checkbox_or_radio_element_name->to_string(), .value = move(value) });
        }
        // 8. Otherwise, if the field element is an input element whose type attribute is in the File Upload state, then:
        else if (auto* file_element = dynamic_cast<HTML::HTMLInputElement*>(control.ptr()); file_element && file_element->type_state() == HTMLInputElement::TypeAttributeState::FileUpload) {
            // 1. If there are no selected files, then create an entry with name and a new File object with an empty name, application/octet-stream as type, and an empty body, and append it to entry list.
            if (file_element->files()->length() == 0) {
                FileAPI::FilePropertyBag options {};
                options.type = "application/octet-stream"_string;
                auto file = TRY(FileAPI::File::create(realm, {}, String {}, options));
                entry_list.append(XHR::FormDataEntry { .name = name.to_string(), .value = GC::make_root(file) });
            }
            // 2. Otherwise, for each file in selected files, create an entry with name and a File object representing the file, and append it to entry list.
            else {
                for (size_t i = 0; i < file_element->files()->length(); i++) {
                    auto file = GC::Ref { *file_element->files()->item(i) };
                    entry_list.append(XHR::FormDataEntry { .name = name.to_string(), .value = GC::make_root(file) });
                }
            }
        }
        // 9. Otherwise, if the field element is an input element whose type attribute is in the Hidden state and name is an ASCII case-insensitive match for "_charset_":
        else if (auto* hidden_input = dynamic_cast<HTML::HTMLInputElement*>(control.ptr()); hidden_input && hidden_input->type_state() == HTMLInputElement::TypeAttributeState::Hidden && Infra::is_ascii_case_insensitive_match(name, "_charset_"sv)) {
            // 1. Let charset be the name of encoding if encoding is given, and "UTF-8" otherwise.
            auto charset = encoding.has_value() ? encoding.value() : "UTF-8"_string;

            // 2. Create an entry with name and charset, and append it to entry list.
            entry_list.append(XHR::FormDataEntry { .name = name.to_string(), .value = move(charset) });
        }
        // 10. Otherwise, create an entry with name and the value of the field element, and append it to entry list.
        else {
            entry_list.append(XHR::FormDataEntry { .name = name.to_string(), .value = control_as_form_associated_element->value() });
        }

        // 11. If the element has a dirname attribute, and that attribute's value is not the empty string, then:
        if (auto attribute = control->get_attribute(HTML::AttributeNames::dirname); attribute.has_value() && !attribute.value().is_empty()) {
            // 1. Let dirname be the value of the element's dirname attribute.
            String dirname = attribute.value();

            // 2. Let dir be the string "ltr" if the directionality of the element is 'ltr', and "rtl" otherwise (i.e., when the directionality of the element is 'rtl').
            String dir = MUST((control->directionality() == DOM::Element::Directionality::Ltr) ? String::from_utf8("ltr"sv) : String::from_utf8("rtl"sv));

            // 3. Create an entry with dirname and dir, and append it to entry list.
            entry_list.append(XHR::FormDataEntry { .name = dirname, .value = dir });
        }
    }
    // 6. Let form data be a new FormData object associated with entry list.
    auto form_data = TRY(XHR::FormData::construct_impl(realm, move(entry_list)));

    // 7. Fire an event named formdata at form using FormDataEvent, with the formData attribute initialized to form data and the bubbles attribute initialized to true.
    FormDataEventInit init {};
    init.form_data = form_data;
    auto form_data_event = TRY(FormDataEvent::construct_impl(realm, HTML::EventNames::formdata, init));
    form_data_event->set_bubbles(true);
    form.dispatch_event(form_data_event);

    // 8. Set form's constructing entry list to false.
    form.set_constructing_entry_list(false);

    // 9. Return a clone of entry list.
    return form_data->entry_list();
}

ErrorOr<String> normalize_line_breaks(StringView value)
{
    // Replace every occurrence of U+000D (CR) not followed by U+000A (LF), and every occurrence of U+000A (LF) not
    // preceded by U+000D (CR) by a string consisting of a U+000D (CR) and U+000A (LF).
    StringBuilder builder;
    GenericLexer lexer { value };
    while (!lexer.is_eof()) {
        TRY(builder.try_append(lexer.consume_until(is_any_of("\r\n"sv))));
        if ((lexer.peek() == '\r' && lexer.peek(1) != '\n') || lexer.peek() == '\n') {
            TRY(builder.try_append("\r\n"sv));
            lexer.ignore(1);
        } else {
            lexer.ignore(2);
        }
    }
    return builder.to_string();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#multipart/form-data-encoding-algorithm
ErrorOr<SerializedFormData> serialize_to_multipart_form_data(Vector<XHR::FormDataEntry> const& entry_list)
{
    auto escape_line_feed_carriage_return_double_quote = [](StringView value) -> ErrorOr<String> {
        StringBuilder builder;
        GenericLexer lexer { value };
        while (!lexer.is_eof()) {
            TRY(builder.try_append(lexer.consume_until(is_any_of("\r\n\""sv))));
            switch (lexer.peek()) {
            case '\r':
                TRY(builder.try_append("%0D"sv));
                break;
            case '\n':
                TRY(builder.try_append("%0A"sv));
                break;
            case '\"':
                TRY(builder.try_append("%22"sv));
                break;
            }
            lexer.ignore(1);
        }
        return builder.to_string();
    };

    // The boundary used by the user agent in generating the return value of this algorithm is the multipart/form-data boundary string.
    auto boundary = TRY(String::formatted("---------------------------{}", get_random<u64>()));
    StringBuilder builder;
    // 1. For each entry of entry list:
    for (auto const& entry : entry_list) {
        TRY(builder.try_append(TRY(String::formatted("--{}\r\n"sv, boundary))));

        // Replace every occurrence of U+000D (CR) not followed by U+000A (LF), and every occurrence of U+000A (LF) not preceded by U+000D (CR) by a string consisting of a U+000D (CR) and U+000A (LF).
        auto normalized_name = TRY(normalize_line_breaks(entry.name));
        // For field names replace any 0x0A (LF) bytes with the byte sequence `%0A`, 0x0D (CR) with `%0D` and 0x22 (") with `%22`
        auto escaped_name = TRY(escape_line_feed_carriage_return_double_quote(normalized_name));

        TRY(entry.value.visit(
            [&](GC::Root<FileAPI::File> const& file) -> ErrorOr<void> {
                // For filenames replace any 0x0A (LF) bytes with the byte sequence `%0A`, 0x0D (CR) with `%0D` and 0x22 (") with `%22`
                auto escaped_filename = TRY(escape_line_feed_carriage_return_double_quote(file->name()));
                // Add a `Content-Disposition` header with a `name` set to entry's name and `filename` set to entry's filename.
                TRY(builder.try_append(TRY(String::formatted("Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n", escaped_name, escaped_filename))));
                // The parts of the generated multipart/form-data resource that correspond to file fields must have a `Content-Type` header specified.
                TRY(builder.try_append(TRY(String::formatted("Content-Type: {}\r\n\r\n", file->type()))));
                TRY(builder.try_append(file->raw_bytes()));
                TRY(builder.try_append("\r\n"sv));
                return {};
            },
            [&](String const& string) -> ErrorOr<void> {
                // Replace every occurrence of U+000D (CR) not followed by U+000A (LF), and every occurrence of U+000A (LF) not preceded by U+000D (CR) by a string consisting of a U+000D (CR) and U+000A (LF).
                auto normalized_value = TRY(normalize_line_breaks(string));
                // Add a `Content-Disposition` header with a `name` set to entry's name.
                TRY(builder.try_append(TRY(String::formatted("Content-Disposition: form-data; name=\"{}\"\r\n\r\n", escaped_name))));
                TRY(builder.try_append(TRY(String::formatted("{}\r\n", normalized_value))));
                return {};
            }));
    }
    TRY(builder.try_append(TRY(String::formatted("--{}--\r\n", boundary))));

    // 2. Return the byte sequence resulting from encoding the entry list using the rules described by RFC 7578, Returning Values from Forms: multipart/form-data, given the following conditions: [RFC7578]
    auto serialized_data = TRY(builder.to_byte_buffer());
    return SerializedFormData {
        .boundary = move(boundary),
        .serialized_data = move(serialized_data)
    };
}

}
