/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSMathValue.h"
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSMathValue);

CSSMathValue::CSSMathValue(JS::Realm& realm, Bindings::CSSMathOperator operator_, NumericType type)
    : CSSNumericValue(realm, move(type))
    , m_operator(operator_)
{
}

void CSSMathValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSMathValue);
    Base::initialize(realm);
}

}
