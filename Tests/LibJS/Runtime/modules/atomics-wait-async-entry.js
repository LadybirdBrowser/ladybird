test("entry module can suspend on an async wait", () => {
    const evaluation = evaluateModule("./modules/atomics-wait-async-entry.mjs");
    expect(evaluation).toBeInstanceOf(Promise);
    expect(globalThis.asyncWaitModuleResult).toBeUndefined();
    gc();
    runAsyncWaitJobs();
    expect(globalThis.asyncWaitModuleResult).toBe("timed-out");
});

test("entry module evaluation does not block a promise reaction", () => {
    globalThis.asyncWaitModuleResult = undefined;
    const order = [];
    Promise.resolve().then(() => {
        evaluateModule("./modules/atomics-wait-async-entry.mjs").then(() => {
            order.push(globalThis.asyncWaitModuleResult);
        });
        order.push("evaluation returned");
    });
    Promise.resolve().then(() => order.push("next reaction"));
    runAsyncWaitJobs();
    expect(order).toEqual(["evaluation returned", "next reaction", "timed-out"]);
});

test("entry module rejection after an async wait is observable", () => {
    let message;
    evaluateModule("./modules/atomics-wait-async-entry-reject.mjs").catch(error => {
        message = error.message;
    });
    runAsyncWaitJobs();
    expect(message).toBe("module failed after waiting");
});
