test("classes that a function makes see their own private names", () => {
    function makeClass(value) {
        return class {
            #value = value;
            static read(object) {
                return object.#value;
            }
        };
    }
    for (let i = 0; i < 100; ++i) {
        const A = makeClass(i);
        const B = makeClass(-i);
        expect(A.read(new A())).toBe(i);
        expect(() => A.read(new B())).toThrow(TypeError);
    }
});

test("methods, arrows and nested functions of a class read its private names", () => {
    class A {
        #x = 1;
        method() {
            const arrow = () => this.#x;
            function nested(object) {
                return object.#x;
            }
            return arrow() + nested(this);
        }
        static make() {
            return new A();
        }
    }
    for (let i = 0; i < 100; ++i) expect(A.make().method()).toBe(2);
});

test("nested classes in one function", () => {
    function outer() {
        class A {
            #a = "a";
            inner() {
                class B {
                    #b = "b";
                    read(a) {
                        return a.#a + this.#b;
                    }
                }
                return new B().read(this);
            }
            read() {
                return this.#a;
            }
        }
        const after = new A().read();
        return new A().inner() + after;
    }
    for (let i = 0; i < 100; ++i) expect(outer()).toBe("aba");
});

test("a function keeps the private environment around it after its class is done", () => {
    class Outer {
        #o = 5;
        test() {
            class Inner {
                #i = 1;
            }
            new Inner();
            return this.#o;
        }
    }
    for (let i = 0; i < 100; ++i) expect(new Outer().test()).toBe(5);
});

test("an exception in a class body leaves its private environment", () => {
    class Outer {
        #o = 7;
        test() {
            try {
                class Inner {
                    #i = 1;
                    static [(() => {
                        throw new Error("thrown");
                    })()] = 1;
                }
            } catch {}
            return this.#o;
        }
    }
    for (let i = 0; i < 100; ++i) expect(new Outer().test()).toBe(7);
});

test("direct eval in a class method sees its private names", () => {
    class A {
        #x = 3;
        read() {
            return eval("this.#x");
        }
        makeInEval() {
            return eval("class B { #y = 4; static read(b) { return b.#y; } }; B.read(new B()) + this.#x");
        }
    }
    for (let i = 0; i < 20; ++i) {
        expect(new A().read()).toBe(3);
        expect(new A().makeInEval()).toBe(7);
    }
});

test("generators and async functions of a class", async () => {
    function make() {
        return class {
            #v = 2;
            *values() {
                yield this.#v;
                class Inner {
                    #w = 3;
                    static read(inner) {
                        return inner.#w;
                    }
                }
                yield Inner.read(new Inner());
                yield this.#v;
            }
            async value() {
                await null;
                return this.#v;
            }
        };
    }
    for (let i = 0; i < 50; ++i) {
        const C = make();
        expect([...new C().values()]).toEqual([2, 3, 2]);
    }
    const C = make();
    let total = 0;
    for (let i = 0; i < 20; ++i) total += await new C().value();
    expect(total).toBe(40);
});
