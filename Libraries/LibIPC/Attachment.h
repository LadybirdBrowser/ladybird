/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>

namespace IPC {

class Attachment {
    AK_MAKE_NONCOPYABLE(Attachment);

public:
    Attachment() = default;
    Attachment(Attachment&&);
    Attachment& operator=(Attachment&&);
    ~Attachment();

    static Attachment from_fd(int fd);
    int to_fd();

private:
    int m_fd { -1 };
};

}
