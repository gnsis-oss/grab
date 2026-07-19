// background.js — grab browser bridge
//
// Sends flat tab-change frames to the grab native-messaging host, which
// forwards them to the unix socket the browser examples (event_logger,
// browser_event_screenshot) listen on.
//
// IMPORTANT: the grab C++ bridge accepts only FLAT JSON objects (scalar
// fields, no nested objects). connectNative frames each postMessage with the
// 4-byte little-endian length header the host expects.

const HOST_NAME = "com.eventgrab.bridge";

let port = null;

function connectToHost() {
  try {
    port = chrome.runtime.connectNative(HOST_NAME);
    port.onDisconnect.addListener(() => {
      port = null;
      setTimeout(connectToHost, 5000); // host/socket gone — retry
    });
  } catch (e) {
    console.error("grab-bridge: connect failed", e);
  }
}

// Flat frame — no nested "data" object (the grab bridge rejects nesting).
function sendTab(tab) {
  if (!port || !tab) return;
  try {
    port.postMessage({
      type: "app.tab_changed",
      app: "browser",
      title: tab.title || "",
      url: tab.url || "",
      tab_id: String(tab.id ?? ""),
      timestamp: Date.now() / 1000,
    });
  } catch (e) {
    console.error("grab-bridge: send failed", e);
  }
}

chrome.tabs.onActivated.addListener((info) => {
  chrome.tabs.get(info.tabId, (tab) => {
    if (!chrome.runtime.lastError) sendTab(tab);
  });
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
  if (changeInfo.status === "complete" || changeInfo.title) sendTab(tab);
});

chrome.windows.onFocusChanged.addListener((windowId) => {
  if (windowId === chrome.windows.WINDOW_ID_NONE) return;
  chrome.tabs.query({ active: true, windowId }, (tabs) => {
    if (tabs[0]) sendTab(tabs[0]);
  });
});

connectToHost();
