/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Queue.h>
#include <LibWebView/FidoHIDDevice.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>

namespace WebView {

// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-20210615.html
// The FIDO Alliance usage page, and its usage for a CTAPHID authenticator device.
static constexpr int fido_usage_page = 0xf1d0;
static constexpr int fido_usage_ctaphid = 0x01;

// NB: A device that floods the host must not grow the queue without bound.
static constexpr size_t maximum_queued_reports = 256;

static CFArrayRef copy_connected_devices()
{
    auto manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager)
        return nullptr;

    auto usage_page = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fido_usage_page);
    auto usage = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fido_usage_ctaphid);
    void const* keys[] = { CFSTR(kIOHIDDeviceUsagePageKey), CFSTR(kIOHIDDeviceUsageKey) };
    void const* values[] = { usage_page, usage };
    auto matching = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);
    CFRelease(usage);
    CFRelease(usage_page);

    CFArrayRef devices = nullptr;
    if (auto device_set = IOHIDManagerCopyDevices(manager)) {
        auto count = CFSetGetCount(device_set);
        Vector<void const*> device_refs;
        device_refs.resize(count);
        CFSetGetValues(device_set, device_refs.data());
        // NB: The array retains the devices, so they outlive the manager.
        devices = CFArrayCreate(kCFAllocatorDefault, device_refs.data(), count, &kCFTypeArrayCallBacks);
        CFRelease(device_set);
    }
    CFRelease(manager);
    return devices;
}

static Optional<u64> registry_entry_id_of(IOHIDDeviceRef device)
{
    auto service = IOHIDDeviceGetService(device);
    if (service == MACH_PORT_NULL)
        return {};
    uint64_t entry_id = 0;
    if (IORegistryEntryGetRegistryEntryID(service, &entry_id) != KERN_SUCCESS)
        return {};
    return entry_id;
}

class FidoHIDDeviceMacOS final : public FidoHIDDevice {
public:
    static ErrorOr<NonnullOwnPtr<FidoHIDDeviceMacOS>> open(IOHIDDeviceRef device, u64 id)
    {
        if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess)
            return Error::from_string_literal("Could not open the HID device");
        return adopt_own(*new FidoHIDDeviceMacOS(device, id));
    }

    virtual ~FidoHIDDeviceMacOS() override
    {
        IOHIDDeviceRegisterInputReportCallback(m_device, m_input_buffer, sizeof(m_input_buffer), nullptr, this);
        IOHIDDeviceUnscheduleFromRunLoop(m_device, m_run_loop, kCFRunLoopDefaultMode);
        IOHIDDeviceClose(m_device, kIOHIDOptionsTypeNone);
        CFRelease(m_run_loop);
        CFRelease(m_device);
    }

    virtual u64 id() const override { return m_id; }

    virtual ErrorOr<void> write_report(ReadonlyBytes report) override
    {
        VERIFY(report.size() == report_size);
        // NB: A FIDO device has no report IDs, so the report goes out as it is, with the ID 0.
        if (IOHIDDeviceSetReport(m_device, kIOHIDReportTypeOutput, 0, report.data(), report.size()) != kIOReturnSuccess)
            return Error::from_string_literal("Could not send a report to the HID device");
        return {};
    }

    virtual ErrorOr<Optional<Report>> read_report(u32 timeout_in_milliseconds) override
    {
        if (m_reports.is_empty())
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, static_cast<CFTimeInterval>(timeout_in_milliseconds) / 1000.0, true);
        if (m_reports.is_empty()) {
            if (m_was_removed)
                return Error::from_string_literal("The HID device was removed");
            return OptionalNone {};
        }
        return m_reports.dequeue();
    }

private:
    FidoHIDDeviceMacOS(IOHIDDeviceRef device, u64 id)
        : m_device(device)
        , m_run_loop(CFRunLoopGetCurrent())
        , m_id(id)
    {
        CFRetain(m_device);
        CFRetain(m_run_loop);
        IOHIDDeviceRegisterInputReportCallback(m_device, m_input_buffer, sizeof(m_input_buffer), did_receive_report, this);
        IOHIDDeviceRegisterRemovalCallback(m_device, did_remove, this);
        IOHIDDeviceScheduleWithRunLoop(m_device, m_run_loop, kCFRunLoopDefaultMode);
    }

    static void did_receive_report(void* context, IOReturn result, void*, IOHIDReportType, uint32_t, uint8_t* report, CFIndex report_length)
    {
        auto& self = *static_cast<FidoHIDDeviceMacOS*>(context);
        if (result != kIOReturnSuccess || report_length < 0 || static_cast<size_t>(report_length) != report_size)
            return;
        if (self.m_reports.size() >= maximum_queued_reports)
            return;
        Report copy;
        ReadonlyBytes { report, report_size }.copy_to(copy.span());
        self.m_reports.enqueue(copy);
    }

    static void did_remove(void* context, IOReturn, void*)
    {
        static_cast<FidoHIDDeviceMacOS*>(context)->m_was_removed = true;
    }

    IOHIDDeviceRef m_device { nullptr };
    CFRunLoopRef m_run_loop { nullptr };
    u64 m_id { 0 };
    u8 m_input_buffer[report_size] {};
    Queue<Report> m_reports;
    bool m_was_removed { false };
};

Vector<NonnullOwnPtr<FidoHIDDevice>> FidoHIDDevice::open_new_devices(HashTable<u64> const& known_device_ids)
{
    Vector<NonnullOwnPtr<FidoHIDDevice>> opened;
    auto devices = copy_connected_devices();
    if (!devices)
        return opened;
    for (CFIndex i = 0; i < CFArrayGetCount(devices); ++i) {
        auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(CFArrayGetValueAtIndex(devices, i)));
        auto id = registry_entry_id_of(device);
        if (!id.has_value() || known_device_ids.contains(*id))
            continue;
        if (auto result = FidoHIDDeviceMacOS::open(device, *id); !result.is_error())
            opened.append(result.release_value());
    }
    CFRelease(devices);
    return opened;
}

bool FidoHIDDevice::is_any_connected()
{
    auto devices = copy_connected_devices();
    if (!devices)
        return false;
    bool is_any_connected = CFArrayGetCount(devices) > 0;
    CFRelease(devices);
    return is_any_connected;
}

}
