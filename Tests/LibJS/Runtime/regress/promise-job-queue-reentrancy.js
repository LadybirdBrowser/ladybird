test("nested promise job checkpoints wait for the current reaction to finish", () => {
    const order = [];
    Promise.resolve()
        .then(() => {
            order.push("reaction start");
            Promise.resolve().then(() => order.push("queued reaction"));
            runQueuedPromiseJobs();
            order.push("reaction end");
        })
        .then(() => order.push("chained reaction"));

    runQueuedPromiseJobs();
    expect(order).toEqual(["reaction start", "reaction end", "queued reaction", "chained reaction"]);
});

test("native awaits resume disposal before draining later promise jobs", () => {
    for (const reject of [false, true]) {
        const order = [];
        Promise.resolve().then(() => {
            const stack = new AsyncDisposableStack();
            stack.defer(() => {
                order.push("second dispose");
            });
            stack.defer(() => {
                order.push("first dispose");
                Promise.resolve()
                    .then(() => order.push("tick 1"))
                    .then(() => order.push("tick 2"))
                    .then(() => order.push("tick 3"));
                return reject ? Promise.reject("disposal failed") : Promise.resolve();
            });
            stack.disposeAsync().then(
                () => order.push("fulfilled"),
                () => order.push("rejected")
            );
            order.push("dispose returned");
        });

        runQueuedPromiseJobs();
        expect(order).toEqual([
            "first dispose",
            "tick 1",
            "second dispose",
            "tick 2",
            "dispose returned",
            "tick 3",
            reject ? "rejected" : "fulfilled",
        ]);
    }
});
