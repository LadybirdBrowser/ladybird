/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/WebAssembly/Builtins.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly::Detail {

using Wasm::Runtime::HeapKind;
using Wasm::Runtime::HostError;
using Wasm::Runtime::ValueKind;
using WasmValue = Wasm::Runtime::Value;
using WasmValueType = Wasm::Runtime::ValueType;

static constexpr auto js_string_builtin_set_name = "js-string"sv;
static constexpr auto js_string_builtin_set_qualified_name = "wasm:js-string"sv;

// The funcTypes of the builtins, each in a recursion group of its own, as the types of a module that has nothing else.
// The store gives types the same identity wherever they are equivalent, so an import matches the funcType of a builtin
// exactly when the identity of its type is the identity of that funcType.
enum BuiltinTypeIndex : u32 {
    // (rec (type (array (mut i16)))).0
    ArrayType,
    // (rec (type (func (param externref) (result (ref extern))))).0
    ExternrefToRefExtern,
    // (rec (type (func (param externref) (result i32)))).0
    ExternrefToI32,
    // (rec (type (func (param (ref null arrayType) i32 i32) (result (ref extern))))).0
    ArrayI32I32ToRefExtern,
    // (rec (type (func (param externref (ref null arrayType) i32) (result i32)))).0
    ExternrefArrayI32ToI32,
    // (rec (type (func (param i32) (result (ref extern))))).0
    I32ToRefExtern,
    // (rec (type (func (param externref i32) (result i32)))).0
    ExternrefI32ToI32,
    // (rec (type (func (param externref externref) (result (ref extern))))).0
    ExternrefExternrefToRefExtern,
    // (rec (type (func (param externref i32 i32) (result (ref extern))))).0
    ExternrefI32I32ToRefExtern,
    // (rec (type (func (param externref externref) (result i32)))).0
    ExternrefExternrefToI32,
};

static constexpr Array<u8, 75> builtin_types_module_bytes {
    // Magic and version.
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    // The type section: 65 bytes, 10 types.
    0x01, 0x41, 0x0a,
    // array (mut i16)
    0x5e, 0x77, 0x01,
    // func (param externref) (result (ref extern))
    0x60, 0x01, 0x6f, 0x01, 0x64, 0x6f,
    // func (param externref) (result i32)
    0x60, 0x01, 0x6f, 0x01, 0x7f,
    // func (param (ref null 0) i32 i32) (result (ref extern))
    0x60, 0x03, 0x63, 0x00, 0x7f, 0x7f, 0x01, 0x64, 0x6f,
    // func (param externref (ref null 0) i32) (result i32)
    0x60, 0x03, 0x6f, 0x63, 0x00, 0x7f, 0x01, 0x7f,
    // func (param i32) (result (ref extern))
    0x60, 0x01, 0x7f, 0x01, 0x64, 0x6f,
    // func (param externref i32) (result i32)
    0x60, 0x02, 0x6f, 0x7f, 0x01, 0x7f,
    // func (param externref externref) (result (ref extern))
    0x60, 0x02, 0x6f, 0x6f, 0x01, 0x64, 0x6f,
    // func (param externref i32 i32) (result (ref extern))
    0x60, 0x03, 0x6f, 0x7f, 0x7f, 0x01, 0x64, 0x6f,
    // func (param externref externref) (result i32)
    0x60, 0x02, 0x6f, 0x6f, 0x01, 0x7f
};

static Wasm::Runtime::Module const& builtin_types_module()
{
    static NeverDestroyed<NonnullRefPtr<Wasm::Runtime::Module>> module { MUST(Wasm::Runtime::Module::compile(builtin_types_module_bytes)) };
    return *module.get();
}

// The store-wide identity of the type at `type_index` of a module.
static u32 type_identity(Wasm::Runtime::Module const& module, u32 type_index)
{
    WasmValueType reference_type { ValueKind::Reference, HeapKind::Concrete, false, type_index };
    return WebAssemblyCache::the().store().canonical_value_type(module, reference_type).type_index;
}

using BuiltinSteps = ErrorOr<void, HostError> (*)(JS::VM&, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results);

struct Builtin {
    StringView name;
    BuiltinTypeIndex function_type;
    BuiltinSteps steps;
};

static constexpr auto externref = WasmValueType::reference(HeapKind::Extern);

static HostError trap(StringView message)
{
    return HostError { ByteString { message } };
}

static JS::Value js_value_of(JS::VM& vm, WasmValue value)
{
    return MUST(to_js_value(vm, value, externref));
}

static WasmValue string_reference(JS::VM& vm, Utf16View const& string)
{
    return MUST(to_webassembly_value(vm, JS::PrimitiveString::create(vm, string), externref));
}

static WasmValue string_reference(JS::VM& vm, Utf16String const& string)
{
    return MUST(to_webassembly_value(vm, JS::PrimitiveString::create(vm, string), externref));
}

static WasmValue string_reference(JS::VM& vm, Vector<char16_t> const& code_units)
{
    if (code_units.is_empty())
        return string_reference(vm, Utf16View {});
    return string_reference(vm, Utf16View { code_units.data(), code_units.size() });
}

static u32 unsigned_i32(WasmValue value)
{
    return static_cast<u32>(value.to_i32());
}

// https://webassembly.github.io/spec/js-api/#abstract-opdef-unwrapstring
static ErrorOr<GC::Ref<JS::PrimitiveString>, HostError> unwrap_string(JS::VM& vm, WasmValue v)
{
    auto value = js_value_of(vm, v);

    // 1. If v is not a String,
    if (!value.is_string()) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("Expected a string"sv);
    }

    // 2. Return v
    return value.as_string();
}

