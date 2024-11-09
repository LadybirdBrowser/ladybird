/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace Web::Bindings {

class AudioConstructor final : public JS::NativeFunction {
    JS_OBJECT(AudioConstructor, JS::NativeFunction);
    JS_DECLARE_ALLOCATOR(AudioConstructor);

public:
    explicit AudioConstructor(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~AudioConstructor() override = default;

    virtual JS::ThrowCompletionOr<JS::Value> call() override;
    virtual JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> construct(JS::FunctionObject& new_target) override;

private:
    virtual bool has_constructor() const override { return true; }
};

}
