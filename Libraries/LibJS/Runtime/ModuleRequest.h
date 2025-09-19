/*
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Module.h>

namespace JS {

// https://tc39.es/ecma262/#importattribute-record
struct ImportAttribute {
    Utf16String key;
    Utf16String value;

    bool operator==(ImportAttribute const&) const = default;
};

// https://tc39.es/ecma262/#loadedmodulerequest-record
struct LoadedModuleRequest {
    Utf16String specifier;              // [[Specifier]]
    Vector<ImportAttribute> attributes; // [[Attributes]]
    GC::Ref<Module> module;             // [[Module]]
};

// https://tc39.es/ecma262/#modulerequest-record
struct JS_API ModuleRequest {
    ModuleRequest() = default;

    explicit ModuleRequest(Utf16FlyString specifier)
        : module_specifier(move(specifier))
    {
    }

    ModuleRequest(Utf16FlyString specifier, Vector<ImportAttribute> attributes);

    void add_attribute(Utf16String key, Utf16String value)
    {
        attributes.empend(move(key), move(value));
    }

    Utf16FlyString module_specifier;    // [[Specifier]]
    Vector<ImportAttribute> attributes; // [[Attributes]]

    bool operator==(ModuleRequest const&) const = default;
};

inline auto const& specifier_of(ModuleRequest const& r) { return r.module_specifier; }
inline auto const& specifier_of(LoadedModuleRequest const& r) { return r.specifier; }

template<typename T>
concept ModuleRequestLike = IsOneOf<RemoveCVReference<T>, ModuleRequest, LoadedModuleRequest>;

// 16.2.1.3.1 ModuleRequestsEqual ( left, right ), https://tc39.es/ecma262/#sec-modulerequestsequal
template<ModuleRequestLike L, ModuleRequestLike R>
bool module_requests_equal(L const& left, R const& right)
{
    // 1. If left.[[Specifier]] is not right.[[Specifier]], return false.
    if (specifier_of(left) != specifier_of(right))
        return false;

    // 2. Let leftAttrs be left.[[Attributes]].
    // 3. Let rightAttrs be right.[[Attributes]].
    auto const& left_attrs = left.attributes;
    auto const& right_attrs = right.attributes;

    // 4. Let leftAttrsCount be the number of elements in leftAttrs.
    // 5. Let rightAttrsCount be the number of elements in rightAttrs.
    // 6. If leftAttrsCount ≠ rightAttrsCount, return false.
    if (left_attrs.size() != right_attrs.size())
        return false;

    // 7. For each ImportAttribute Record l of leftAttrs
    //    a. If rightAttrs does not contain an ImportAttribute Record r such that l.[[Key]] is r.[[Key]] and l.[[Value]] is r.[[Value]], return false.
    // 8. Return true.
    return AK::all_of(left_attrs, [&right_attrs](auto const& l) {
        return AK::any_of(right_attrs, [&l](auto const& r) {
            return l.key == r.key && l.value == r.value;
        });
    });
}

}
