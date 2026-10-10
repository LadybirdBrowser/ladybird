// Results of native calls made by inlined callees must survive popping the callee's frame, also when the call or a
// check after it exits to the interpreter.

test("native results returned through inlined frames", () => {
    const failures = compareCompiledWithInterpreter(
        () => {
            const inner = (array, value) => array.push(value) + Math.max(value, array.length);
            const middle = (array, value) => inner(array, value) * 2;
            return (value, log) => {
                const array = [1, 2];
                const result = middle(array, value);
                log.push(array.length);
                return [result, String(value).charCodeAt(0), array];
            };
        },
        {
            warmUp: (log, i) => [i, log],
            probes: {
                double: log => [1.5, log],
                string: log => ["7", log],
                object: log => [{ valueOf: () => (log.push("valueOf"), 3) }, log],
                overflow: log => [2147483647, log],
            },
        }
    );
    expect(failures).toEqual([]);
});
