/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ladybird/Headless/Fixture.h>

namespace Ladybird {

// Key function for Fixture
Fixture::~Fixture() = default;

Optional<Fixture&> Fixture::lookup(StringView name)
{
    for (auto& fixture : all()) {
        if (fixture->name() == name)
            return *fixture;
    }
    return {};
}

Vector<NonnullOwnPtr<Fixture>>& Fixture::all()
{
    static Vector<NonnullOwnPtr<Fixture>> fixtures;
    return fixtures;
}

void Fixture::initialize_fixtures()
{
}

}
