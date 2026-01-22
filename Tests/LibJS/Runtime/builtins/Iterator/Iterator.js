describe("errors", () => {
    test("called without new", () => {
        expect(() => {
            Iterator();
        }).toThrowWithMessage(TypeError, "Iterator constructor must be called with 'new'");
    });

    test("cannot be directly constructed", () => {
        expect(() => {
            new Iterator();
        }).toThrowWithMessage(TypeError, "Abstract class Iterator cannot be constructed directly");
    });
});

describe("normal behavior", () => {
    test("length is 0", () => {
        expect(Iterator).toHaveLength(0);
    });

    test("can be constructed from with subclass", () => {
        class TestIterator extends Iterator {}

        const iterator = new TestIterator();
        expect(iterator).toBeInstanceOf(TestIterator);
        expect(iterator).toBeInstanceOf(Iterator);
    });
});
