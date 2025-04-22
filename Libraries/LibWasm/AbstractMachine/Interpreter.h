/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWasm/AbstractMachine/Configuration.h>

namespace Wasm {

struct Interpreter {
    virtual ~Interpreter() = default;
    virtual void interpret(Configuration&) = 0;
    virtual Trap trap() const = 0;
    virtual bool did_trap() const = 0;
    virtual void clear_trap() = 0;
    virtual void visit_external_resources(HostVisitOps const&) { }
};

}
