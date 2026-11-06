describe("an assignment to a class declaration's binding can read the binding", () => {
    test("in a function", () => {
        function f() {
            class C {}
            C = C;
            return typeof C;
        }
        expect(f()).toBe("function");
    });

    test("in a block", () => {
        function f() {
            {
                class C {}
                C = [C][0];
                return typeof C;
            }
        }
        expect(f()).toBe("function");
    });

    test("as a call argument", () => {
        function f() {
            class C {}
            C = ((c, d) => d)([], C);
            return C.name;
        }
        expect(f()).toBe("C");
    });

    test("in a compound assignment", () => {
        function f() {
            class C {}
            C += "";
            return typeof C;
        }
        expect(f()).toBe("string");
    });

    test("in a logical assignment", () => {
        function f() {
            class C {}
            C ??= 1;
            return typeof C;
        }
        expect(f()).toBe("function");
    });
});

test("a class declaration's binding is still in its dead zone before the declaration", () => {
    expect(() => {
        C = C;
        class C {}
    }).toThrowWithMessage(ReferenceError, "is not initialized");
});
