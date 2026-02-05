/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibCore/IOSurface.h>

#if !defined(AK_OS_MACOS)
static_assert(false, "This file must only be used for macOS");
#endif

#import <IOSurface/IOSurface.h>

namespace Core {

template<typename T>
class RefAutoRelease {
    AK_MAKE_NONCOPYABLE(RefAutoRelease);

public:
    RefAutoRelease(T ref)
        : m_ref(ref)
    {
    }

    ~RefAutoRelease()
    {
        if (m_ref)
            CFRelease(m_ref);
        m_ref = nullptr;
    }

    T ref() const { return m_ref; }

private:
    T m_ref { nullptr };
};

struct IOSurfaceHandle::IOSurfaceRefWrapper {
    IOSurfaceRef ref;
};

IOSurfaceHandle::IOSurfaceHandle(OwnPtr<IOSurfaceRefWrapper>&& ref_wrapper)
    : m_ref_wrapper(move(ref_wrapper))
{
}

IOSurfaceHandle::IOSurfaceHandle(IOSurfaceHandle&& other) = default;
IOSurfaceHandle& IOSurfaceHandle::operator=(IOSurfaceHandle&& other) = default;

IOSurfaceHandle::~IOSurfaceHandle()
{
    if (m_ref_wrapper)
        CFRelease(m_ref_wrapper->ref);
}

IOSurfaceHandle IOSurfaceHandle::create(int width, int height)
{
    size_t bytes_per_element = 4;
    uint32_t pixel_format = 'BGRA';

    RefAutoRelease<CFNumberRef> width_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
    RefAutoRelease<CFNumberRef> height_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
    RefAutoRelease<CFNumberRef> bytes_per_element_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bytes_per_element);
    RefAutoRelease<CFNumberRef> pixel_format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(props, kIOSurfaceWidth, width_number.ref());
    CFDictionarySetValue(props, kIOSurfaceHeight, height_number.ref());
    CFDictionarySetValue(props, kIOSurfaceBytesPerElement, bytes_per_element_number.ref());
    CFDictionarySetValue(props, kIOSurfacePixelFormat, pixel_format_number.ref());

    auto* ref = IOSurfaceCreate(props);
    VERIFY(ref);
    return IOSurfaceHandle(make<IOSurfaceRefWrapper>(ref));
}

MachPort IOSurfaceHandle::create_mach_port() const
{
    auto port = IOSurfaceCreateMachPort(m_ref_wrapper->ref);
    return MachPort::adopt_right(port, MachPort::PortRight::Send);
}

IOSurfaceHandle IOSurfaceHandle::from_mach_port(MachPort const& port)
{
    // NOTE: This call does not destroy the port
    auto* ref = IOSurfaceLookupFromMachPort(port.port());
    VERIFY(ref);
    return IOSurfaceHandle(make<IOSurfaceRefWrapper>(ref));
}

size_t IOSurfaceHandle::width() const
{
    return IOSurfaceGetWidth(m_ref_wrapper->ref);
}

size_t IOSurfaceHandle::height() const
{
    return IOSurfaceGetHeight(m_ref_wrapper->ref);
}

size_t IOSurfaceHandle::bytes_per_element() const
{
    return IOSurfaceGetBytesPerElement(m_ref_wrapper->ref);
}

size_t IOSurfaceHandle::bytes_per_row() const
{
    return IOSurfaceGetBytesPerRow(m_ref_wrapper->ref);
}

void* IOSurfaceHandle::data() const
{
    return IOSurfaceGetBaseAddress(m_ref_wrapper->ref);
}

void* IOSurfaceHandle::core_foundation_pointer() const
{
    return m_ref_wrapper->ref;
}

}
