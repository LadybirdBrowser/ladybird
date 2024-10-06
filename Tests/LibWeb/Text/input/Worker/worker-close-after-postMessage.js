self.onmessage = function () {
    postMessage("PASS (didn't hang)");
    self.close();
};
