/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>

namespace Sync {

struct PolicyNonRecursive { };
struct PolicyRecursive { };
struct PolicyIntraprocess { };
struct PolicyInterprocess { };

namespace Detail {

template<typename T>
concept IsIntraprocess = requires {
    requires IsSame<typename T::InterprocessPolicyType, PolicyIntraprocess>;
};

template<typename T>
concept IsNonRecursive = requires {
    requires IsSame<typename T::RecursivePolicyType, PolicyNonRecursive>;
};

}
}
