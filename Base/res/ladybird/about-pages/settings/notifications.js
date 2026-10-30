const enableNotifications = document.querySelector("#enable-notifications");

enableNotifications.addEventListener("change", () => {
    ladybird.sendMessage("setNotificationsPolicy", enableNotifications.checked ? "ask" : "block");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name !== "loadSettings") {
        return;
    }
    if (!Object.hasOwn(event.detail.data, "notificationsPolicy")) {
        return;
    }

    enableNotifications.checked = event.detail.data.notificationsPolicy === "ask";
});
