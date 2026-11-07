/* global chrome */

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.method === 'query') {
    chrome.tabs.query(request.query || {}).then(tabs => sendResponse(tabs));
  } else {
    chrome.tabs.get(sender.tab.id).then(sendResponse);
  }
  return true;
});
