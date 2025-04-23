const languagesAdd = document.querySelector("#languages-add");
const languagesClose = document.querySelector("#languages-close");
const languagesDialog = document.querySelector("#languages-dialog");
const languagesList = document.querySelector("#languages-list");
const languagesSelect = document.querySelector("#languages-select");
const languagesSettings = document.querySelector("#languages-settings");

let LANGUAGES = {};

function loadSettings(settings) {
    LANGUAGES = settings.languages;

    if (languagesDialog.open) {
        showLanguages();
    }
}

function languageDisplayName(language) {
    const item = AVAILABLE_LANGUAGES.find(item => item.language === language);
    return item.displayName;
}

function saveLanguages() {
    ladybird.sendMessage("setLanguages", LANGUAGES);
}

function moveLanguage(from, to) {
    [LANGUAGES[from], LANGUAGES[to]] = [LANGUAGES[to], LANGUAGES[from]];
    saveLanguages();
}

function removeLanguage(index) {
    LANGUAGES.splice(index, 1);
    saveLanguages();
}

function loadLanguages() {
    for (const language of AVAILABLE_LANGUAGES) {
        const option = document.createElement("option");
        option.text = language.displayName;
        option.value = language.language;

        languagesSelect.add(option);
    }
}

function showLanguages() {
    languagesList.innerHTML = "";

    LANGUAGES.forEach((language, index) => {
        const name = document.createElement("span");
        name.className = "dialog-list-item-label";
        name.textContent = languageDisplayName(language);

        const moveUp = document.createElement("button");
        moveUp.className = "dialog-button";
        moveUp.innerHTML = upwardArrowSVG;
        moveUp.title = "Move up";

        if (index === 0) {
            moveUp.disabled = true;
        } else {
            moveUp.addEventListener("click", () => {
                moveLanguage(index, index - 1);
            });
        }

        const moveDown = document.createElement("button");
        moveDown.className = "dialog-button";
        moveDown.innerHTML = downwardArrowSVG;
        moveDown.title = "Move down";

        if (index === LANGUAGES.length - 1) {
            moveDown.disabled = true;
        } else {
            moveDown.addEventListener("click", () => {
                moveLanguage(index, index + 1);
            });
        }

        const remove = document.createElement("button");
        remove.className = "dialog-button";
        remove.innerHTML = "&times;";
        remove.title = "Remove";

        if (LANGUAGES.length <= 1) {
            remove.disabled = true;
        } else {
            remove.addEventListener("click", () => {
                removeLanguage(index);
            });
        }

        const controls = document.createElement("div");
        controls.className = "dialog-controls";
        controls.appendChild(moveUp);
        controls.appendChild(moveDown);
        controls.appendChild(remove);

        const item = document.createElement("div");
        item.className = "dialog-list-item";
        item.appendChild(name);
        item.appendChild(controls);

        languagesList.appendChild(item);
    });

    for (const language of languagesSelect.options) {
        language.disabled = LANGUAGES.includes(language.value);
    }

    if (!languagesDialog.open) {
        setTimeout(() => languagesSelect.focus());
        languagesDialog.showModal();
    }
}

languagesAdd.addEventListener("click", () => {
    const language = languagesSelect.value;

    languagesAdd.disabled = true;
    languagesSelect.selectedIndex = 0;

    if (!language || LANGUAGES.includes(language)) {
        return;
    }

    LANGUAGES.push(language);
    saveLanguages();
});

languagesClose.addEventListener("click", () => {
    languagesDialog.close();
});

languagesSelect.addEventListener("change", () => {
    languagesAdd.disabled = !languagesSelect.value;
});

languagesSettings.addEventListener("click", event => {
    showLanguages();
    event.stopPropagation();
});

document.addEventListener("WebUILoaded", () => {
    loadLanguages();
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});

