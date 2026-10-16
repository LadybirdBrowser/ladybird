const restorePreviousSession = document.querySelector("#restore-previous-session");
const newTabPageURL = document.querySelector("#new-tab-page-url");
const newTabPageURLReset = document.querySelector("#new-tab-page-url-reset");

const loadSettings = settings => {
    if (Object.hasOwn(settings, "restorePreviousSession")) {
        restorePreviousSession.checked = settings.restorePreviousSession === true;
    }

    if (Object.hasOwn(settings, "newTabPageURL")) {
        newTabPageURL.classList.remove("error");
        newTabPageURL.value = settings.newTabPageURL;
    }
};

restorePreviousSession.addEventListener("change", () => {
    ladybird.sendMessage("setRestorePreviousSession", restorePreviousSession.checked);
});

newTabPageURL.addEventListener("change", () => {
    newTabPageURL.classList.remove("success");
    newTabPageURL.classList.remove("error");

    if (!containsValidURL(newTabPageURL)) {
        newTabPageURL.classList.add("error");
        return;
    }

    ladybird.sendMessage("setNewTabPageURL", newTabPageURL.value);
    newTabPageURL.classList.add("success");

    setTimeout(() => {
        newTabPageURL.classList.remove("success");
    }, 1000);
});

newTabPageURLReset.addEventListener("click", () => {
    newTabPageURL.value = "about:newtab";
    newTabPageURL.dispatchEvent(new Event("change"));
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
