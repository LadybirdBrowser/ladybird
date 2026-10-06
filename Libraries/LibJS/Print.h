/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Value.h>

namespace AK {

class Stream;
class Utf16StringBuilder;

}

namespace JS {

struct PrintContext {
    JS::VM& vm;
    AK::Stream* stream { nullptr };
    AK::Utf16StringBuilder* builder { nullptr };
    bool strip_ansi { false };
    bool raw_strings { false };
};

JS_API ErrorOr<void> print(JS::Value value, PrintContext&);

}
