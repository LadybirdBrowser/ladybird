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
            job.runSynchronously(on: self.asUnownedSerialExecutor())
        }
    }
}

public actor EventLoopActor {
    nonisolated private let executor: EventLoopExecutor

    nonisolated public var unownedExecutor: UnownedSerialExecutor {
        executor.asUnownedSerialExecutor()
    }

    public init() {
        executor = EventLoopExecutor()
    }

    public init(eventLoop: Core.EventLoop) {
        executor = EventLoopExecutor(eventLoop: eventLoop)
    }

    public func async(action: @escaping @Sendable () async -> Void) async {
        Task(executorPreference: self.executor) {
            await action()
        }
    }
}