// https://webassembly.github.io/spec/js-api/#abstract-opdef-fromcharcode
static Utf16String from_char_code(i32 v)
{
    // 1. Assert: v is of type i32.
    // 2. Return ! Call(String.fromCharCode, undefined, « ToJSValue(v) »).
    char16_t code_unit = static_cast<u16>(v);
    return Utf16String::from_utf16({ &code_unit, 1 });
}

// https://webassembly.github.io/spec/js-api/#abstract-opdef-charcodeat
static i32 char_code_at(Utf16View const& string, u32 index)
{
    // 1. Assert: index is of type i32.
    // 2. Return ! Call(String.prototype.charCodeAt, string, « ToJSValue(index) »).
    return string.code_unit_at(index);
}

namespace JSStringBuiltins {

// https://webassembly.github.io/spec/js-api/#js-string-cast
static ErrorOr<void, HostError> cast(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    // 1. Return ? UnwrapString(v)
    TRY(unwrap_string(vm, arguments[0]));
    results[0] = arguments[0];
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-test
static ErrorOr<void, HostError> test(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    // 1. If v is not a String,
    if (!js_value_of(vm, arguments[0]).is_string()) {
        // 1. Return 0.
        results[0] = WasmValue::from_i32(0);
        return {};
    }

    // 2. Return 1.
    results[0] = WasmValue::from_i32(1);
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-fromcharcodearray
static ErrorOr<void, HostError> from_char_code_array(JS::VM& vm, Wasm::Runtime::Store& store, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    auto array = arguments[0];
    // NB: The indices are unsigned, as everywhere else in WebAssembly.
    auto start = unsigned_i32(arguments[1]);
    auto end = unsigned_i32(arguments[2]);

    // 1. If array is null,
    if (array.is_null_reference()) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("The array is null"sv);
    }

    // 2. Let length be the number of elements in array.
    auto length = store.array_length(array.reference_bits());

    // 3. If start > end or end > length,
    if (start > end || end > length) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("The range is out of the array's bounds"sv);
    }

    // 4. Let result be the empty string.
    Vector<char16_t> result;
    result.ensure_capacity(end - start);

    // 5. Let i be start.
    // 6. While i < end:
    for (auto i = start; i < end; ++i) {
        // 1. Let charCode be the value of the element stored at index i in array.
        auto char_code = static_cast<i32>(store.array_element(array.reference_bits(), i));

        // 2. Let charCodeString be FromCharCode(charCode).
        // 3. Let result be the concatenation of result and charCodeString.
        // NB: FromCharCode() makes a string of one code unit, which is the code unit itself here.
        result.unchecked_append(static_cast<u16>(char_code));

        // 4. Set i to i + 1.
    }

    // 7. Return result.
    results[0] = string_reference(vm, result);
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-intocharcodearray
static ErrorOr<void, HostError> into_char_code_array(JS::VM& vm, Wasm::Runtime::Store& store, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    auto array = arguments[1];
    auto start = unsigned_i32(arguments[2]);

    // 1. If array is null,
    if (array.is_null_reference()) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("The array is null"sv);
    }

    // 2. Let string be ? UnwrapString(string).
    auto string = TRY(unwrap_string(vm, arguments[0]))->utf16_string_view();

    // 3. Let stringLength be the length of string.
    auto string_length = string.length_in_code_units();

    // 4. Let arrayLength be the number of elements in array.
    auto array_length = store.array_length(array.reference_bits());

    // 5. If start + stringLength > arrayLength,
    if (static_cast<u64>(start) + string_length > array_length) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("The string does not fit in the array"sv);
    }

