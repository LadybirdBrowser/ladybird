/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Bytecode {

struct ClassElementDescriptor {
    enum class Kind : u8 {
        Method,
        Getter,
        Setter,
        Field,
        StaticInitializer,
    };

    Kind kind;
    bool is_static;
    bool is_private;
    Optional<Utf16FlyString> private_identifier;
    Optional<u32> shared_function_data_index;
    bool has_initializer { false };
    Optional<Value> literal_value;
};

struct ClassBlueprint {
    u32 constructor_shared_function_data_index;
    bool has_super_class;
    bool has_name;
    Utf16FlyString name;
    RefPtr<SourceCode const> source_code;
    size_t source_text_offset { 0 };
    size_t source_text_length { 0 };
    Vector<ClassElementDescriptor> elements;
};

}
