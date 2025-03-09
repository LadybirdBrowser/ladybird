/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Core
import Testing

@Suite(.serialized)
struct TestEventLoopActor {
    init() {
        Core.install_thread_local_event_loop()
    }

    @Test
    func testEventLoopActor() async {
        // Creates an executor around EventLoop::current()
        let actor = EventLoopActor()

        let ev = Core.EventLoop.current()
        print("Event loop at \(Unmanaged.passUnretained(ev).toOpaque())")

        let (stream, continuation) = AsyncStream<Int>.makeStream()
        var iterator = stream.makeAsyncIterator()

        Task {
            await actor.async {
                #expect(ev == Core.EventLoop.current(), "Closure is executed on event loop")
                print("Hello from event loop at \(Unmanaged.passUnretained(Core.EventLoop.current()).toOpaque())")

                continuation.yield(42)
            }
        }

        Task {
            await actor.async {
                #expect(ev == Core.EventLoop.current(), "Closure is executed on event loop")
                Core.EventLoop.current().quit(4)

                continuation.yield(1234)
                continuation.finish()
            }
        }

        #expect(ev == Core.EventLoop.current(), "Event loop exists until end of function")

        let rc = ev.exec()
        #expect(rc == 4)
        // Values not available until event loop has processed tasks
        #expect(await iterator.next() == 42)
        #expect(await iterator.next() == 1234)
    }
}
