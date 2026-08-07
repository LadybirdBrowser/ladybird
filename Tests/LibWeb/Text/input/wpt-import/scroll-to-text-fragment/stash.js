// Put test results into Stash
function stashResultsThenClose(key, results) {
  fetch(`/scroll-to-text-fragment/stash.py?key=${key}`, {
    method: 'POST',
    body: JSON.stringify(results)
  }).then(() => {
    window.close();
  });
}

// Fetch test results from the Stash
function fetchResults(key, resolve, reject) {
  fetch(`/scroll-to-text-fragment/stash.py?key=${key}`).then(response => {
    return response.text();
  }).then(text => {
    if (text) {
      try {
        const results = JSON.parse(text);
        resolve(results);
      } catch(e) {
        reject();
      }
    } else {
      // We keep trying to fetch results as the target page may not have stashed
      // them yet.
      fetchResults(key, resolve, reject);
    }
  });
}
