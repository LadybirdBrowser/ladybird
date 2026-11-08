/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Crypto/CryptoAlgorithms.h>

namespace Web::Crypto {

struct NormalizedAlgorithmAndParameter {
    AK_ALLOC_WITH_KMALLOC;

    NonnullOwnPtr<AlgorithmMethods> methods;
    NonnullOwnPtr<AlgorithmParams> parameter;
};

}
