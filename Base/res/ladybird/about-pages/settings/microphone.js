const enableMicrophone = document.querySelector("#enable-microphone");

enableMicrophone.addEventListener("change", () => {
    ladybird.sendMessage("setMicrophonePolicy", enableMicrophone.checked ? "ask" : "block");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name !== "loadSettings") {
        return;
    }
    if (!Object.hasOwn(event.detail.data, "microphonePolicy")) {
        return;
    }

    enableMicrophone.checked = event.detail.data.microphonePolicy === "ask";
});
