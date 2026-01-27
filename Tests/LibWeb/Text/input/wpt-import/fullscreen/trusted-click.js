/**
 * Invokes callback from a trusted click event, avoiding interception by fullscreen element.
 *
 * @param {Element} container - Element where button will be created and clicked.
 */
function trusted_click(container = document.body) {
    var document = container.ownerDocument;
    var button = document.createElement("button");
    button.textContent = "click to continue test";
    button.style.display = "block";
    button.style.fontSize = "20px";
    button.style.padding = "10px";
    button.addEventListener("click", () => {
        button.remove();
    });
    container.appendChild(button);
    if (window.top !== window) test_driver.set_test_context(window.top);
    // Race them for manually testing...
    return Promise.race([
        test_driver.click(button),
        new Promise((resolve) => {
            button.addEventListener("click", resolve);
        }),
    ]);
}

// Invokes element.requestFullscreen() from a trusted click.
async function trusted_request(element = document.body, whereToCreateButton = null) {
    await trusted_click(whereToCreateButton ?? element.parentNode ?? element);
    return element.requestFullscreen();
}

/**
 * Used to await a fullscreen change event, once.
 *
 * @param {EventTarget} target
 * @returns
 */
function fullScreenChange(target = document) {
    return new Promise((resolve) =>
        target.addEventListener("fullscreenchange", resolve, { once: true })
    );
}

/**
 * Sets up a message event listener, and returns a promise that resolves
 * when the message from the iframe is received.
 *
 * @param {HTMLIFrameElement} iframe
 * @returns {Promise<object>}
 */
function promiseMessage(iframe) {
    return new Promise((resolve) => {
        window.addEventListener(
            "message",
            (e) => {
                if (e.data?.report.api === "fullscreen") {
                    resolve(e.data);
                }
            },
            { once: true }
        );
        iframe.contentWindow.postMessage({ action: "report" }, "*");
    });
}
