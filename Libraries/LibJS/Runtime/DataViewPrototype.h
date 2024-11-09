/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class DataViewPrototype final : public PrototypeObject<DataViewPrototype, DataView> {
    JS_PROTOTYPE_OBJECT(DataViewPrototype, DataView, DataView);
    JS_DECLARE_ALLOCATOR(DataViewPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~DataViewPrototype() override = default;

private:
    explicit DataViewPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(get_big_int_64);
    JS_DECLARE_NATIVE_FUNCTION(get_big_uint_64);
    JS_DECLARE_NATIVE_FUNCTION(get_float_32);
    JS_DECLARE_NATIVE_FUNCTION(get_float_64);
    JS_DECLARE_NATIVE_FUNCTION(get_int_8);
    JS_DECLARE_NATIVE_FUNCTION(get_int_16);
    JS_DECLARE_NATIVE_FUNCTION(get_int_32);
    JS_DECLARE_NATIVE_FUNCTION(get_uint_8);
    JS_DECLARE_NATIVE_FUNCTION(get_uint_16);
    JS_DECLARE_NATIVE_FUNCTION(get_uint_32);
    JS_DECLARE_NATIVE_FUNCTION(set_big_int_64);
    JS_DECLARE_NATIVE_FUNCTION(set_big_uint_64);
    JS_DECLARE_NATIVE_FUNCTION(set_float_32);
    JS_DECLARE_NATIVE_FUNCTION(set_float_64);
    JS_DECLARE_NATIVE_FUNCTION(set_int_8);
    JS_DECLARE_NATIVE_FUNCTION(set_int_16);
    JS_DECLARE_NATIVE_FUNCTION(set_int_32);
    JS_DECLARE_NATIVE_FUNCTION(set_uint_8);
    JS_DECLARE_NATIVE_FUNCTION(set_uint_16);
    JS_DECLARE_NATIVE_FUNCTION(set_uint_32);

    JS_DECLARE_NATIVE_FUNCTION(buffer_getter);
    JS_DECLARE_NATIVE_FUNCTION(byte_length_getter);
    JS_DECLARE_NATIVE_FUNCTION(byte_offset_getter);
};

}
