self.onmessage = event => {
    const log = [];
    const note = entry => log.push(entry);
    const armed = [
        "timer",
        "posted message",
        "animation frame callback",
        "promise reaction",
        "queueMicrotask() callback",
    ];

    setTimeout(() => note("timer"), 0);
    const channel = new MessageChannel();
    channel.port1.onmessage = () => note("posted message");
    channel.port2.postMessage("x");
    requestAnimationFrame(() => note("animation frame callback"));
    Promise.resolve().then(() => note("promise reaction"));
    queueMicrotask(() => note("queueMicrotask() callback"));

    importScripts(event.data);

    const results = [
        `The imported script ran: ${self.importedScriptRan === true}`,
        `Ran while importScripts() was blocked: ${log.join(", ") || "(nothing)"}`,
    ];

    const report = () => {
        const missing = armed.filter(entry => !log.includes(entry));
        const duplicated = armed.filter(entry => log.filter(logged => logged === entry).length > 1);
        const firstTask = Math.min(log.indexOf("timer"), log.indexOf("posted message"));
        const promiseReaction = log.indexOf("promise reaction");
        const queuedMicrotask = log.indexOf("queueMicrotask() callback");
        results.push(`Never ran after importScripts() returned: ${missing.join(", ") || "(none)"}`);
        results.push(`Ran more than once: ${duplicated.join(", ") || "(none)"}`);
        results.push(
            `The microtasks ran before the tasks: ${promiseReaction < firstTask && queuedMicrotask < firstTask}`
        );
        results.push(`The microtasks ran in the order they were queued: ${promiseReaction < queuedMicrotask}`);
        self.postMessage(results);
    };

    requestAnimationFrame(() => setTimeout(report, 0));
};
