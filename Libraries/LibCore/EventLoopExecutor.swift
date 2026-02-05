/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
@_exported import CoreCxx

extension Core.EventLoop: Equatable {
    func deferred_invoke(_ task: @escaping () -> Void) {
        Core.deferred_invoke_block(self, task)
    }

    public static func == (lhs: Core.EventLoop, rhs: Core.EventLoop) -> Bool {
        Unmanaged.passUnretained(lhs).toOpaque() == Unmanaged.passUnretained(rhs).toOpaque()
    }
}

public class EventLoopExecutor: SerialExecutor, TaskExecutor, @unchecked Sendable {
    nonisolated private let eventLoop: Core.EventLoop

    public init() {
        eventLoop = Core.EventLoop.current()
    }

    public init(eventLoop: Core.EventLoop) {
        self.eventLoop = eventLoop
    }

    public nonisolated func enqueue(_ job: consuming ExecutorJob) {
        let job = UnownedJob(job)
        eventLoop.deferred_invoke { [self, job] in
            job.runSynchronously(
                isolatedTo: self.asUnownedSerialExecutor(),
                taskExecutor: self.asUnownedTaskExecutor())
        }
    }

    public func checkIsolated() {
        precondition(Core.EventLoop.current() == eventLoop)
    }
}

public protocol EventLoopActor: Actor {
    nonisolated var executor: EventLoopExecutor { get }  // impl with a let
}

extension EventLoopActor {
    public nonisolated var unownedExecutor: UnownedSerialExecutor {
        executor.asUnownedSerialExecutor()
    }
}
