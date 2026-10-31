/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibUnicode/DateTimeFormat.h>
#include <LibUnicode/Locale.h>
#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::HTML {

// NB: Date and time inputs show their value as a row of fields, one for each component of the value, in the order
//     and with the separators the user's locale uses. Each field is edited on its own with the keyboard, and the
//     value only becomes non-empty once every field holds a number.

using FieldKind = HTMLInputElement::DateTimeField::Kind;

static constexpr i32 maximum_year = 275760;

// NB: A moment whose components all differ from each other, which tells the formatter's parts apart.
static constexpr double sample_epoch_milliseconds = 1793454965000.0; // 2026-10-31T13:56:05Z

static i32 current_year()
{
    auto days = UnixDateTime::now().seconds_since_epoch() / 86400;
    i32 year = 1970;
    while (days >= static_cast<i64>(days_in_year(year))) {
        days -= days_in_year(year);
        ++year;
    }
    return year;
}

static Optional<FieldKind> field_kind_for_part_type(Utf16View type)
{
    if (type == u"year"sv)
        return FieldKind::Year;
    if (type == u"month"sv)
        return FieldKind::Month;
    if (type == u"day"sv)
        return FieldKind::Day;
    if (type == u"hour"sv)
        return FieldKind::Hour;
    if (type == u"minute"sv)
        return FieldKind::Minute;
    if (type == u"second"sv)
        return FieldKind::Second;
    if (type == u"dayPeriod"sv)
        return FieldKind::DayPeriod;
    return {};
}

static Utf16View field_placeholder(FieldKind kind)
{
    switch (kind) {
    case FieldKind::Year:
        return u"yyyy"sv;
    case FieldKind::Month:
        return u"mm"sv;
    case FieldKind::Day:
        return u"dd"sv;
    case FieldKind::Week:
    case FieldKind::Hour:
    case FieldKind::Minute:
    case FieldKind::Second:
    case FieldKind::DayPeriod:
        return u"--"sv;
    }
    VERIFY_NOT_REACHED();
}

bool HTMLInputElement::date_time_fields_use_12_hour_clock() const
{
    return any_of(m_date_time_fields, [](auto const& field) { return field.kind == FieldKind::DayPeriod; });
}

// NB: Fields the user filled in that do not make a value, because some are still empty or they name a day that does
//     not exist, describe input that cannot be converted to a valid string.
bool HTMLInputElement::date_time_fields_describe_bad_input() const
{
    return m_value.is_empty() && any_of(m_date_time_fields, [](auto const& field) { return field.value.has_value(); });
}

Optional<HTMLInputElement::DateTimeField const&> HTMLInputElement::date_time_field(DateTimeField::Kind kind) const
{
    for (auto const& field : m_date_time_fields) {
        if (field.kind == kind)
            return field;
    }
    return {};
}

Optional<i32> HTMLInputElement::date_time_field_value(DateTimeField::Kind kind) const
{
    if (auto field = date_time_field(kind); field.has_value())
        return field->value;
    return {};
}

i32 HTMLInputElement::date_time_field_minimum(DateTimeField::Kind kind) const
{
    switch (kind) {
    case FieldKind::Year:
    case FieldKind::Month:
    case FieldKind::Day:
    case FieldKind::Week:
        return 1;
    case FieldKind::Hour:
        return date_time_fields_use_12_hour_clock() ? 1 : 0;
    case FieldKind::Minute:
    case FieldKind::Second:
    case FieldKind::DayPeriod:
        return 0;
    }
    VERIFY_NOT_REACHED();
}

i32 HTMLInputElement::date_time_field_maximum(DateTimeField::Kind kind) const
{
    switch (kind) {
    case FieldKind::Year:
        return maximum_year;
    case FieldKind::Month:
        return 12;
    case FieldKind::Day:
        // NB: A day the month does not have leaves the value empty, as the user may change the month next.
        return 31;
    case FieldKind::Week:
        return 53;
    case FieldKind::Hour:
        return date_time_fields_use_12_hour_clock() ? 12 : 23;
    case FieldKind::Minute:
    case FieldKind::Second:
        return 59;
    case FieldKind::DayPeriod:
        return 1;
    }
    VERIFY_NOT_REACHED();
}

