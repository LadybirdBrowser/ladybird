/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <LibCore/Export.h>
#include <LibCore/MachPort.h>

namespace Core {

class CORE_API IOSurfaceHandle {
    AK_MAKE_NONCOPYABLE(IOSurfaceHandle);

public:
    IOSurfaceHandle(IOSurfaceHandle&& other);
    IOSurfaceHandle& operator=(IOSurfaceHandle&& other);

    static IOSurfaceHandle create(int width, int height);
    static ErrorOr<IOSurfaceHandle> from_mach_port(MachPort const& port);

    // Adopts a surface owned by someone else, such as one a decoder hands back, by retaining it.
    static IOSurfaceHandle from_ref(void* io_surface_ref);

    // Whoever allocated the surface may recycle it as soon as its use count falls to zero. Retaining it does not
    // make it used.
    void increment_use_count();
    void decrement_use_count();

    MachPort create_mach_port() const;

    u32 id() const;

    size_t width() const;
    size_t height() const;

    // The four character code the surface was allocated with, such as '420v' for biplanar 8-bit YCbCr.
    u32 pixel_format() const;
    size_t plane_count() const;
    size_t plane_width(size_t plane) const;
    size_t plane_height(size_t plane) const;
    size_t bytes_per_element() const;
    size_t bytes_per_row() const;
    void* data() const;

    size_t bytes_per_row_of_plane(size_t plane) const;
    void* data_of_plane(size_t plane) const;

    // Reading a surface's data with the CPU requires holding this lock for the duration of the read.
    bool lock_read_only() const;
    void unlock_read_only() const;

    void* core_foundation_pointer() const;

    ~IOSurfaceHandle();

private:
    struct IOSurfaceRefWrapper;

    IOSurfaceHandle(OwnPtr<IOSurfaceRefWrapper>&&);

    OwnPtr<IOSurfaceRefWrapper> m_ref_wrapper;
};

}
