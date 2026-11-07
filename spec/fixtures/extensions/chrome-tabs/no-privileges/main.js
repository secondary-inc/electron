/* global chrome */

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  sendResponse(request);
});

window.addEventListener(
  'message',
  (event) => {
    chrome.runtime.sendMessage(JSON.parse(event.data), (response) => {
      console.log(JSON.stringify(response));
    });
  },
  false
);
