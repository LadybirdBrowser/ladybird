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

// The list of values of Promise.all holds the values that came in so far.
holder.allWithObject = Promise.all([
    new Promise(resolve => (holder.resolveWithObject = resolve)),
    new Promise(resolve => (holder.resolveLast = resolve)),
]);
holder.allWithObject.then(values => settled.push(["all with object", values]));
holder.resolveWithObject({ came: "first" });
runQueuedPromiseJobs();

// Converting objects of one shape to primitives fills the special property cache of the shape.
holder.convertible = { toString: () => "converted" };
for (let i = 0; i < 4; ++i) String(holder.convertible);

// The prototype chain of `reader` is watched by the cache of `readMethod()`, so changing the prototype afterwards fires
// a watchpoint set that moved.
holder.prototype = { method: () => "before" };
const reader = Object.create(Object.create(holder.prototype));
function readMethod(object) {
    return object.method();
}
for (let i = 0; i < 10; ++i) readMethod(reader);

// A suspended generator keeps the values of its frame in memory of its own.
holder.generator = (function* () {
    const local = { kept: "by the frame" };
    holder.generatorLocalAddress = addressOf(local);
    yield 1;
    yield local;
})();
holder.generator.next();

// So does an async function that waits.
holder.asyncResult = (async () => {
    const local = { kept: "across await" };
    holder.asyncLocalAddress = addressOf(local);
    await new Promise(resolve => (holder.resumeAsync = resolve));
    return local;
})();

// The fields of a class are kept by its constructor.
holder.Class = class {
    field = "initial " + "value";
    ["computed" + "Field"] = 2;
};

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

    const allWithObjectSettled = once(() => {
        settled.length = 0;
        holder.resolveLast(2);
        runQueuedPromiseJobs();
        return [...settled];
    });
    test("the values of Promise.all move, and end up in its result", () => {
        expect(allWithObjectSettled()).toEqual([["all with object", [{ came: "first" }, 2]]]);
    });

    const conversionBeforeChange = once(() => {
        const converted = String(holder.convertible);
        holder.convertible.toString = () => "changed";
        return converted;
    });
    test("special property caches keep what they know when it moves", () => {
        expect(conversionBeforeChange()).toBe("converted");
        expect(String(holder.convertible)).toBe("changed");
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

    const generatorLocal = once(() => holder.generator.next().value);
    test("the values in the frame of a suspended generator move", () => {
        const value = generatorLocal();
        expect(addressOf(value)).not.toBe(holder.generatorLocalAddress);
        expect(value).toEqual({ kept: "by the frame" });
    });

    test("the values in the frame of a waiting async function move", () => {
        settled.length = 0;
        holder.asyncResult.then(value => settled.push(["async", value, addressOf(value)]));
        holder.resumeAsync();
        runQueuedPromiseJobs();
        expect(settled.length).toBe(1);
        expect(settled[0][1]).toEqual({ kept: "across await" });
        expect(settled[0][2]).not.toBe(holder.asyncLocalAddress);
    });

    test("class constructors keep their fields when they move", () => {
        const instance = new holder.Class();
        expect(instance.field).toBe("initial value");
        expect(instance.computedField).toBe(2);
    });

    const evalBindingBeforeRemoval = once(() => [holder.withEvalBindings.read(), holder.withEvalBindings.remove()]);
    test("environments keep the bindings that eval added when their rare data moves", () => {
        expect(evalBindingBeforeRemoval()).toEqual(["added by eval", true]);
        expect(() => holder.withEvalBindings.read()).toThrow(ReferenceError);
    });
});
