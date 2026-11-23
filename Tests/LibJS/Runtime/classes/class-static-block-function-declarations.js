test("function declarations of a static block are not bindings of the block around the class", () => {
    function inBlock() {
        var b = "outer";
        {
            class C {
                static {
                    function b() {}
                }
            }
            return typeof b;
        }
    }
    function inCatch() {
        try {
            throw 0;
        } catch (e) {
            class C {
                static {
                    function e() {}
                }
            }
            return typeof e;
        }
    }
    function inSwitch() {
        switch (1) {
            case 1:
                class C {
                    static {
                        function b() {}
                    }
                }
                return typeof b;
        }
    }
    expect(inBlock()).toBe("string");
    expect(inCatch()).toBe("number");
    expect(inSwitch()).toBe("undefined");
});

test("function declarations of a static block can use private names", () => {
    let result;
    {
        class C {
            static #p = 7;
            static {
                function read(o) {
                    return o.#p;
                }
                result = read(C);
            }
        }
        expect(() => read(C)).toThrowWithMessage(ReferenceError, "'read' is not defined");
    }
    expect(result).toBe(7);
});

test("function declarations of a static block see the bindings of the block", () => {
    let result;
    {
        class C {
            static {
                var q = 1;
                function b() {
                    return q;
                }
                result = b();
            }
        }
    }
    expect(result).toBe(1);
});

test("function declarations in a block of a static block", () => {
    let result;
    {
        class C {
            static {
                {
                    function inner() {
                        return 2;
                    }
                    result = inner();
                }
            }
        }
        expect(typeof inner).toBe("undefined");
    }
    expect(result).toBe(2);
});
