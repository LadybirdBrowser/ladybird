/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/CSSStyleValue.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue
// The C++ side of the CSS Typed OM, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/css): what C++
// creates and reads.
class WEB_API CSSStyleValue : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CSSStyleValue, css_style_value)

public:
    enum class PerformTypeCheck : u8 {
        No,
        Yes,
    };

    static GC::Ref<CSSStyleValue> create(Utf16FlyString associated_property, StyleValue const&, SerializationMode = SerializationMode::Normal);

    // https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
    static GC::Ref<CSSStyleValue> reify(StyleValue const&, Utf16FlyString const& associated_property);

    // https://drafts.css-houdini.org/css-typed-om-1/#parse-a-cssstylevalue, with parseMultiple set to false.
    static WebIDL::ExceptionOr<GC::Ref<CSSStyleValue>> parse_a_css_style_value(Utf16FlyString const& property, Utf16View css_text);

    Optional<Utf16FlyString> associated_property() const;

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const;

    bool is_unparsed_value() const;

    template<typename T>
    bool fast_is() const
    requires(IsSame<T, CSSNumericValue> || IsSame<T, CSSUnitValue>)
    {
        if constexpr (IsSame<T, CSSNumericValue>)
            return is_numeric_value();
        else
            return is_unit_value();
    }

private:
    bool is_numeric_value() const;
    bool is_unit_value() const;
};

}
