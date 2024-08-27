/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// FIXME: This file is intended to become redundant as swift-testing stabilizes
//        See https://github.com/swiftlang/swift-testing/blob/133e30231c4583b02ab3ea2a7f678f3d7f4f8a3d/Documentation/CMake.md#add-an-entry-point

import Testing

@main struct Runner {
    static func main() async {
        await Testing.__swiftPMEntryPoint() as Never
    }
}
