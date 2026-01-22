test("postfix increment evaluation order", () => {
    function bar(a, b) {
        expect(a).toBe(0);
        expect(b).toBe(0);
    }

    function foo() {
        let i = 0;
        bar(i, i++);
        expect(i).toBe(1);
    }
    foo();
});
