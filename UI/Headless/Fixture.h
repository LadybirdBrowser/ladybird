/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Ladybird {

class Fixture {
public:
    virtual ~Fixture();

    virtual ErrorOr<void> setup() = 0;

    void teardown()
    {
        if (is_running())
            teardown_impl();
    }

    virtual StringView name() const = 0;
    virtual bool is_running() const { return false; }

    static void initialize_fixtures();
    static Optional<Fixture&> lookup(StringView name);
    static Vector<NonnullOwnPtr<Fixture>>& all();

protected:
    virtual void teardown_impl() = 0;
};

}
