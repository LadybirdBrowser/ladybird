/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Test262 {

class IsHTMLDDA final : public NativeFunction {
    JS_OBJECT(IsHTMLDDA, NativeFunction);
    JS_DECLARE_ALLOCATOR(IsHTMLDDA);

public:
    virtual ~IsHTMLDDA() override = default;

    virtual ThrowCompletionOr<Value> call() override;

private:
    explicit IsHTMLDDA(Realm&);

    virtual bool is_htmldda() const override { return true; }
};

}
