test("eval of nested code near the call stack size limit", () => {
    const sources = [
        "try{".repeat(100) + "1" + "}finally{}".repeat(100),
        "{".repeat(150) + "1" + "}".repeat(150),
        "if (1) ".repeat(150) + "1;",
        "var a; " + "a = ".repeat(150) + "1",
        "var " + "[".repeat(150) + "a" + "]".repeat(150) + " = [];",
        Array.from({ length: 1900 }, (_, i) => `l${i}:`).join("") + ";",
    ];
    let levels = 0;
    function recurse() {
        try {
            Reflect.apply(recurse, null, []);
        } catch {}
        if (levels++ > 1000) return;
        for (const source of sources) {
            try {
                eval(source);
            } catch {}
            try {
                (0, eval)(source);
            } catch {}
        }
    }
    recurse();
    expect(sources.slice(0, 4).map(source => (0, eval)(source))).toEqual([1, 1, 1, 1]);
});
