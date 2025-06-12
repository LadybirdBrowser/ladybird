/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <LibCore/MachPort.h>

namespace Core {

class IOSurfaceHandle {
    AK_MAKE_NONCOPYABLE(IOSurfaceHandle);

public:
    IOSurfaceHandle(IOSurfaceHandle&& other);
    IOSurfaceHandle& operator=(IOSurfaceHandle&& other);

    static IOSurfaceHandle create(int width, int height);
    static IOSurfaceHandle from_mach_port(MachPort const& port);

    MachPort create_mach_port() const;

    size_t width() const;
    size_t height() const;
    size_t bytes_per_element() const;
    size_t bytes_per_row() const;
    void* data() const;

    void* core_foundation_pointer() const;

    ~IOSurfaceHandle();

private:
    struct IOSurfaceRefWrapper;

    IOSurfaceHandle(OwnPtr<IOSurfaceRefWrapper>&&);

    OwnPtr<IOSurfaceRefWrapper> m_ref_wrapper;
};

}
