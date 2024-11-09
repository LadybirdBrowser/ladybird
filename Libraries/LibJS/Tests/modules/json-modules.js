describe("basic behavior", () => {
    test("can import json modules", () => {
        let passed = false;
        let error = null;
        let result = null;

        import("./json-module.json", { assert: { type: "json" } })
            .then(jsonObj => {
                passed = true;
                result = jsonObj;
            })
            .catch(err => {
                error = err;
            });

        runQueuedPromiseJobs();

        if (error) throw error;

        console.log(JSON.stringify(result));
        expect(passed).toBeTrue();

        expect(result).not.toBeNull();
        expect(result).not.toBeUndefined();

        const jsonResult = result.default;
        expect(jsonResult).not.toBeNull();
        expect(jsonResult).not.toBeUndefined();

        expect(jsonResult).toHaveProperty("value", "value");
        expect(jsonResult).toHaveProperty("array", [1, 2, 3]);
        expect(jsonResult).toHaveProperty("map", { innerValue: "innerValue" });
    });
});
