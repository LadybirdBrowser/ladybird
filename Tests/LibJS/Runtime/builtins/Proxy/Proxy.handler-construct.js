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

    test("only the passed arguments are supplied to the trap", () => {
        const handler = {
            construct(target, arguments_) {
                return { arguments_ };
            },
        };
        const p = new Proxy(function (a, b, c) {}, handler);

        expect(new p().arguments_).toEqual([]);
        expect(new p(1).arguments_).toEqual([1]);
        expect(new p(1, 2, 3, 4).arguments_).toEqual([1, 2, 3, 4]);
        expect(Reflect.construct(p, [1]).arguments_).toEqual([1]);
        expect(new (p.bind(null, 1))(2).arguments_).toEqual([1, 2]);
        expect(new new Proxy(p, {})(1).arguments_).toEqual([1]);

        class C extends p {
            constructor(...arguments_) {
                return super(...arguments_);
            }
        }
        expect(new C(1).arguments_).toEqual([1]);
    });

    test("only the passed arguments are forwarded to the target", () => {
        function f(a, b, c) {
            this.argumentCount = arguments.length;
        }
        const p = new Proxy(f, {});

        expect(new p().argumentCount).toBe(0);
        expect(new p(1).argumentCount).toBe(1);
        expect(new p(1, 2, 3, 4).argumentCount).toBe(4);
        expect(Reflect.construct(p, [1]).argumentCount).toBe(1);
        expect(new (p.bind(null, 1))(2).argumentCount).toBe(2);
        expect(new new Proxy(p, {})(1).argumentCount).toBe(1);
        expect(new new Proxy(f.bind(null, 1), {})(2).argumentCount).toBe(2);
    });

    test("rest parameters of the target only receive the passed arguments", () => {
        function f(a, b, ...rest) {
            this.rest = rest;
        }
        const p = new Proxy(f, {});

        expect(new p(1).rest).toEqual([]);
        expect(new p(1, 2).rest).toEqual([]);
        expect(new p(1, 2, 3).rest).toEqual([3]);
        expect(new new Proxy(p, {})(1).rest).toEqual([]);
        expect(new new Proxy(f.bind(null, 1), {})(2).rest).toEqual([]);
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
