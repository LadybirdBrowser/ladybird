// A sync XHR send() in a worker: the worker's event loop is paused while send() blocks, so the timer, the microtask
// and the animation frame callback armed here must not run until send() has returned.
self.onmessage = event => {
    const log = [];
    let sendReturned = false;
    let animationFrameCallback = "did not run";
    setTimeout(() => log.push("timer fired"), 0);
    Promise.resolve().then(() => log.push("microtask ran"));
    requestAnimationFrame(() => (animationFrameCallback = sendReturned ? "ran after send" : "ran during send"));

    const xhr = new XMLHttpRequest();
    xhr.open("GET", event.data, false);
    log.push("before send");
    xhr.send();
    sendReturned = true;
    log.push(`after send: status=${xhr.status} responseText=${xhr.responseText}`);

    requestAnimationFrame(() => {
        setTimeout(() => {
            log.push(`animation frame callback ${animationFrameCallback}`);
            postMessage(log.join("\n"));
            self.close();
        }, 0);
    });
};
