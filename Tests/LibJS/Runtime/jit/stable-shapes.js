// JIT code assumes that objects keep stable shapes (shapes no object has
// left yet) while it calls other code, and is invalidated when an object
// leaves one.
// These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("stable shapes in JIT code", () => {
    test("objects that gain properties during a call", () => {
        function make() {
            return { a: 1, b: 2 };
        }
        function sum(object, call) {
            const before = object.a;
            call(object);
            return before + object.a + object.b;
        }
        const leave = object => {
            object.c = 3;
        };
        const stay = () => {};
        for (let i = 0; i < iterations; ++i) expect(sum(make(), stay)).toBe(4);
        for (let i = 0; i < iterations; ++i) expect(sum(make(), leave)).toBe(4);
        for (let i = 0; i < iterations; ++i) expect(sum(make(), stay)).toBe(4);
    });

    test("objects whose properties are deleted during a call", () => {
        function make() {
            return { first: 1, second: 2, third: 3 };
        }
        function read(object, call) {
            const first = object.first;
            call(object);
            return [first, object.second, object.third];
        }
        const remove = object => {
            delete object.first;
        };
        const stay = () => {};
        for (let i = 0; i < iterations; ++i) expect(read(make(), stay)).toEqual([1, 2, 3]);
        for (let i = 0; i < iterations; ++i) expect(read(make(), remove)).toEqual([1, 2, 3]);
    });

    test("objects that become dictionaries during a call", () => {
        function make() {
            return { x: 10, y: 20 };
        }
        function read(object, call) {
            const x = object.x;
            call(object);
            return x + object.y;
        }
        const grow = object => {
            for (let i = 0; i < 100; ++i) object["key" + i] = i;
            delete object.key0;
            object.y = 5;
        };
        const stay = () => {};
        for (let i = 0; i < iterations; ++i) expect(read(make(), stay)).toBe(30);
        expect(read(make(), grow)).toBe(15);
        for (let i = 0; i < iterations; ++i) expect(read(make(), stay)).toBe(30);
    });

    test("objects whose prototype changes during a call", () => {
        const proto = {
            get value() {
                return "prototype";
            },
        };
        function make() {
            return { own: 1 };
        }
        function read(object, call) {
            const own = object.own;
            call(object);
            return own + ":" + object.own + ":" + object.value;
        }
        const reparent = object => {
            Object.setPrototypeOf(object, proto);
        };
        const stay = () => {};
        for (let i = 0; i < iterations; ++i) expect(read(make(), stay)).toBe("1:1:undefined");
        for (let i = 0; i < iterations; ++i) expect(read(make(), reparent)).toBe("1:1:prototype");
    });

    test("loops whose calls change objects late", () => {
        function make() {
            return { count: 0, step: 1 };
        }
        function run(object, call) {
            let total = 0;
            for (let i = 0; i < 200; ++i) {
                total += object.step;
                call(object, i);
            }
            return total + object.count;
        }
        const change = (object, i) => {
            if (i === 150) {
                object.extra = true;
                object.step = 2;
            }
        };
        const stay = () => {};
        for (let i = 0; i < 20; ++i) expect(run(make(), stay)).toBe(200);
        expect(run(make(), change)).toBe(151 + 49 * 2);
        for (let i = 0; i < 20; ++i) expect(run(make(), stay)).toBe(200);
    });
});
