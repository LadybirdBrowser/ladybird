const importScriptsAndCheck = () => {
    const runsBefore = self.importedScriptRuns ?? 0;
    try {
        importScripts("worker-importScripts-in-microtask-imported.js");
    } catch (error) {
        return `threw ${error.name}`;
    }
    return self.importedScriptRuns === runsBefore + 1 ? "the imported script ran" : "the imported script did not run";
};

Promise.resolve().then(() => {
    const fromPromiseReaction = importScriptsAndCheck();
    queueMicrotask(() => {
        self.postMessage({ fromPromiseReaction, fromQueueMicrotask: importScriptsAndCheck() });
    });
});