// Rather than creating a list of all languages supported by ICU (of which there are on the order of a thousand), we
// create a list of languages that are supported by both Chrome and Firefox. We can extend this list as needed.
//
// https://github.com/chromium/chromium/blob/main/ui/base/l10n/l10n_util.cc (see kAcceptLanguageList)
// https://github.com/mozilla/gecko-dev/blob/master/intl/locale/language.properties
const AVAILABLE_LANGUAGES = (() => {
    const display = new Intl.DisplayNames([], { type: "language", languageDisplay: "standard" });

    const language = languageID => {
        return {
            language: languageID,
            displayName: display.of(languageID),
        };
    };

    const languages = [
        language("af"),
        language("ak"),
        language("am"),
        language("ar"),
        language("as"),
        language("ast"),
        language("az"),
        language("be"),
        language("bg"),
        language("bm"),
        language("bn"),
        language("br"),
        language("bs"),
        language("ca"),
        language("cs"),
        language("cy"),
        language("da"),
        language("de"),
        language("de-AT"),
        language("de-CH"),
        language("de-DE"),
        language("de-LI"),
        language("ee"),
        language("el"),
        language("en"),
        language("en-AU"),
        language("en-CA"),
        language("en-GB"),
        language("en-IE"),
        language("en-NZ"),
        language("en-US"),
        language("en-ZA"),
        language("eo"),
        language("es"),
        language("es-AR"),
        language("es-CL"),
        language("es-CO"),
        language("es-CR"),
        language("es-ES"),
        language("es-HN"),
        language("es-MX"),
        language("es-PE"),
        language("es-UY"),
        language("es-VE"),
        language("et"),
        language("eu"),
        language("fa"),
        language("fi"),
        language("fo"),
        language("fr"),
        language("fr-CA"),
        language("fr-CH"),
        language("fr-FR"),
        language("fy"),
        language("ga"),
        language("gd"),
        language("gl"),
        language("gu"),
        language("ha"),
        language("haw"),
        language("he"),
        language("hi"),
        language("hr"),
        language("hu"),
        language("hy"),
        language("ia"),
        language("id"),
        language("ig"),
        language("is"),
        language("it"),
        language("it-CH"),
        language("ja"),
        language("jv"),
        language("ka"),
        language("kk"),
        language("km"),
        language("kn"),
        language("ko"),
        language("kok"),
        language("ku"),
        language("ky"),
        language("lb"),
        language("lg"),
        language("ln"),
        language("lo"),
        language("lt"),
        language("lv"),
        language("mai"),
        language("mg"),
        language("mi"),
        language("mk"),
        language("ml"),
        language("mn"),
        language("mr"),
        language("ms"),
        language("mt"),
        language("my"),
        language("nb"),
        language("ne"),
        language("nl"),
        language("nn"),
        language("no"),
        language("nso"),
        language("oc"),
        language("om"),
        language("or"),
        language("pa"),
        language("pl"),
        language("ps"),
        language("pt"),
        language("pt-BR"),
        language("pt-PT"),
        language("qu"),
        language("rm"),
        language("ro"),
        language("ru"),
        language("rw"),
        language("sa"),
        language("sd"),
        language("si"),
        language("sk"),
        language("sl"),
        language("so"),
        language("sq"),
        language("sr"),
        language("st"),
        language("su"),
        language("sv"),
        language("sw"),
        language("ta"),
        language("te"),
        language("tg"),
        language("th"),
        language("ti"),
        language("tk"),
        language("tn"),
        language("to"),
        language("tr"),
        language("tt"),
        language("ug"),
        language("uk"),
        language("ur"),
        language("uz"),
        language("vi"),
        language("wo"),
        language("xh"),
        language("yi"),
        language("yo"),
        language("zh"),
        language("zh-CN"),
        language("zh-HK"),
        language("zh-TW"),
        language("zu"),
    ];

    languages.sort((lhs, rhs) => {
        return lhs.displayName.localeCompare(rhs.displayName);
    });

    return languages;
})();
