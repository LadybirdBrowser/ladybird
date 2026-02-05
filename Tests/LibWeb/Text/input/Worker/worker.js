let extraPort = null;

onmessage = evt => {
    if (evt.ports.length > 0) {
        extraPort = evt.ports[0];
        extraPort.onmessage = evt => {
            extraPort.postMessage("Extra Port got message: " + JSON.stringify(evt.data));
        };
        extraPort.postMessage("Worker got message port!");
    } else {
        postMessage(evt.data);
    }
};
postMessage("loaded");
