test("check that codegen doesn't crash", () => {
    function func(x) {
        expect(x()).toBe(0);
    }

    function go() {
        for (let i = 0; ; ) {
            func(() => i);
            break;
        }
    }
});