bool HTMLInputElement::date_time_fields_want_seconds() const
{
    if (!first_is_one_of(type_state(), TypeAttributeState::Time, TypeAttributeState::LocalDateAndTime))
        return false;

    // NB: The value carries seconds when its time has more than hours and minutes.
    auto value = m_value.utf16_view();
    if (auto separator = value.find_code_unit_offset(u'T'); separator.has_value())
        value = value.substring_view(*separator + 1);
    if (value.length_in_code_units() > 5)
        return true;

    // NB: A step that is not a whole number of minutes lets the user pick seconds.
    if (auto step = get_attribute(AttributeNames::step); step.has_value()) {
        if (auto parsed = step->utf16_view().to_number<double>(); parsed.has_value() && *parsed > 0 && fmod(*parsed, 60) != 0)
            return true;
    }
    return false;
}

void HTMLInputElement::create_date_time_fields()
{
    VERIFY(m_date_time_fields_container);
    m_date_time_fields_container->remove_all_children();
    m_date_time_fields.clear();
    m_day_period_names.clear();
    m_active_date_time_field = 0;
    m_date_time_field_typed_digits = 0;
    m_date_time_fields_show_seconds = date_time_fields_want_seconds();

    Vector<Variant<FieldKind, Utf16String>> parts;

    if (type_state() == TypeAttributeState::Week) {
        parts.append("Week "_utf16);
        parts.append(FieldKind::Week);
        parts.append(", "_utf16);
        parts.append(FieldKind::Year);
    } else {
        Unicode::CalendarPattern pattern;
        if (type_state() != TypeAttributeState::Time) {
            pattern.year = Unicode::CalendarPatternStyle::Numeric;
            pattern.month = Unicode::CalendarPatternStyle::TwoDigit;
            if (type_state() != TypeAttributeState::Month)
                pattern.day = Unicode::CalendarPatternStyle::TwoDigit;
        }
        if (first_is_one_of(type_state(), TypeAttributeState::Time, TypeAttributeState::LocalDateAndTime)) {
            pattern.hour = Unicode::CalendarPatternStyle::TwoDigit;
            pattern.minute = Unicode::CalendarPatternStyle::TwoDigit;
            if (m_date_time_fields_show_seconds)
                pattern.second = Unicode::CalendarPatternStyle::TwoDigit;
        }

        auto formatter = Unicode::DateTimeFormat::create_for_pattern_options(Unicode::default_locale(), u"UTC"sv, pattern);
        for (auto const& part : formatter->format_to_parts(sample_epoch_milliseconds)) {
            if (auto kind = field_kind_for_part_type(part.type); kind.has_value())
                parts.append(*kind);
            else if (part.type == u"literal"sv)
                parts.append(part.value);
        }

        if (pattern.hour.has_value()) {
            // NB: The names of the two day periods, as the formatter writes them for a morning and an afternoon hour.
            for (auto hour : { 1, 13 }) {
                for (auto const& part : formatter->format_to_parts(hour * 3600000.0)) {
                    if (part.type == u"dayPeriod"sv)
                        m_day_period_names.append(part.value);
                }
            }
        }
    }

    auto& realm = HTML::relevant_realm(*this);
    for (auto const& part : parts) {
        if (auto const* literal = part.get_pointer<Utf16String>()) {
            MUST(m_date_time_fields_container->append_child(DOM::Text::create(document(), *literal)));
            continue;
        }

        auto kind = part.get<FieldKind>();
        if (kind == FieldKind::DayPeriod && m_day_period_names.size() != 2)
            continue;

        auto element = MUST(DOM::create_element(document(), HTML::TagNames::span, Namespace::HTML));
        MUST(element->append_child(DOM::Text::create(document(), {})));
        MUST(m_date_time_fields_container->append_child(element));

        auto index = m_date_time_fields.size();
        auto mousedown_function = JS::NativeFunction::create(
            realm, [this, index](JS::VM&) {
                if (index < m_date_time_fields.size())
                    set_active_date_time_field(index);
                return JS::js_undefined();
            },
            0, Utf16FlyString {}, &realm);
        auto mousedown_callback = GC::Heap::the().allocate<WebIDL::CallbackType>(*mousedown_function, realm);
        element->add_event_listener_without_options(UIEvents::EventNames::mousedown, DOM::IDLEventListener::create(mousedown_callback));

        m_date_time_fields.append({ .kind = kind, .value = {}, .element = element });
    }

    m_date_time_fields_value = {};
    update_date_time_fields();
}

