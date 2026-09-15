// Copyright (c) 2026-present, the Ladybird developers.
// SPDX-License-Identifier: BSD-2-Clause

(() => {
    // Appended to the runner through WebDriver's resource map, before page load.
    // Observe the runner's existing boundaries without replacing benchmark workloads.
    if (typeof BenchmarkRunner !== "function" || typeof startBenchmark !== "function")
        throw new Error("This URL does not expose the StyleBench runner");
    const capture = (window.styleUpdateCapture = { samples: [], done: false, started: false, expected: [] });
    const snapshot = window => ({
        rust: window.internals.styleEngineCounters(),
        cpp: window.internals.getStyleInvalidationCounters(),
    });
    const zero = value =>
        Object.fromEntries(
            Object.entries(value).map(([lane, fields]) => [
                lane,
                Object.fromEntries(
                    Object.entries(fields).map(([key, value]) => [key, typeof value === "number" ? 0 : value])
                ),
            ])
        );
    const record = (name, kind, before, after, extra = {}) =>
        capture.samples.push({
            name,
            kind,
            before,
            after,
            mutationMicroseconds: null,
            observationMicroseconds: null,
            ...extra,
        });
    const runTest = BenchmarkRunner.prototype._runTest;
    BenchmarkRunner.prototype._runTest = function (suite, test, prepared, callback) {
        const before = snapshot(this._frame.contentWindow);
        const suiteName = suite.name;
        record(
            `${suiteName}/${test.name}/before`,
            this.capturePrevious ? "other" : "setup",
            this.capturePrevious || zero(before),
            before
        );
        this.captureSuite = suiteName;
        return runTest.call(this, suite, test, prepared, (sync, async) => {
            const after = snapshot(this._frame.contentWindow);
            record(`${suiteName}/${test.name}`, "mutation", before, after, {
                benchmarkSyncMicroseconds: sync * 1000,
                benchmarkAsyncMicroseconds: async * 1000,
            });
            this.capturePrevious = after;
            return callback(sync, async);
        });
    };
    const removeFrame = BenchmarkRunner.prototype._removeFrame;
    BenchmarkRunner.prototype._removeFrame = function () {
        if (this._frame) {
            const after = snapshot(this._frame.contentWindow);
            record(`${this.captureSuite}/after`, "other", this.capturePrevious || zero(after), after);
            record(this.captureSuite, "suite-total", zero(after), after);
        }
        this.capturePrevious = null;
        return removeFrame.call(this);
    };
    let before;
    const runMultipleIterations = BenchmarkRunner.prototype.runMultipleIterations;
    BenchmarkRunner.prototype.runMultipleIterations = function (count) {
        if (capture.started || count !== 1) throw new Error("Capture requires exactly one StyleBench iteration");
        capture.started = true;
        capture.expected = this._suites
            .filter(suite => !suite.disabled)
            .flatMap(suite => suite.tests.map(test => `${suite.name}/${test.name}`));
        before = snapshot(window);
        return runMultipleIterations.call(this, count);
    };
    const finished = benchmarkClient.didFinishLastIteration;
    benchmarkClient.didFinishLastIteration = function (...args) {
        const result = finished.apply(this, args);
        record("runner-document", "other", before, snapshot(window));
        capture.done = true;
        return result;
    };
    // Fail if the runner changes its boundary API instead of silently capturing no work.
    if (typeof runTest !== "function" || typeof removeFrame !== "function" || typeof finished !== "function")
        throw new Error("Unsupported StyleBench boundary API");
})();
