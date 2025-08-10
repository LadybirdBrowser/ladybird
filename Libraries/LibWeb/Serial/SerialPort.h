/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <serial_cpp/serial.h>

#include <LibWeb/Bindings/SerialPortPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Serial {

// https://wicg.github.io/serial/#serialoptions-dictionary
struct SerialOptions {
    Optional<WebIDL::UnsignedLong> baud_rate = {};
    Optional<WebIDL::Octet> data_bits = 8;
    Optional<WebIDL::Octet> stop_bits = 1;
    Optional<Bindings::ParityType> parity = Bindings::ParityType::None;
    Optional<WebIDL::UnsignedLong> buffer_size = 255;
    Optional<Bindings::FlowControlType> flow_control = Bindings::FlowControlType::None;
};

// https://wicg.github.io/serial/#serialoutputsignals-dictionary
struct SerialOutputSignals {
    Optional<WebIDL::Boolean> data_terminal_ready;
    Optional<WebIDL::Boolean> request_to_send;
    Optional<WebIDL::Boolean> break_;
};

// https://wicg.github.io/serial/#serialinputsignals-dictionary
struct SerialInputSignals {
    WebIDL::Boolean data_carrier_detect;
    WebIDL::Boolean clear_to_send;
    WebIDL::Boolean ring_indicator;
    WebIDL::Boolean data_set_ready;
};

// https://wicg.github.io/serial/#serialportinfo-dictionary
struct SerialPortInfo {
    Optional<WebIDL::UnsignedShort> usb_vendor_id;
    Optional<WebIDL::UnsignedShort> usb_product_id;
    Optional<String> bluetooth_service_class_id;
};

enum SerialPortState : u8 {
    Closed,
    Opening,
    Opened,
    Closing,
    Forgetting,
    Forgotten,
};

// https://wicg.github.io/serial/#serialport-interface
class SerialPort : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(SerialPort, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SerialPort);

    serial_cpp::PortInfo device() { return m_device; }

    // https://wicg.github.io/serial/#getinfo-method
    SerialPortInfo get_info() const;
    // https://wicg.github.io/serial/#open-method
    GC::Ref<WebIDL::Promise> open(SerialOptions);
    // https://wicg.github.io/serial/#setsignals-method
    GC::Ref<WebIDL::Promise> set_signals(SerialOutputSignals = {});
    // https://wicg.github.io/serial/#getsignals-method
    GC::Ref<WebIDL::Promise> get_signals() const;
    // https://wicg.github.io/serial/#close-method
    GC::Ref<WebIDL::Promise> close();
    // https://wicg.github.io/serial/#forget-method
    GC::Ref<WebIDL::Promise> forget();

    // https://wicg.github.io/serial/#connected-attribute
    bool connected() const { return m_connected; }
    // https://wicg.github.io/serial/#readable-attribute
    GC::Ref<Streams::ReadableStream> readable() { return *m_readable; }
    // https://wicg.github.io/serial/#writable-attribute
    GC::Ref<Streams::WritableStream> writable() { return *m_writable; }

    // https://wicg.github.io/serial/#onconnect-attribute-0
    void set_onconnect(WebIDL::CallbackType*);
    WebIDL::CallbackType* onconnect();

    // https://wicg.github.io/serial/#ondisconnect-attribute-0
    void set_ondisconnect(WebIDL::CallbackType*);
    WebIDL::CallbackType* ondisconnect();

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    SerialPort(JS::Realm&, serial_cpp::PortInfo m_device);

    virtual void initialize(JS::Realm&) override;

    serial_cpp::PortInfo m_device;

    // https://wicg.github.io/serial/#dfn-state
    // Tracks the active state of the SerialPort
    SerialPortState m_state { SerialPortState::Closed };

    // https://wicg.github.io/serial/#dfn-buffersize
    // The amount of data to buffer for transmit and receive
    unsigned long m_buffer_size = {};

    // https://wicg.github.io/serial/#dfn-connected
    // A flag indicating the logical connection state of serial port
    bool m_connected { false };

    // https://wicg.github.io/serial/#dfn-readable
    // A ReadableStream that receives data from the port
    GC::Ptr<Streams::ReadableStream> m_readable = {};

    // https://wicg.github.io/serial/#dfn-readfatal
    // A flag indicating that the port has encountered a fatal read error
    bool m_read_fatal { false };

    // https://wicg.github.io/serial/#dfn-writable
    // A WritableStream that transmits data to the port
    GC::Ptr<Streams::WritableStream> m_writable = {};

    // https://wicg.github.io/serial/#dfn-writefatal
    // A flag indicating that the port has encountered a fatal write error
    bool m_write_fatal { false };

    // https://wicg.github.io/serial/#dfn-pendingclosepromise
    // A Promise used to wait for readable and writable to close
    GC::Ptr<WebIDL::Promise> m_pending_close_promise = {};
};

}
