test("running async wait jobs from a promise reaction is rejected", () => {
    const view = new Int32Array(new SharedArrayBuffer(4));
    const result = Atomics.waitAsync(view, 0, 0);
    const order = [];
    Promise.resolve().then(() => {
        expect(() => runAsyncWaitJobs()).toThrow(Error);
        order.push("reaction");
    });
    Promise.resolve().then(() => {
        order.push("notify");
        Atomics.notify(view, 0);
    });
    result.value.then(value => order.push(value));
    runAsyncWaitJobs();
    expect(order).toEqual(["reaction", "notify", "ok"]);
});
