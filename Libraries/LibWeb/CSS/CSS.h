/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

// https://www.w3.org/TR/cssom-1/#namespacedef-css
namespace Web::CSS {

struct PropertyDefinition {
    String name;
    String syntax;
    bool inherits;
    Optional<String> initial_value;
};

WEB_API WebIDL::ExceptionOr<String> escape(JS::VM&, StringView identifier);

WEB_API bool supports(JS::VM&, StringView property, StringView value);
WEB_API WebIDL::ExceptionOr<bool> supports(JS::VM&, StringView condition_text);

WEB_API WebIDL::ExceptionOr<void> register_property(JS::VM&, PropertyDefinition definition);

GC::Ref<CSSUnitValue> number(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> percent(JS::VM&, WebIDL::Double value);

// <length>
GC::Ref<CSSUnitValue> cap(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> ch(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> em(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> ex(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> ic(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> lh(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> rcap(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> rch(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> rem(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> rex(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> ric(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> rlh(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> vw(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> vh(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> vi(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> vb(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> vmin(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> vmax(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> svw(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> svh(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> svi(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> svb(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> svmin(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> svmax(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> lvw(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> lvh(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> lvi(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> lvb(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> lvmin(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> lvmax(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dvw(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dvh(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dvi(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dvb(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dvmin(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dvmax(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> cqw(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> cqh(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> cqi(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> cqb(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> cqmin(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> cqmax(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> cm(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> mm(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> q(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> in(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> pt(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> pc(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> px(JS::VM&, WebIDL::Double value);

// <angle>
GC::Ref<CSSUnitValue> deg(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> grad(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> rad(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> turn(JS::VM&, WebIDL::Double value);

// <time>
GC::Ref<CSSUnitValue> s(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> ms(JS::VM&, WebIDL::Double value);

// <frequency>
GC::Ref<CSSUnitValue> hz(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> k_hz(JS::VM&, WebIDL::Double value);

// <resolution>
GC::Ref<CSSUnitValue> dpi(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dpcm(JS::VM&, WebIDL::Double value);
GC::Ref<CSSUnitValue> dppx(JS::VM&, WebIDL::Double value);

// <flex>
GC::Ref<CSSUnitValue> fr(JS::VM&, WebIDL::Double value);

}
