/*
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class TypedArrayPrototype final : public Object {
    JS_OBJECT(TypedArrayPrototype, Object);
    JS_DECLARE_ALLOCATOR(TypedArrayPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~TypedArrayPrototype() override = default;

private:
    explicit TypedArrayPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(buffer_getter);
    JS_DECLARE_NATIVE_FUNCTION(byte_length_getter);
    JS_DECLARE_NATIVE_FUNCTION(byte_offset_getter);
    JS_DECLARE_NATIVE_FUNCTION(length_getter);

    JS_DECLARE_NATIVE_FUNCTION(at);
    JS_DECLARE_NATIVE_FUNCTION(copy_within);
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(every);
    JS_DECLARE_NATIVE_FUNCTION(fill);
    JS_DECLARE_NATIVE_FUNCTION(filter);
    JS_DECLARE_NATIVE_FUNCTION(find);
    JS_DECLARE_NATIVE_FUNCTION(find_index);
    JS_DECLARE_NATIVE_FUNCTION(find_last);
    JS_DECLARE_NATIVE_FUNCTION(find_last_index);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(includes);
    JS_DECLARE_NATIVE_FUNCTION(index_of);
    JS_DECLARE_NATIVE_FUNCTION(join);
    JS_DECLARE_NATIVE_FUNCTION(keys);
    JS_DECLARE_NATIVE_FUNCTION(last_index_of);
    JS_DECLARE_NATIVE_FUNCTION(map);
    JS_DECLARE_NATIVE_FUNCTION(reduce);
    JS_DECLARE_NATIVE_FUNCTION(reduce_right);
    JS_DECLARE_NATIVE_FUNCTION(reverse);
    JS_DECLARE_NATIVE_FUNCTION(set);
    JS_DECLARE_NATIVE_FUNCTION(slice);
    JS_DECLARE_NATIVE_FUNCTION(some);
    JS_DECLARE_NATIVE_FUNCTION(sort);
    JS_DECLARE_NATIVE_FUNCTION(subarray);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(to_reversed);
    JS_DECLARE_NATIVE_FUNCTION(to_sorted);
    JS_DECLARE_NATIVE_FUNCTION(with);
    JS_DECLARE_NATIVE_FUNCTION(values);
    JS_DECLARE_NATIVE_FUNCTION(to_string_tag_getter);
};

}
