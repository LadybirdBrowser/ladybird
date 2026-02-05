/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>

namespace IPC {

class AutoCloseFileDescriptor : public RefCounted<AutoCloseFileDescriptor> {
public:
    explicit AutoCloseFileDescriptor(int fd);
    ~AutoCloseFileDescriptor();

    int value() const { return m_fd; }
    int take_fd() { return exchange(m_fd, -1); }

private:
    int m_fd { -1 };
};

}
