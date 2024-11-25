/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>

namespace Core {

class Gamepad : public RefCounted<Gamepad> {
public:
    static ErrorOr<NonnullRefPtr<Gamepad>> create(StringView path);

    virtual ~Gamepad() = default;

    virtual String const& path() const = 0;
    virtual String const& name() const = 0;

    virtual ErrorOr<Vector<double>> get_axes() = 0;
    virtual ErrorOr<Vector<bool>> get_buttons() = 0;

    virtual ErrorOr<bool> poll_all_events() = 0;

protected:
    Gamepad() = default;
};

}
