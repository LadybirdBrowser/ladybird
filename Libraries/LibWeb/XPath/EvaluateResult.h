/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGC/Forward.h>

namespace Web::XPath {

class XPathResult;

enum class EvaluationError {
    InvalidExpression,
    EvaluationFailed,
};

using EvaluateResult = Variant<GC::Ref<XPathResult>, EvaluationError>;

}
