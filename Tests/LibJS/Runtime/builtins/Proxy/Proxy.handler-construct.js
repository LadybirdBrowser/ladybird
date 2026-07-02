describe("[[Construct]] trap normal behavior", () => {
    test("forwarding when not defined in handler", () => {
        let p = new Proxy(
            function () {
                this.x = 5;
            },
            { construct: null }
        );
        expect(new p().x).toBe(5);
        p = new Proxy(
            function () {
                this.x = 5;
            },
            { construct: undefined }
        );
        expect(new p().x).toBe(5);
        p = new Proxy(function () {
            this.x = 5;
        }, {});
        expect(new p().x).toBe(5);
    });

    test("trapping 'new'", () => {
        function f(value) {
            this.x = value;
        }

        let p;
        const handler = {
            construct(target, arguments_, newTarget) {
                expect(target).toBe(f);
                expect(newTarget).toBe(p);
                if (arguments_[1]) {
                    return Reflect.construct(target, [arguments_[0] * 2], newTarget);
                }
                return Reflect.construct(target, arguments_, newTarget);
            },
        };
        p = new Proxy(f, handler);

        expect(new p(15).x).toBe(15);
        expect(new p(15, true).x).toBe(30);
    });

    test("trapping Reflect.construct", () => {
        function f(value) {
            this.x = value;
        }

        let p;
        function theNewTarget() {}
        const handler = {
            construct(target, arguments_, newTarget) {
                expect(target).toBe(f);
                expect(newTarget).toBe(theNewTarget);
                if (arguments_[1]) {
                    return Reflect.construct(target, [arguments_[0] * 2], newTarget);
                }
                return Reflect.construct(target, arguments_, newTarget);
            },
        };
        p = new Proxy(f, handler);

        Reflect.construct(p, [15], theNewTarget);
    });

    test("trap receives only passed arguments", () => {
        let receivedArguments;
        const p = new Proxy(
            class {
                constructor(_formalParameter) {}
            },
            {
                construct(target, arguments_, newTarget) {
                    receivedArguments = arguments_;
                    return Reflect.construct(target, arguments_, newTarget);
                },
            }
        );

        new p();
        expect(receivedArguments.length).toBe(0);

        new p(1, 2);
        expect(receivedArguments.length).toBe(2);
        expect(receivedArguments[0]).toBe(1);
        expect(receivedArguments[1]).toBe(2);
    });

    test("bound proxy constructor preserves empty argument list", () => {
        let receivedArguments;
        const p = new Proxy(
            class {
                constructor(_formalParameter) {}
            },
            {
                construct(target, arguments_, newTarget) {
                    receivedArguments = arguments_;
                    return Reflect.construct(target, arguments_, newTarget);
                },
            }
        );

        new (p.bind(undefined))();
        expect(receivedArguments.length).toBe(0);
    });
});

describe("[[Construct]] invariants", () => {
    test("target must have a [[Construct]] slot", () => {
        [{}, [], new Proxy({}, {})].forEach(item => {
            expect(() => {
                new new Proxy(item, {})();
            }).toThrowWithMessage(TypeError, "[object ProxyObject] is not a constructor");
        });
    });
});
