/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Array.h>

namespace JS {

class ArrayPrototype final : public Array {
    JS_OBJECT(ArrayPrototype, Array);
    JS_DECLARE_ALLOCATOR(ArrayPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ArrayPrototype() override = default;

private:
    explicit ArrayPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(at);
    JS_DECLARE_NATIVE_FUNCTION(concat);
    JS_DECLARE_NATIVE_FUNCTION(copy_within);
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(every);
    JS_DECLARE_NATIVE_FUNCTION(fill);
    JS_DECLARE_NATIVE_FUNCTION(filter);
    JS_DECLARE_NATIVE_FUNCTION(find);
    JS_DECLARE_NATIVE_FUNCTION(find_index);
    JS_DECLARE_NATIVE_FUNCTION(find_last);
    JS_DECLARE_NATIVE_FUNCTION(find_last_index);
    JS_DECLARE_NATIVE_FUNCTION(flat);
    JS_DECLARE_NATIVE_FUNCTION(flat_map);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(group);
    JS_DECLARE_NATIVE_FUNCTION(group_to_map);
    JS_DECLARE_NATIVE_FUNCTION(includes);
    JS_DECLARE_NATIVE_FUNCTION(index_of);
    JS_DECLARE_NATIVE_FUNCTION(join);
    JS_DECLARE_NATIVE_FUNCTION(keys);
    JS_DECLARE_NATIVE_FUNCTION(last_index_of);
    JS_DECLARE_NATIVE_FUNCTION(map);
    JS_DECLARE_NATIVE_FUNCTION(pop);
    JS_DECLARE_NATIVE_FUNCTION(push);
    JS_DECLARE_NATIVE_FUNCTION(reduce);
    JS_DECLARE_NATIVE_FUNCTION(reduce_right);
    JS_DECLARE_NATIVE_FUNCTION(reverse);
    JS_DECLARE_NATIVE_FUNCTION(shift);
    JS_DECLARE_NATIVE_FUNCTION(slice);
    JS_DECLARE_NATIVE_FUNCTION(some);
    JS_DECLARE_NATIVE_FUNCTION(sort);
    JS_DECLARE_NATIVE_FUNCTION(splice);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(to_reversed);
    JS_DECLARE_NATIVE_FUNCTION(to_sorted);
    JS_DECLARE_NATIVE_FUNCTION(to_spliced);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(unshift);
    JS_DECLARE_NATIVE_FUNCTION(values);
    JS_DECLARE_NATIVE_FUNCTION(with);
};

ThrowCompletionOr<void> array_merge_sort(VM&, Function<ThrowCompletionOr<double>(Value, Value)> const& compare_func, MarkedVector<Value>& arr_to_sort);

}
