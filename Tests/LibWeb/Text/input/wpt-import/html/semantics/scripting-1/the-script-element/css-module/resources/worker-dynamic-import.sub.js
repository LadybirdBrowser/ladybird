import("./record-fetch.py?key={{GET[key]}}&action=incCount", { with: { type: "css" } })
  .then(() => postMessage("LOADED"))
  .catch(e => postMessage("NOT LOADED"));