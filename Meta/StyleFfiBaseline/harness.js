// Shared harness for the (temporary) style FFI baseline workloads: runs the
// workload with counters reset, then prints every counter.
function runFfiBaselineWorkload(workload) {
    test(() => {
        internals.resetStyleFfiCounters();
        workload();
        internals.updateStyle();
        const counters = internals.styleFfiCounters();
        for (const key of Object.keys(counters)) println(`${key}: ${counters[key]}`);
    });
}
