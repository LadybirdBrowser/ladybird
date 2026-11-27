// The shared data of functions, their environment shapes and templates, and the caches of for-in loops move in
// collections that move cells, along with the code that a flush leaves behind for the next compile.

function tag(strings) {
    return strings;
}

const holder = {
    makeAdder(base) {
        return value => base + value;
    },
    withTemplate() {
        return tag`template ${1} site`;
    },
    keysOf(object) {
        const keys = [];
        for (const key in object) keys.push(key);
        return keys;
    },
    withBlockBindings(value) {
        let doubled = value * 2;
        {
            let tripled = value * 3;
            const read = () => doubled + tripled;
            return read();
        }
    },
};

holder.counter = (function* () {
    let count = 0;
    while (true) yield ++count;
})();
holder.counter.next();
holder.thrower = function throwsFromHere() {
    throw new Error("thrown");
};
holder.callsThrower = function callsThrower() {
    holder.thrower();
};

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

const templateBefore = holder.withTemplate();
const adderBefore = holder.makeAdder(1);
const keyed = { first: 1, second: 2 };
holder.keysOf(keyed);
holder.withBlockBindings(1);

// The code of withTemplate() is flushed, so its template object waits in the flushed code for the next compile.
if (typeof jit !== "undefined") jit.neverCompile(holder.withTemplate);
flushCode();

// Built-in functions are held by the intrinsics of their realm and by the objects they are properties of.
const nativeAddresses = {
    max: addressOf(Math.max),
    push: addressOf(Array.prototype.push),
};

runAfterMovingCollection(() => {
    test("built-in functions move and keep working", () => {
        expect(addressOf(Math.max)).not.toBe(nativeAddresses.max);
        expect(addressOf(Array.prototype.push)).not.toBe(nativeAddresses.push);
        expect(Math.max(1, 3, 2)).toBe(3);
        expect([].push).toBe(Array.prototype.push);
        const array = [1];
        array.push(2);
        expect(array).toEqual([1, 2]);
        expect(Math.max.name).toBe("max");
    });

    test("functions whose shared data moved keep working", () => {
        expect(adderBefore(2)).toBe(3);
        expect(holder.makeAdder(10)(5)).toBe(15);
    });

    test("a recompile after a move takes the template objects of the flushed code", () => {
        expect(holder.withTemplate()).toBe(templateBefore);
    });

    test("for-in caches keep their keys when they move", () => {
        expect(holder.keysOf(keyed)).toEqual(["first", "second"]);
        expect(holder.keysOf({ first: 1, second: 2 })).toEqual(["first", "second"]);
    });

    const countsAfterMove = once(() => [holder.counter.next().value, holder.counter.next().value]);
    test("a generator resumes in its executable after the executable moved", () => {
        expect(countsAfterMove()).toEqual([2, 3]);
    });

    test("errors thrown from executables that moved know where they were thrown", () => {
        let error;
        try {
            holder.callsThrower();
        } catch (e) {
            error = e;
        }
        expect(error.stack.includes("at throwsFromHere (")).toBeTrue();
        expect(error.stack.includes("at callsThrower (")).toBeTrue();
        expect(/gc-moving-code\.js:\d+:\d+/.test(error.stack)).toBeTrue();
    });

    test("environments made after their shape moved work", () => {
        expect(holder.withBlockBindings(2)).toBe(10);
    });
});
