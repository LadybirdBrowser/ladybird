// The cells below are only reachable through the properties of `holder`, which the collector can rewrite, so a
// collection that moves cells moves them.
const holder = {};
const settled = [];

holder.pending = new Promise((resolve, reject) => {
    holder.resolve = resolve;
    holder.reject = reject;
});
holder.pending.then(
    value => settled.push(["fulfilled", value]),
    reason => settled.push(["rejected", reason])
);

holder.all = Promise.all([
    new Promise(resolve => (holder.resolveFirst = resolve)),
    new Promise(resolve => (holder.resolveSecond = resolve)),
]);
holder.all.then(values => settled.push(["all", values]));

holder.revocable = Proxy.revocable({ value: 1 }, {});

// The prototype chain of `reader` is watched by the cache of `readMethod()`, so changing the prototype afterwards fires
// a watchpoint set that moved.
holder.prototype = { method: () => "before" };
const reader = Object.create(Object.create(holder.prototype));
function readMethod(object) {
    return object.method();
}
for (let i = 0; i < 10; ++i) readMethod(reader);

// Bindings that eval adds give an environment rare data.
holder.withEvalBindings = (() => {
    eval("var added = 'added by eval'");
    return {
        read: () => added,
        remove: () => delete added,
    };
})();

// NB: test-js runs each test more than once with LIBJS_TEST_REPEAT, but the cells moved once, so what a test cannot undo
//     happens once, and every run checks what it saw.
function once(steps) {
    let result;
    let ran = false;
    return () => {
        if (!ran) {
            result = steps();
            ran = true;
        }
        return result;
    };
}

const before = {
    resolve: addressOf(holder.resolve),
    reject: addressOf(holder.reject),
    revoke: addressOf(holder.revocable.revoke),
};

runAfterMovingCollection(() => {
    test("resolving functions move, and still settle their promise once", () => {
        expect(addressOf(holder.resolve)).not.toBe(before.resolve);
        expect(addressOf(holder.reject)).not.toBe(before.reject);
        holder.resolve("value");
        holder.reject("reason");
        runQueuedPromiseJobs();
        expect(settled).toEqual([["fulfilled", "value"]]);
    });

    const allSettled = once(() => {
        settled.length = 0;
        holder.resolveSecond(2);
        holder.resolveFirst(1);
        runQueuedPromiseJobs();
        return [...settled];
    });
    test("element functions of Promise.all keep their state when they move", () => {
        expect(allSettled()).toEqual([["all", [1, 2]]]);
    });

    const valueBeforeRevoke = once(() => {
        const value = holder.revocable.proxy.value;
        holder.revocable.revoke();
        return value;
    });
    test("native functions that capture state move, and keep it", () => {
        expect(addressOf(holder.revocable.revoke)).not.toBe(before.revoke);
        expect(valueBeforeRevoke()).toBe(1);
        expect(() => holder.revocable.proxy.value).toThrow(TypeError);
    });

    const methodBeforeChange = once(() => {
        const method = readMethod(reader);
        holder.prototype.method = () => "after";
        return method;
    });
    test("a watchpoint set that moved still fires", () => {
        expect(methodBeforeChange()).toBe("before");
        expect(readMethod(reader)).toBe("after");
    });

    const evalBindingBeforeRemoval = once(() => [holder.withEvalBindings.read(), holder.withEvalBindings.remove()]);
    test("environments keep the bindings that eval added when their rare data moves", () => {
        expect(evalBindingBeforeRemoval()).toEqual(["added by eval", true]);
        expect(() => holder.withEvalBindings.read()).toThrow(ReferenceError);
    });
});
