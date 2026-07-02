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

    test("trap receives only passed arguments", () => {
        let receivedArguments;
        const p = new Proxy(function (_formalParameter) {}, {
            apply(_target, _thisValue, arguments_) {
                receivedArguments = arguments_;
            },
        });

        p();
        expect(receivedArguments.length).toBe(0);

        p(1, 2);
        expect(receivedArguments.length).toBe(2);
        expect(receivedArguments[0]).toBe(1);
        expect(receivedArguments[1]).toBe(2);
    });

    test("forwarding uses only passed arguments", () => {
        const p = new Proxy(function (_formalParameter) {
            return arguments.length;
        }, {});

        expect(p()).toBe(0);
        expect(p(1, 2)).toBe(2);
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
