const newTabPageURL = document.querySelector("#new-tab-page-url");
const newTabPageURLReset = document.querySelector("#new-tab-page-url-reset");

const loadSettings = settings => {
    newTabPageURL.classList.remove("error");
    newTabPageURL.value = settings.newTabPageURL;
};

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
