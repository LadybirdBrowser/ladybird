describe("cached property additions do not bypass exotic [[Set]] behavior", () => {
    test("typed array with a named key", () => {
        function put(object, value) {
            object["-0"] = value;
        }
        put(Object.create(Int8Array.prototype), 1);
        const typedArray = new Int8Array(2);
        put(typedArray, 7);
        expect(Object.getOwnPropertyNames(typedArray)).toEqual(["0", "1"]);
    });

    test("array length with a named key", () => {
        function put(object, value) {
            object.length = value;
        }
        put(Object.create(Array.prototype), 5);
        const array = [1, 2, 3];
        put(array, 1);
        expect(array).toEqual([1]);
        expect(Object.getOwnPropertyNames(array)).toEqual(["0", "length"]);
    });
});
