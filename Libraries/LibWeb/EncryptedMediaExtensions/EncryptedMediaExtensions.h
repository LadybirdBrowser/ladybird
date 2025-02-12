/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::Bindings {

// https://w3c.github.io/encrypted-media/#dom-mediakeysrequirement
enum class MediaKeysRequirement {
    Required,
    Optional,
    NotAllowed
};

}
