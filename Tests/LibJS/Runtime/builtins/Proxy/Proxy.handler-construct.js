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
