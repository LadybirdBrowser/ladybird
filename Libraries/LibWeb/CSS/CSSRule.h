/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/CSSRule.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Parser/SourcePosition.h>
#include <LibWeb/CSS/RustRule.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class WEB_API CSSRule : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CSSRule, css_rule)

public:
    using Type = RustRule::Type;

    Type type() const;
    RustRule native_rule() const { return RustRule { native_handle() }; }

    Utf16String css_text() const { return serialized(); }

    CSSRule* parent_rule() const;
    StyleSheetState* parent_style_sheet() const;

    Optional<SourcePosition> source_location() const;

    template<typename T>
    bool fast_is() const = delete;

    // https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
    Utf16String serialized() const;

protected:
    // The CSSStyleDeclaration of the declarations of the rule, which the first call creates.
    GC::Ref<CSSStyleDeclaration> declarations() const;

private:
    Parser::ValueParserFFI::NativeRule const* native_handle() const;
};

}
