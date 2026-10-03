describe("property lookup cache entries whose cells were collected", () => {
    test("property found on a prototype that is no longer in the chain", () => {
        function makeMiddle() {
            return Object.create({ x: "inherited" });
        }
        function read(object) {
            return object.x;
        }

        const middle = makeMiddle();
        const receiver = Object.create(middle);
        receiver.y = "own";
        expect(read(receiver)).toBe("inherited");

        Object.setPrototypeOf(middle, null);
        gc();
        expect(read(receiver)).toBeUndefined();
    });

    test("property addition after a setter was defined on the prototype chain", () => {
        const base = {};
        const prototype = Object.create(base);
        function make() {
            const object = Object.create(prototype);
            object.x = 1;
            return object;
        }

        const plain = Object.create(prototype);
        const withX = make();

        let calls = 0;
        Object.defineProperty(base, "x", {
            set(value) {
                ++calls;
            },
        });
        gc();

        const object = make();
        expect(calls).toBe(1);
        expect(Object.hasOwn(object, "x")).toBeFalse();
        expect(Object.hasOwn(plain, "x")).toBeFalse();
        expect(withX.x).toBe(1);
    });
});
