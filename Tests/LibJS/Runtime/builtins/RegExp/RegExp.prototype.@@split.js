describe("basic functionality", () => {
    test("observes individual flag overrides", () => {
        const regexp = /^safe$/;
        const input = {
            toString() {
                Object.defineProperty(regexp, "ignoreCase", { value: true });
                return "SAFE";
            },
        };

        const result = RegExp.prototype[Symbol.split].call(regexp, input);
        expect(JSON.stringify(result)).toBe('["",""]');
    });
});
