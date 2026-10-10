// JIT code depends on things staying as they are, instead of checking them
// where it relies on them, and is invalidated when they change: frames
// running it exit where they next rely on them, and the code is discarded.
// These tests run every function many times, so that with a low JIT
// threshold they are compiled, and then change what the code depends on,
// also while it runs.

const iterations = 200;

let neverAssignedBefore = 1;

describe("Invalidation of JIT code", () => {
    test("global bindings assigned for the first time while the code runs", () => {
        const read = () => neverAssignedBefore;
        const assign = i => {
            if (i === iterations / 2) neverAssignedBefore = 2;
        };
        let sum = 0;
        for (let i = 0; i < iterations; ++i) {
            assign(i);
            sum += read();
        }
        expect(sum).toBe(iterations / 2 + 2 * (iterations / 2));
        for (let i = 0; i < iterations; ++i) expect(read()).toBe(2);
    });

    test("prototype chains that change between calls", () => {
        class Base {
            method() {
                return "base";
            }
        }
        class Derived extends Base {}
        const instance = new Derived();
        const call = object => object.method();
        for (let i = 0; i < iterations; ++i) expect(call(instance)).toBe("base");
        Derived.prototype.method = () => "derived";
        expect(call(instance)).toBe("derived");
        delete Derived.prototype.method;
        expect(call(instance)).toBe("base");
    });

    test("prototype chains that change while the code runs", () => {
        class Base {
            method() {
                return 1;
            }
        }
        class Derived extends Base {}
        const instance = new Derived();
        const change = i => {
            if (i === iterations / 2) Derived.prototype.method = () => 2;
        };
        const run = () => {
            let sum = 0;
            for (let i = 0; i < iterations; ++i) {
                change(i);
                sum += instance.method();
            }
            return sum;
        };
        for (let round = 0; round < 3; ++round) {
            delete Derived.prototype.method;
            expect(run()).toBe(iterations / 2 + 2 * (iterations / 2));
        }
    });

    test("prototypes that change in a getter the code calls", () => {
        const base = {
            value() {
                return "base";
            },
        };
        const middle = Object.create(base);
        const instance = Object.create(middle);
        let shadow = false;
        const holder = {
            get trigger() {
                if (shadow) middle.value = () => "middle";
                return 0;
            },
        };
        const read = () => holder.trigger + instance.value().length;
        for (let i = 0; i < iterations; ++i) expect(read()).toBe(4);
        shadow = true;
        expect(read()).toBe(6);
        expect(read()).toBe(6);
    });
});
