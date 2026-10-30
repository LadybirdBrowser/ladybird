export function permissionTest(origin, sender) {
  function ping(message) {
    if (!globalThis.WorkerGlobalScope) {
      window.top.postMessage(message, origin);
    } else {
      globalThis.postMessage(message);
    }
    n.close();
  }

  const n = new Notification(sender);
  const permission = Notification.permission;
  n.onshow = () => ping({ sender, permission, shown: true });
  n.onerror = () => ping({ sender, permission, shown: false });

  if (!globalThis.WorkerGlobalScope) {
    Notification.requestPermission().then(permission => ping({ sender: `${sender}Request`, permission }));

    const workerUrl = new URL(`./permission-worker.js`, import.meta.url);
    workerUrl.searchParams.set("sender", `${sender}Worker`);
    const worker = new Worker(workerUrl, { type: "module" });
    worker.onmessage = ev => ping(ev.data, origin);
  }
}
