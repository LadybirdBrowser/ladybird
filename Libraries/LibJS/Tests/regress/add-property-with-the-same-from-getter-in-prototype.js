test("Mutation of object in getter should result in skipping of collecting inline cache data", () => {
    class Color {
        get rgb() {
            const value = [];
            Object.defineProperty(this, "rgb", { value });
            return value;
        }

        set rgb(value) {
            Object.defineProperty(this, "rgb", { value });
        }
    }

    function testGetting() {
        const c = new Color();
        for (let i = 0; i < 2; i++) {
            c.rgb;
        }
    }

    function testSetting() {
        const c = new Color();
        for (let i = 0; i < 2; i++) {
            c.rgb = i;
        }
    }

    expect(testGetting).not.toThrow();
    expect(testSetting).not.toThrow();
});
