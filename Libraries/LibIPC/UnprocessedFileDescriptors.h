/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/File.h>

namespace IPC {

class UnprocessedFileDescriptors {
public:
    void enqueue(File&& fd)
    {
        m_fds.append(move(fd));
    }

    File dequeue()
    {
        return m_fds.take_first();
    }

    void return_fds_to_front_of_queue(Vector<File>&& fds)
    {
        m_fds.prepend(move(fds));
    }

private:
    Vector<File> m_fds;
};

}
