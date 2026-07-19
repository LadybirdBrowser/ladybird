/*
 * Copyright (c) 2023, Emil Militzer <emil.militzer@posteo.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API DisplayStyleValue : public StyleValueWithDefaultOperators<DisplayStyleValue> {
public:
    static ValueComparingNonnullRefPtr<DisplayStyleValue const> create(Display const&);
    virtual ~DisplayStyleValue() override = default;

    void serialize(StringBuilder& builder, SerializationMode) const { builder.append(display().to_string()); }

    Display display() const { return bit_cast<Display>(m_value->display.raw); }

    bool properties_equal(DisplayStyleValue const& other) const { return display() == other.display(); }
    GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const;

private:
    explicit DisplayStyleValue(Display const& display)
        : StyleValueWithDefaultOperators(Type::Display, StyleValueFFI::rust_style_value_create_display(bit_cast<u32>(display)))
    {
        static_assert(sizeof(Display) == sizeof(u32));
    }
};

}
