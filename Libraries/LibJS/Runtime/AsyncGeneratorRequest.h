/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/PromiseCapability.h>

namespace JS {

// 27.6.3.1 AsyncGeneratorRequest Records, https://tc39.es/ecma262/#sec-asyncgeneratorrequest-records
struct AsyncGeneratorRequest {
    Completion completion;                      // [[Completion]]
    NonnullGCPtr<PromiseCapability> capability; // [[Capability]]
};

}
