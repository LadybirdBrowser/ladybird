const showFeedbackButtonGroup = document.querySelector("#show-feedback-button-group");
const showFeedbackButton = document.querySelector("#show-feedback-button");
const crashModes = [...document.querySelectorAll("input[name=crash-report-sending]")];
const crashReportsWaitingGroup = document.querySelector("#crash-reports-waiting-group");
const crashReportsWaiting = document.querySelector("#crash-reports-waiting");
const crashReportsStatus = document.querySelector("#crash-reports-status");

function loadSettings(settings) {
    const variable = settings.configVariableDefinitions?.find(
        candidate => candidate.name === "ui.toolbar.show_feedback_button"
    );
    if (variable) {
        showFeedbackButton.checked = variable.value;
        showFeedbackButtonGroup.classList.remove("hidden");
    }

    if (Object.hasOwn(settings, "crashReportSending")) {
        crashModes.forEach(mode => {
            mode.checked = mode.value === settings.crashReportSending;
        });
    }
}

showFeedbackButton.addEventListener("change", () => {
    ladybird.sendMessage("setConfigVariable", {
        name: "ui.toolbar.show_feedback_button",
        value: showFeedbackButton.checked,
    });
});

crashModes.forEach(mode => {
    mode.addEventListener("change", () => ladybird.sendMessage("setCrashReportSending", mode.value));
});

document.querySelector("#review-crash-reports").addEventListener("click", () => {
    location.href = "about:crashes";
});

document.querySelector("#open-crash-reports").addEventListener("click", () => {
    crashReportsStatus.hidden = true;
    ladybird.sendMessage("showCrashReports");
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadCrashReportsWaiting");
});

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;
    if (name === "loadSettings") {
        loadSettings(data);
    } else if (name === "crashReportsStatus") {
        crashReportsStatus.textContent = data;
        crashReportsStatus.hidden = !data;
    } else if (name === "crashReportsWaiting") {
        crashReportsWaiting.textContent = `${data} ${data === 1 ? "report" : "reports"} waiting`;
        crashReportsWaitingGroup.classList.toggle("hidden", data === 0);
    }
});
