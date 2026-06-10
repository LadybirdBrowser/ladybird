/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SerialPort.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Serial {

using SerialInputSignals = Bindings::SerialInputSignals;
using SerialOptions = Bindings::SerialOptions;
using SerialOutputSignals = Bindings::SerialOutputSignals;
using SerialPortInfo = Bindings::SerialPortInfo;

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
    WEB_WRAPPABLE(SerialPort, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SerialPort);

public:
    // https://wicg.github.io/serial/#getinfo-method
    SerialPortInfo get_info() const;
    // https://wicg.github.io/serial/#open-method
    void open(JS::Realm&, SerialOptions const&, GC::Ref<WebIDL::Promise>);
    // https://wicg.github.io/serial/#setsignals-method
    void set_signals(JS::Realm&, SerialOutputSignals const&, GC::Ref<WebIDL::Promise>);
    // https://wicg.github.io/serial/#getsignals-method
    void get_signals(JS::Realm&, GC::Ref<WebIDL::Promise>) const;
    // https://wicg.github.io/serial/#close-method
    void close(JS::Realm&, GC::Ref<WebIDL::Promise>);
    // https://wicg.github.io/serial/#forget-method
    void forget(JS::Realm&, GC::Ref<WebIDL::Promise>);

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
    explicit SerialPort();

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
