/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace Web::Bindings {

class ImageConstructor final : public JS::NativeFunction {
    JS_OBJECT(ImageConstructor, JS::NativeFunction);
    JS_DECLARE_ALLOCATOR(ImageConstructor);

public:
    explicit ImageConstructor(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~ImageConstructor() override = default;

    virtual JS::ThrowCompletionOr<JS::Value> call() override;
    virtual JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> construct(JS::FunctionObject& new_target) override;

private:
    virtual bool has_constructor() const override { return true; }
};

}
