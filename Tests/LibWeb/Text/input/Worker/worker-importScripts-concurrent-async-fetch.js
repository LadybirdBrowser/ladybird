self.onmessage = event => {
    const { fetchURL, importURL } = event.data;
    let importScriptsReturned = false;
    let fetchReactionsRanWhileBlocked = false;

    const fetched = fetch(fetchURL)
        .then(response => response.text())
        .then(text => {
            fetchReactionsRanWhileBlocked = !importScriptsReturned;
            return text;
        });

    importScripts(importURL);
    importScriptsReturned = true;

    fetched.then(
        text => {
            self.postMessage([
                `The imported script ran: ${self.importedScriptRan === true}`,
                `The fetch() completed after importScripts() returned, with body: ${text}`,
                `The fetch() promise reactions ran while importScripts() was blocked: ${fetchReactionsRanWhileBlocked}`,
            ]);
        },
        error => self.postMessage([`The fetch() failed: ${error.name}`])
    );
};
