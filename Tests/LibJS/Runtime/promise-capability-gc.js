test("resolving functions from a custom constructor survive garbage collection", () => {
    const settlements = [];

    function CustomConstructor(executor) {
        executor(
            value => settlements.push(`resolved ${value}`),
            reason => settlements.push(`rejected ${reason}`)
        );
    }

    collectGarbageOnEveryAllocation(() => {
        Promise.resolve.call(CustomConstructor, 1);
        Promise.reject.call(CustomConstructor, 2);
    });

    expect(settlements).toEqual(["resolved 1", "rejected 2"]);
});
