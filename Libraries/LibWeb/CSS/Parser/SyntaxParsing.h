/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

OwnPtr<SyntaxNode> parse_as_syntax(Vector<ComponentValue> const&);

}
