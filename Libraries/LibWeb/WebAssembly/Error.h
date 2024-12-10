/*
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Error.h>

namespace Web::WebAssembly {

class CompileError final : public JS::Error {
    JS_OBJECT(CompileError, JS::Error);
    GC_DECLARE_ALLOCATOR(CompileError);

public:
    static GC::Ref<CompileError> create(JS::Realm&);
    static GC::Ref<CompileError> create(JS::Realm&, StringView message);
    virtual ~CompileError() override = default;

private:
    explicit CompileError(JS::Object& prototype);
};

class LinkError final : public JS::Error {
    JS_OBJECT(LinkError, JS::Error);
    GC_DECLARE_ALLOCATOR(LinkError);

public:
    static GC::Ref<LinkError> create(JS::Realm&);
    static GC::Ref<LinkError> create(JS::Realm&, StringView message);
    virtual ~LinkError() override = default;

private:
    explicit LinkError(JS::Object& prototype);
};

class RuntimeError final : public JS::Error {
    JS_OBJECT(RuntimeError, JS::Error);
    GC_DECLARE_ALLOCATOR(RuntimeError);

public:
    static GC::Ref<RuntimeError> create(JS::Realm&);
    static GC::Ref<RuntimeError> create(JS::Realm&, StringView message);
    virtual ~RuntimeError() override = default;

private:
    explicit RuntimeError(JS::Object& prototype);
};

} // namespace Web::WebAssembly
