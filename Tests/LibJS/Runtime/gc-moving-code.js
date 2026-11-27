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

const templateBefore = holder.withTemplate();
const adderBefore = holder.makeAdder(1);
const keyed = { first: 1, second: 2 };
holder.keysOf(keyed);
holder.withBlockBindings(1);

// The code of withTemplate() is flushed, so its template object waits in the flushed code for the next compile.
if (typeof jit !== "undefined") jit.neverCompile(holder.withTemplate);
flushCode();

runAfterMovingCollection(() => {
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

    test("environments made after their shape moved work", () => {
        expect(holder.withBlockBindings(2)).toBe(10);
    });
});
