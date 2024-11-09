test("basic functionality", () => {
    var last = 0;
    for (var i = 0; i < 100; ++i) {
        var now = Date.now();
        expect(now).not.toBeNaN();
        expect(Math.floor(now)).toBe(now);
        expect(now).toBeGreaterThan(1580000000000);
        expect(now).toBeGreaterThanOrEqual(last);
        last = now;
    }
});
