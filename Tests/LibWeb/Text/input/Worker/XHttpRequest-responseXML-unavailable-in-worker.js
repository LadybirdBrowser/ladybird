self.onmessage = function () {
    let xhr = new XMLHttpRequest();
    postMessage(xhr.responseXML === undefined ? "PASS" : "FAIL");
    self.close();
};
