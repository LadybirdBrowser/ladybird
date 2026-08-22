// A sync XHR send() in a worker: the worker's event loop is paused while send() blocks, so the timer and the
// microtask armed here must not run until send() has returned.
self.onmessage = event => {
    const log = [];
    setTimeout(() => log.push("timer fired"), 0);
    Promise.resolve().then(() => log.push("microtask ran"));

    const xhr = new XMLHttpRequest();
    xhr.open("GET", event.data, false);
    log.push("before send");
    xhr.send();
    log.push(`after send: status=${xhr.status} responseText=${xhr.responseText}`);

    setTimeout(() => {
        postMessage(log.join("\n"));
        self.close();
    }, 0);
};