    // 6. Let i be 0.
    // 7. While i < stringLength:
    for (u32 i = 0; i < string_length; ++i) {
        // 1. Let charCode be CharCodeAt(string, i).
        auto char_code = Detail::char_code_at(string, i);

        // 2. Set the element at index start + i in array to ToWebAssemblyValue(charCode, i32).
        store.set_array_element(array.reference_bits(), start + i, static_cast<u32>(char_code));

        // 3. Set i to i + 1.
    }

    // 8. Return stringLength.
    results[0] = WasmValue::from_i32(static_cast<i32>(string_length));
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-fromcharcode
static ErrorOr<void, HostError> from_char_code(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    // 1. Return FromCharCode(v).
    results[0] = string_reference(vm, Detail::from_char_code(arguments[0].to_i32()));
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-fromcodepoint
static ErrorOr<void, HostError> from_code_point(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    auto v = unsigned_i32(arguments[0]);

    // 1. If v > 0x10ffff,
    if (v > 0x10ffff) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("The code point is out of range"sv);
    }

    // 2. Return ! Call(String.fromCodePoint, undefined, « ToJSValue(v) »).
    results[0] = string_reference(vm, Utf16String::from_code_point(v));
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-charcodeat
static ErrorOr<void, HostError> char_code_at(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    auto index = unsigned_i32(arguments[1]);

    // 1. Let string be ? UnwrapString(string).
    auto string = TRY(unwrap_string(vm, arguments[0]))->utf16_string_view();

    // 2. Let length be the length of string.
    auto length = string.length_in_code_units();

    // 3. If index >= length,
    if (index >= length) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("The index is out of the string's bounds"sv);
    }

    // 4. Return CharCodeAt(string, index).
    results[0] = WasmValue::from_i32(Detail::char_code_at(string, index));
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-codepointat
static ErrorOr<void, HostError> code_point_at(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    auto index = unsigned_i32(arguments[1]);

    // 1. Let string be ? UnwrapString(string).
    auto string = TRY(unwrap_string(vm, arguments[0]))->utf16_string_view();

    // 2. Let length be the length of string.
    auto length = string.length_in_code_units();

    // 3. If index >= length,
    if (index >= length) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("The index is out of the string's bounds"sv);
    }

    // 4. Return ! Call(String.prototype.codePointAt, string, « ToJSValue(index) »).
    results[0] = WasmValue::from_i32(static_cast<i32>(string.code_point_at(index)));
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-length
static ErrorOr<void, HostError> length(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    // 1. Let string be ? UnwrapString(v).
    auto string = TRY(unwrap_string(vm, arguments[0]));

    // 2. Return the length of string.
    results[0] = WasmValue::from_i32(static_cast<i32>(string->length_in_utf16_code_units()));
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-concat
static ErrorOr<void, HostError> concat(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    // 1. Let first be ? UnwrapString(first).
    auto first = TRY(unwrap_string(vm, arguments[0]));

    // 2. Let second be ? UnwrapString(second).
    auto second = TRY(unwrap_string(vm, arguments[1]));

    // 3. Return ! Call(String.prototype.concat, first, « second »).
    auto first_view = first->utf16_string_view();
    auto second_view = second->utf16_string_view();
    Vector<char16_t> result;
    result.ensure_capacity(first_view.length_in_code_units() + second_view.length_in_code_units());
    for (size_t index = 0; index < first_view.length_in_code_units(); ++index)
        result.unchecked_append(first_view.code_unit_at(index));
    for (size_t index = 0; index < second_view.length_in_code_units(); ++index)
        result.unchecked_append(second_view.code_unit_at(index));
    results[0] = string_reference(vm, result);
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-substring
static ErrorOr<void, HostError> substring(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    // NB: The indices are unsigned, as everywhere else in WebAssembly.
    auto start = unsigned_i32(arguments[1]);
    auto end = unsigned_i32(arguments[2]);

    // 1. Let string be ? UnwrapString(string).
    auto string = TRY(unwrap_string(vm, arguments[0]))->utf16_string_view();

    // 2. Let length be the length of string.
    auto length = string.length_in_code_units();

    // 3. If start > end or start > length,
    if (start > end || start > length) {
        // 1. Return the empty string.
        results[0] = string_reference(vm, Utf16View {});
        return {};
    }

    // 4. Return ! Call(String.prototype.substring, string, « ToJSValue(start), ToJSValue(end) »).
    // NB: start <= end here, and String.prototype.substring clamps end to the length.
    auto clamped_end = min<size_t>(end, length);
    results[0] = string_reference(vm, string.substring_view(start, clamped_end - start));
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-equals
static ErrorOr<void, HostError> equals(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    auto first = js_value_of(vm, arguments[0]);
    auto second = js_value_of(vm, arguments[1]);

    // 1. If first is not null and first is not a String,
    if (!first.is_null() && !first.is_string()) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("Expected a string or null"sv);
    }

    // 2. If second is not null and second is not a String,
    if (!second.is_null() && !second.is_string()) {
        // 1. Throw a RuntimeError exception as if a trap was executed.
        return trap("Expected a string or null"sv);
    }

    // 3. If IsStrictlyEqual(first, second) is true,
    bool strictly_equal = first.is_null() || second.is_null()
        ? first.is_null() && second.is_null()
        : first.as_string().utf16_string_view() == second.as_string().utf16_string_view();
    if (strictly_equal) {
        // 1. Return 1.
        results[0] = WasmValue::from_i32(1);
        return {};
    }

    // 4. Return 0.
    results[0] = WasmValue::from_i32(0);
    return {};
}

// https://webassembly.github.io/spec/js-api/#js-string-compare
static ErrorOr<void, HostError> compare(JS::VM& vm, Wasm::Runtime::Store&, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results)
{
    // 1. Let first be ? UnwrapString(first).
    auto first = TRY(unwrap_string(vm, arguments[0]))->utf16_string_view();

    // 2. Let second be ? UnwrapString(second).
    auto second = TRY(unwrap_string(vm, arguments[1]))->utf16_string_view();

    // 3. If IsStrictlyEqual(first, second) is true,
    if (first == second) {
        // 1. Return 0.
        results[0] = WasmValue::from_i32(0);
        return {};
    }

    // 4. If ! IsLessThan(first, second, true) is true,
    // NB: IsLessThan() compares two strings by their code units.
    if ((first <=> second) < 0) {
        // 1. Return -1.
        results[0] = WasmValue::from_i32(-1);
        return {};
    }

    // 5. Return 1.
    results[0] = WasmValue::from_i32(1);
    return {};
}

}

// https://webassembly.github.io/spec/js-api/#string-builtins
static constexpr Array js_string_builtins {
    Builtin { "cast"sv, ExternrefToRefExtern, JSStringBuiltins::cast },
    Builtin { "test"sv, ExternrefToI32, JSStringBuiltins::test },
    Builtin { "fromCharCodeArray"sv, ArrayI32I32ToRefExtern, JSStringBuiltins::from_char_code_array },
    Builtin { "intoCharCodeArray"sv, ExternrefArrayI32ToI32, JSStringBuiltins::into_char_code_array },
    Builtin { "fromCharCode"sv, I32ToRefExtern, JSStringBuiltins::from_char_code },
    // INTEROP: The funcType of fromCodePoint has a (ref extern) result in other implementations, where the spec has an
    //          externref result.
    Builtin { "fromCodePoint"sv, I32ToRefExtern, JSStringBuiltins::from_code_point },
    Builtin { "charCodeAt"sv, ExternrefI32ToI32, JSStringBuiltins::char_code_at },
    Builtin { "codePointAt"sv, ExternrefI32ToI32, JSStringBuiltins::code_point_at },
    Builtin { "length"sv, ExternrefToI32, JSStringBuiltins::length },
    Builtin { "concat"sv, ExternrefExternrefToRefExtern, JSStringBuiltins::concat },
    Builtin { "substring"sv, ExternrefI32I32ToRefExtern, JSStringBuiltins::substring },
    Builtin { "equals"sv, ExternrefExternrefToI32, JSStringBuiltins::equals },
    Builtin { "compare"sv, ExternrefExternrefToI32, JSStringBuiltins::compare },
};

bool refers_to_a_builtin_set(Utf16String const& builtin_set_name)
{
    return builtin_set_name == js_string_builtin_set_name;
}

// https://webassembly.github.io/spec/js-api/#get-the-builtins-for-a-builtin-set
static ReadonlySpan<Builtin> get_the_builtins_for_a_builtin_set(Utf16String const& builtin_set_name)
{
    // 1. Return a list of (name, funcType, steps) for the set with name builtinSetName defined within this section.
    VERIFY(refers_to_a_builtin_set(builtin_set_name));
    return js_string_builtins.span();
}

// https://webassembly.github.io/spec/js-api/#validate-builtin-set-names
static bool validate_builtin_set_names(ReadonlySpan<Utf16String> builtin_set_names)
{
    // 1. If builtinSetNames contains any duplicates, return false.
    HashTable<Utf16String> seen_names;
    for (auto const& builtin_set_name : builtin_set_names) {
        if (seen_names.set(builtin_set_name) != HashSetResult::InsertedNewEntry)
            return false;
    }

    // 2. Return true.
    return true;
}

// https://webassembly.github.io/spec/js-api/#find-a-builtin
static Builtin const* find_a_builtin(Wasm::Runtime::Import const& import, ReadonlySpan<Utf16String> builtin_set_names)
{
    // 1. Assert: validate builtin set names builtinSetNames is true.
    VERIFY(validate_builtin_set_names(builtin_set_names));

    // 2. Let importModuleName be import[0].
    auto import_module_name = import.module;

    // 3. Let importName be import[1].
    auto import_name = import.name;

    // 4. For each builtinSetName of builtinSetNames,
    for (auto const& builtin_set_name : builtin_set_names) {
        // 1. If builtinSetName does not refer to a builtin set, then continue.
        if (!refers_to_a_builtin_set(builtin_set_name))
            continue;

        // 2. Let builtinSetQualifiedName be builtinSetName prefixed with "wasm:".
        auto builtin_set_qualified_name = js_string_builtin_set_qualified_name;

        // 3. If importModuleName equals builtinSetQualifiedName,
        if (import_module_name == builtin_set_qualified_name) {
            // 1. Let builtins be the result of get the builtins for a builtin set builtinSetName.
            auto builtins = get_the_builtins_for_a_builtin_set(builtin_set_name);

            // 2. For each builtin of builtins,
            for (auto const& builtin : builtins) {
                // 1. Let builtinName be builtin[0].
                // 2. If importName equals builtinName, return (builtinSetName, builtin).
                if (import_name == builtin.name)
                    return &builtin;
            }
        }
    }

    // 5. Return null.
    return nullptr;
}

bool import_refers_to_a_builtin(Wasm::Runtime::Import const& import, ReadonlySpan<Utf16String> builtin_set_names)
{
    return find_a_builtin(import, builtin_set_names) != nullptr;
}

// https://webassembly.github.io/spec/js-api/#validate-an-import-for-builtins
static bool validate_an_import_for_builtins(Wasm::Runtime::Module const& module, Wasm::Runtime::Import const& import, ReadonlySpan<Utf16String> builtin_set_names)
{
    // 1. Assert: validate builtin set names builtinSetNames is true.
    VERIFY(validate_builtin_set_names(builtin_set_names));

    // 2. Let maybeBuiltin be the result of finding a builtin for import and builtinSetNames.
    auto const* maybe_builtin = find_a_builtin(import, builtin_set_names);

    // 3. If maybeBuiltin is null, return true.
    if (!maybe_builtin)
        return true;

    // 4. Let importExternType be import[2].
    auto const& import_extern_type = import.type;

    // 5. Let builtinFuncType be maybeBuiltin[1][1].
    auto builtin_func_type = maybe_builtin->function_type;

    // 6. Let builtinExternType be func |builtinFuncType|.
    // 7. Return match_externtype(builtinExternType, importExternType).
    // NB: The funcType of a builtin is final and has no supertypes, so it only matches the import's type if both are
    //     the same type.
    if (import_extern_type.kind != Wasm::Runtime::ExternKind::Function)
        return false;
    return type_identity(module, import_extern_type.type_index) == type_identity(builtin_types_module(), builtin_func_type);
}

// https://webassembly.github.io/spec/js-api/#validate-builtins-and-imported-string-for-a-webassembly-module
bool validate_builtins_and_imported_strings(Wasm::Runtime::Module const& module, ReadonlySpan<Utf16String> builtin_set_names, Optional<Utf16String> const& imported_string_module)
{
    // 1. If validating builtin set names for builtinSetNames is false, return false.
    if (!validate_builtin_set_names(builtin_set_names))
        return false;

    Optional<String> imported_string_module_utf8;
    if (imported_string_module.has_value())
        imported_string_module_utf8 = imported_string_module->to_utf8();

    // 2. For each import of module_imports(module),
    for (size_t import_index = 0; import_index < module.import_count(); ++import_index) {
        auto import = module.import(import_index);

        // 1. If importedStringModule is not null and import[0] equals importedStringModule,
        if (imported_string_module_utf8.has_value() && import.module == imported_string_module_utf8->bytes_as_string_view()) {
            // 1. Let importExternType be import[2].
            auto const& import_extern_type = import.type;

            // 2. Let stringExternType be global ref extern.
            // 3. If match_externtype(stringExternType, importExternType) is false, return false
            // NB: An immutable global matches if (ref extern) is a subtype of the import's value type, which then is
            //     (ref extern) or externref.
            if (import_extern_type.kind != Wasm::Runtime::ExternKind::Global
                || import_extern_type.is_mutable
                || import_extern_type.value_type.kind != ValueKind::Reference
                || import_extern_type.value_type.heap != HeapKind::Extern) {
                return false;
            }
        }
        // 2. Else,
        else {
            // 1. If validating an import for builtins with import and builtinSetNames is false, return false.
            if (!validate_an_import_for_builtins(module, import, builtin_set_names))
                return false;
        }
    }

    // 3. Return true.
    return true;
}

// https://webassembly.github.io/spec/js-api/#create-a-builtin-function
static Wasm::Runtime::Entity create_a_builtin_function(JS::VM& vm, BuiltinTypeIndex function_type, BuiltinSteps steps, u32 index)
{
    // 1. Let hostfunc be a host function which executes steps when called.
    auto host_function = [&vm, steps](Wasm::Runtime::Store& store, ReadonlySpan<WasmValue> arguments, Span<WasmValue> results) -> ErrorOr<void, HostError> {
        return steps(vm, store, arguments, results);
    };

    // 2. Let store be the surrounding agent's associated store.
    // 3. Let (store, funcaddr) be func_alloc(store, funcType, hostfunc).
    // 4. Set the surrounding agent's associated store to store.
    // 5. Return funcaddr.
    // NB: The index is the builtin's position in its set, which names the Exported Function made for it.
    auto type = function_type_in_store(builtin_types_module(), function_type);
    return WebAssemblyCache::the().store().allocate_host_function(type, move(host_function), index);
}

// https://webassembly.github.io/spec/js-api/#instantiate-a-builtin-set
GC::Ref<JS::Object> instantiate_a_builtin_set(JS::VM& vm, Utf16String const& builtin_set_name)
{
    auto& realm = *vm.current_realm();

    // 1. Let builtins be the result of get the builtins for a builtin set builtinSetName.
    auto builtins = get_the_builtins_for_a_builtin_set(builtin_set_name);

    // 2. Let exportsObject be OrdinaryObjectCreate(null).
    auto exports_object = JS::Object::create(realm, nullptr);

    // 3. For each (name, funcType, steps) of builtins,
    for (size_t index = 0; index < builtins.size(); ++index) {
        auto const& builtin = builtins[index];

        // 1. Let funcaddr be the result of create a builtin function with funcType and steps.
        auto funcaddr = create_a_builtin_function(vm, builtin.function_type, builtin.steps, static_cast<u32>(index));

        // 2. Let func be the result of creating a new Exported Function from funcaddr.
        auto func = create_native_function(realm, funcaddr);

        // 3. Let value be func.
        JS::Value value = func;

        // 4. Let status be ! CreateDataProperty(exportsObject, name, value).
        auto status = MUST(exports_object->create_data_property(Utf16FlyString::from_utf8(builtin.name), value));

        // 5. Assert: status is true.
        VERIFY(status);
    }

    // 4. Return exportsObject.
    return exports_object;
}

// https://webassembly.github.io/spec/js-api/#instantiate-imported-strings
GC::Ref<JS::Object> instantiate_imported_strings(JS::VM& vm, Wasm::Runtime::Module const& module, Utf16String const& imported_string_module)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: importedStringModule is not null.
    auto imported_string_module_utf8 = imported_string_module.to_utf8();

    // 2. Let exportsObject be OrdinaryObjectCreate(null).
    auto exports_object = JS::Object::create(realm, nullptr);

    // 3. For each (moduleName, componentName, externtype) of module_imports(module),
    for (size_t import_index = 0; import_index < module.import_count(); ++import_index) {
        auto import = module.import(import_index);

        // 1. If moduleName does not equal importedStringModule, then continue.
        if (import.module != imported_string_module_utf8.bytes_as_string_view())
            continue;

        // 2. Let stringConstant be componentName.
        auto string_constant = Utf16String::from_utf8(import.name);

        // 3. Let status be ! CreateDataProperty(exportsObject, stringConstant, stringConstant).
        auto status = MUST(exports_object->create_data_property(Utf16FlyString::from_utf16(string_constant.utf16_view()), JS::PrimitiveString::create(vm, string_constant)));

        // 4. Assert: status is true.
        VERIFY(status);
    }

    // 4. Return exportsObject.
    return exports_object;
}

}
