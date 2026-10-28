promise_test(async () => {
  const worker = new Worker(URL.createObjectURL(new Blob([`
    postMessage("load");
    while (true) {
      navigator.permissions.query({ name: "geolocation" });
    }
  `])));
  await new Promise(resolve => {
    worker.onmessage = (e) => {
      if (e.data === "load") {
        worker.terminate();
        resolve();
      }
    };
  });
}, "Terminating worker after permission query should not crash");