void HTMLInputElement::update_date_time_fields()
{
    if (!m_date_time_fields_container)
        return;

    // NB: Script and the picker change the value; the fields then show it. Fields the user is still filling in keep
    //     their numbers while the value stays empty.
    if (m_date_time_fields_value != m_value) {
        // NB: Only a value from outside the fields decides whether they show seconds, so that clearing a field the
        //     user is editing does not build the fields again and lose what they typed into the others.
        if (date_time_fields_want_seconds() != m_date_time_fields_show_seconds) {
            create_date_time_fields();
            return;
        }

        m_date_time_fields_value = m_value;
        m_date_time_field_typed_digits = 0;
        for (auto& field : m_date_time_fields)
            field.value = {};

        auto set = [&](FieldKind kind, Optional<i32> value) {
            for (auto& field : m_date_time_fields) {
                if (field.kind == kind)
                    field.value = value;
            }
        };

        // NB: The value is sanitized, so its components are where its type puts them.
        auto value = m_value.utf16_view();
        Utf16View date_part;
        Utf16View time_part;
        switch (type_state()) {
        case TypeAttributeState::Date:
        case TypeAttributeState::Month:
        case TypeAttributeState::Week:
            date_part = value;
            break;
        case TypeAttributeState::Time:
            time_part = value;
            break;
        case TypeAttributeState::LocalDateAndTime:
            if (auto separator = value.find_code_unit_offset(u'T'); separator.has_value()) {
                date_part = value.substring_view(0, *separator);
                time_part = value.substring_view(*separator + 1);
            }
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        if (!date_part.is_empty()) {
            if (type_state() == TypeAttributeState::Week) {
                auto components = date_part.split_view(u"-W"sv, SplitBehavior::Nothing);
                if (components.size() == 2) {
                    set(FieldKind::Year, components[0].to_number<i32>());
                    set(FieldKind::Week, components[1].to_number<i32>());
                }
            } else {
                auto components = date_part.split_view(u'-', SplitBehavior::Nothing);
                if (components.size() >= 2) {
                    set(FieldKind::Year, components[0].to_number<i32>());
                    set(FieldKind::Month, components[1].to_number<i32>());
                }
                if (components.size() >= 3)
                    set(FieldKind::Day, components[2].to_number<i32>());
            }
        }

        if (!time_part.is_empty()) {
            auto components = time_part.split_view(u':', SplitBehavior::Nothing);
            if (components.size() >= 2) {
                auto hour = components[0].to_number<i32>();
                if (hour.has_value() && date_time_fields_use_12_hour_clock()) {
                    set(FieldKind::DayPeriod, *hour >= 12 ? 1 : 0);
                    hour = *hour % 12 == 0 ? 12 : *hour % 12;
                }
                set(FieldKind::Hour, hour);
                set(FieldKind::Minute, components[1].to_number<i32>());
            }
            if (components.size() >= 3)
                set(FieldKind::Second, components[2].substring_view(0, AK::min(2uz, components[2].length_in_code_units())).to_number<i32>());
        }
    }

    update_date_time_field_elements();
}

void HTMLInputElement::update_date_time_field_elements()
{
    bool const is_focused = document().focused_area().ptr() == this;
    for (size_t index = 0; index < m_date_time_fields.size(); ++index) {
        auto const& field = m_date_time_fields[index];

        Utf16String text;
        if (!field.value.has_value())
            text = Utf16String::from_utf16(field_placeholder(field.kind));
        else if (field.kind == FieldKind::DayPeriod)
            text = m_day_period_names[*field.value];
        else if (field.kind == FieldKind::Year)
            text = Utf16String::formatted("{:04}", *field.value);
        else
            text = Utf16String::formatted("{:02}", *field.value);

        auto& text_node = as<DOM::Text>(*field.element->first_child());
        if (text_node.data() != text)
            text_node.set_data(text);

        bool const is_active = is_focused && index == m_active_date_time_field;
        auto style = is_active
            ? "font-variant-numeric: tabular-nums; border-radius: 2px; background-color: AccentColor; color: AccentColorText;"_utf16
            : "font-variant-numeric: tabular-nums; border-radius: 2px;"_utf16;
        if (field.element->get_attribute_value(HTML::AttributeNames::style) != style)
            field.element->set_attribute_value(HTML::AttributeNames::style, style);
    }
}

void HTMLInputElement::set_active_date_time_field(size_t index)
{
    VERIFY(index < m_date_time_fields.size());
    m_active_date_time_field = index;
    m_date_time_field_typed_digits = 0;
    update_date_time_field_elements();
}

// NB: A step that is not a whole number of minutes adds a seconds field to a time input, and one that is takes it away.
void HTMLInputElement::update_date_time_fields_for_step()
{
    if (m_date_time_fields_container && date_time_fields_want_seconds() != m_date_time_fields_show_seconds)
        create_date_time_fields();
}

// NB: Moving focus backwards into the input lands on its last field, so that the fields are reached in reverse.
void HTMLInputElement::activate_last_date_time_field()
{
    if (!m_date_time_fields.is_empty())
        set_active_date_time_field(m_date_time_fields.size() - 1);
}

void HTMLInputElement::date_time_fields_did_change()
{
    Utf16String new_value;

    auto all_fields_filled = all_of(m_date_time_fields, [](auto const& field) { return field.value.has_value(); });
    if (all_fields_filled && !m_date_time_fields.is_empty()) {
        auto value_of = [&](FieldKind kind) { return date_time_field_value(kind).value_or(0); };

        auto hour = value_of(FieldKind::Hour);
        if (date_time_fields_use_12_hour_clock())
            hour = (hour % 12) + (value_of(FieldKind::DayPeriod) == 1 ? 12 : 0);
        auto time = date_time_field(FieldKind::Second).has_value()
            ? Utf16String::formatted("{:02}:{:02}:{:02}", hour, value_of(FieldKind::Minute), value_of(FieldKind::Second))
            : Utf16String::formatted("{:02}:{:02}", hour, value_of(FieldKind::Minute));

        switch (type_state()) {
        case TypeAttributeState::Date:
            new_value = Utf16String::formatted("{:04}-{:02}-{:02}", value_of(FieldKind::Year), value_of(FieldKind::Month), value_of(FieldKind::Day));
            break;
        case TypeAttributeState::Month:
            new_value = Utf16String::formatted("{:04}-{:02}", value_of(FieldKind::Year), value_of(FieldKind::Month));
            break;
        case TypeAttributeState::Week:
            new_value = Utf16String::formatted("{:04}-W{:02}", value_of(FieldKind::Year), value_of(FieldKind::Week));
            break;
        case TypeAttributeState::Time:
            new_value = time;
            break;
        case TypeAttributeState::LocalDateAndTime:
            new_value = Utf16String::formatted("{:04}-{:02}-{:02}T{}", value_of(FieldKind::Year), value_of(FieldKind::Month), value_of(FieldKind::Day), time);
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        // NB: Fields that do not make a valid value together, such as the 31st of a 30-day month, leave the value
        //     empty.
        new_value = value_sanitization_algorithm(new_value);
    }

    // NB: The fields already show the value they make, so it must not be parsed back into them.
    m_date_time_fields_value = new_value;
    update_date_time_field_elements();
    // AD-HOC: Whether the fields describe bad input changes which validity pseudo-classes match, even when the
    //         value stays empty.
    CSS::Invalidation::invalidate_style_after_validity_change(*this);
    commit_date_time_value(move(new_value));
}

bool HTMLInputElement::handle_date_time_field_key(UIEvents::KeyCode key, u32 modifiers, u32 code_point)
{
    if (m_date_time_fields.is_empty())
        return false;
    if (modifiers & (UIEvents::Mod_Ctrl | UIEvents::Mod_Alt | UIEvents::Mod_Super))
        return false;

    auto field_count = m_date_time_fields.size();
    auto move_to = [&](size_t index) {
        set_active_date_time_field(index);
        return true;
    };

    // NB: Tab moves between the fields before it moves focus away from the input.
    if (key == UIEvents::Key_Tab) {
        if (modifiers & UIEvents::Mod_Shift) {
            if (m_active_date_time_field > 0)
                return move_to(m_active_date_time_field - 1);
        } else if (m_active_date_time_field + 1 < field_count) {
            return move_to(m_active_date_time_field + 1);
        }
        return false;
    }
    if (key == UIEvents::Key_Left)
        return move_to(m_active_date_time_field > 0 ? m_active_date_time_field - 1 : 0);
    if (key == UIEvents::Key_Right)
        return move_to(AK::min(m_active_date_time_field + 1, field_count - 1));
    if (key == UIEvents::Key_Home)
        return move_to(0);
    if (key == UIEvents::Key_End)
        return move_to(field_count - 1);

    auto& field = m_date_time_fields[m_active_date_time_field];
    auto minimum = date_time_field_minimum(field.kind);
    auto maximum = date_time_field_maximum(field.kind);

    if (key == UIEvents::Key_Up || key == UIEvents::Key_Down) {
        if (!is_mutable())
            return true;
        bool const up = key == UIEvents::Key_Up;
        if (!field.value.has_value()) {
            // NB: An empty year starts at the current one; other fields start at the end of their range.
            if (field.kind == FieldKind::Year)
                field.value = current_year();
            else
                field.value = up ? minimum : maximum;
        } else if (up) {
            field.value = *field.value >= maximum ? minimum : *field.value + 1;
        } else {
            field.value = *field.value <= minimum ? maximum : *field.value - 1;
        }
        m_date_time_field_typed_digits = 0;
        date_time_fields_did_change();
        return true;
    }

    if (key == UIEvents::Key_Backspace || key == UIEvents::Key_Delete) {
        if (!is_mutable())
            return true;
        field.value = {};
        m_date_time_field_typed_digits = 0;
        date_time_fields_did_change();
        return true;
    }

    if (code_point == 0 || code_point < ' ')
        return false;

    if (!is_mutable())
        return true;

    if (field.kind == FieldKind::DayPeriod) {
        // NB: A day period is chosen by the first letter of its name.
        for (size_t index = 0; index < m_day_period_names.size(); ++index) {
            auto name = m_day_period_names[index].utf16_view();
            if (name.is_empty())
                continue;
            auto first = name.code_point_at(0);
            if (to_ascii_lowercase(first) == to_ascii_lowercase(code_point)) {
                field.value = static_cast<i32>(index);
                date_time_fields_did_change();
                // NB: The change event can change the type of the input, which builds other fields or none.
                if (m_active_date_time_field + 1 < m_date_time_fields.size())
                    set_active_date_time_field(m_active_date_time_field + 1);
                return true;
            }
        }
        return true;
    }

    if (is_ascii_digit(code_point)) {
        auto digit = static_cast<i32>(code_point - '0');
        auto maximum_digits = field.kind == FieldKind::Year ? 4 : 2;

        i32 typed = digit;
        if (m_date_time_field_typed_digits > 0 && field.value.has_value())
            typed = *field.value * 10 + digit;
        if (typed > maximum) {
            // NB: A digit that would overflow the field starts the field over.
            typed = digit;
            m_date_time_field_typed_digits = 0;
        }
        ++m_date_time_field_typed_digits;

        // NB: The field is done once no further digit could keep it in range.
        bool const is_complete = m_date_time_field_typed_digits >= maximum_digits || static_cast<i64>(typed) * 10 > maximum;
        field.value = is_complete ? AK::max(typed, minimum) : typed;

        if (is_complete) {
            m_date_time_field_typed_digits = 0;
            if (m_active_date_time_field + 1 < field_count)
                m_active_date_time_field = m_active_date_time_field + 1;
        }
        date_time_fields_did_change();
        return true;
    }

    // NB: A separator ends the field being typed into.
    if (m_date_time_field_typed_digits > 0) {
        if (field.value.has_value())
            field.value = AK::max(*field.value, minimum);
        m_date_time_field_typed_digits = 0;
        if (m_active_date_time_field + 1 < field_count)
            m_active_date_time_field = m_active_date_time_field + 1;
        date_time_fields_did_change();
    }
    return true;
}

}
