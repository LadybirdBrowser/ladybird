/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace JS::Bytecode {

// PutKind indicates how a property is being set.
// `Normal` is a normal `o.foo = x` or `o[foo] = x` operation.
// The others are used for object expressions.
#define JS_ENUMERATE_PUT_KINDS(X) \
    X(Normal)                     \
    X(Getter)                     \
    X(Setter)                     \
    X(Prototype)                  \
    X(Own) // Always sets an own property, never calls a setter.

enum class PutKind {
#define __JS_ENUMERATE_PUT_KIND(name) name,
    JS_ENUMERATE_PUT_KINDS(__JS_ENUMERATE_PUT_KIND)
#undef __JS_ENUMERATE_PUT_KIND
};

}
