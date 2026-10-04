describe("[[Call]] trap normal behavior", () => {
    test("forwarding when not defined in handler", () => {
        let p = new Proxy(() => 5, { apply: null });
        expect(p()).toBe(5);
        p = new Proxy(() => 5, { apply: undefined });
        expect(p()).toBe(5);
        p = new Proxy(() => 5, {});
        expect(p()).toBe(5);
    });

    test("correct arguments supplied to trap", () => {
        const f = (a, b) => a + b;
        const handler = {
            apply(target, this_, arguments_) {
                expect(target).toBe(f);
                expect(this_).toBeUndefined();
                if (arguments_[2]) {
                    return arguments_[0] * arguments_[1];
                }
                return f(...arguments_);
            },
        };
        let p = new Proxy(f, handler);

        expect(p(2, 4)).toBe(6);
        expect(p(2, 4, true)).toBe(8);
    });

    test("only the passed arguments are supplied to the trap", () => {
        const handler = {
            apply(target, this_, arguments_) {
                return arguments_;
            },
        };
        const p = new Proxy(function (a, b, c) {}, handler);

        expect(p()).toEqual([]);
        expect(p(1)).toEqual([1]);
        expect(p(1, 2, 3, 4)).toEqual([1, 2, 3, 4]);
        expect(p.call(null, 1)).toEqual([1]);
        expect(p.apply(null, [1])).toEqual([1]);
        expect(Reflect.apply(p, null, [1])).toEqual([1]);
        expect(p.bind(null, 1)(2)).toEqual([1, 2]);
        expect(new Proxy(p, {})(1)).toEqual([1]);
    });

    test("only the passed arguments are forwarded to the target", () => {
        function f(a, b, c) {
            return arguments.length;
        }
        const p = new Proxy(f, {});

        expect(p()).toBe(0);
        expect(p(1)).toBe(1);
        expect(p(1, 2, 3, 4)).toBe(4);
        expect(p.call(null, 1)).toBe(1);
        expect(p.apply(null, [1])).toBe(1);
        expect(Reflect.apply(p, null, [1])).toBe(1);
        expect(p.bind(null, 1)(2)).toBe(2);
        expect(new Proxy(p, {})(1)).toBe(1);
        expect(new Proxy(new Proxy(p, {}), {})(1)).toBe(1);
        expect(new Proxy(f.bind(null), {})(1)).toBe(1);
        expect(new Proxy(f.bind(null, 1), {})(2)).toBe(2);
    });

    test("rest parameters of the target only receive the passed arguments", () => {
        function f(a, b, ...rest) {
            return rest;
        }
        const p = new Proxy(f, {});

        expect(p(1)).toEqual([]);
        expect(p(1, 2)).toEqual([]);
        expect(p(1, 2, 3)).toEqual([3]);
        expect(new Proxy(p, {})(1)).toEqual([]);
        expect(new Proxy(f.bind(null, 1), {})(2)).toEqual([]);
    });
});

describe("[[Call]] invariants", () => {
    test("target must have a [[Call]] slot", () => {
        [{}, [], new Proxy({}, {})].forEach(item => {
            expect(() => {
                new Proxy(item, {})();
            }).toThrowWithMessage(TypeError, "[object ProxyObject] is not a function");
        });
    });
});
