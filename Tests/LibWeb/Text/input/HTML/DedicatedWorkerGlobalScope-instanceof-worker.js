onmessage = function (event) {
    if (event.data === "run") {
        postMessage({ result: self instanceof DedicatedWorkerGlobalScope });
    }
};
