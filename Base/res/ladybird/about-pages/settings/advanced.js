const configFilter = document.querySelector("#config-filter");
const configList = document.querySelector("#config-list");

function sendConfigVariableValue(variable, value) {
    ladybird.sendMessage("setConfigVariable", {
        name: variable.name,
        value,
    });
}

function createControl(variable) {
    const control = document.createElement("div");
    control.classList.add("config-control");

    if (variable.type === "boolean") {
        const input = document.createElement("input");
        input.type = "checkbox";
        input.toggleAttribute("switch", true);
        input.checked = !!variable.value;
        input.addEventListener("change", () => {
            sendConfigVariableValue(variable, input.checked);
        });
        control.append(input);
        return control;
    }

    const input = document.createElement("input");
    input.value = variable.value ?? "";

    if (variable.type === "number") {
        input.type = "number";
        input.step = "any";
        input.addEventListener("change", () => {
            const value = Number.parseFloat(input.value);
            if (!Number.isNaN(value)) {
                sendConfigVariableValue(variable, value);
            }
        });
    } else {
        input.type = "text";
        input.addEventListener("change", () => {
            sendConfigVariableValue(variable, input.value);
        });
    }

    control.append(input);
    return control;
}

function createRow(variable) {
    const row = document.createElement("div");
    row.classList.add("config-row");
    row.dataset.filterText = `${variable.name} ${variable.title} ${variable.description}`.toLowerCase();

    const details = document.createElement("div");

    const name = document.createElement("div");
    name.classList.add("config-name");
    name.textContent = variable.name;

    const title = document.createElement("div");
    title.classList.add("config-title");
    title.textContent = variable.title;

    const description = document.createElement("p");
    description.classList.add("config-description", "description");
    description.textContent = variable.description;

    details.append(name, title, description);
    row.append(details, createControl(variable));
    return row;
}

function applyFilter() {
    const filter = configFilter.value.trim().toLowerCase();

    document.querySelectorAll(".config-row").forEach(row => {
        row.classList.toggle("hidden", filter.length !== 0 && !row.dataset.filterText.includes(filter));
    });
}

function loadConfigVariables(settings) {
    const configVariables = settings.configVariableDefinitions || [];
    configList.innerHTML = "";

    configVariables.forEach(variable => {
        configList.append(createRow(variable));
    });

    applyFilter();
}

configFilter.addEventListener("input", applyFilter);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadConfigVariables(event.detail.data);
    }
});
