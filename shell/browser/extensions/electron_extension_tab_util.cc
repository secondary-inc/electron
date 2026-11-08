// Copyright (c) 2026 Anthropic PBC
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/electron_extension_tab_util.h"

#include "content/public/browser/web_contents.h"
#include "content/public/browser/browser_context.h"
#include "base/supports_user_data.h"
#include "shell/browser/api/electron_api_web_contents.h"

namespace extensions {
namespace {
const char kManagedBrowserContext[] = "iris-managed-browser-context";
const char kBrowserTabState[] = "iris-browser-tab-state";
struct BrowserTabState : public base::SupportsUserData::Data {
  bool active = false;
  bool pinned = false;
  int window_id = -1;
  int index = -1;
};
BrowserTabState* State(content::WebContents* contents) {
  return static_cast<BrowserTabState*>(contents->GetUserData(kBrowserTabState));
}
}  // namespace

void SetElectronBrowserTabState(content::WebContents* contents, bool active,
                                int window_id, int index, bool pinned) {
  auto state = std::make_unique<BrowserTabState>();
  state->active = active;
  state->pinned = pinned;
  state->window_id = window_id;
  state->index = index;
  contents->SetUserData(kBrowserTabState, std::move(state));
  contents->GetBrowserContext()->SetUserData(
      kManagedBrowserContext, std::make_unique<BrowserTabState>());
}
bool IsElectronBrowserTabPinned(content::WebContents* contents) {
  auto* state = State(contents);
  return state && state->pinned;
}
bool IsElectronBrowserTab(content::WebContents* contents) {
  return State(contents) ||
      !contents->GetBrowserContext()->GetUserData(kManagedBrowserContext);
}
bool IsElectronBrowserTabActive(electron::api::WebContents* contents) {
  auto* state = State(contents->web_contents());
  return state ? state->active : contents->IsFocused();
}
int ElectronBrowserTabWindowId(content::WebContents* contents) {
  auto* state = State(contents);
  return state ? state->window_id : -1;
}
int ElectronBrowserTabIndex(content::WebContents* contents) {
  auto* state = State(contents);
  return state ? state->index : -1;
}

electron::api::WebContents* GetElectronTabById(
    int tab_id,
    content::BrowserContext* browser_context) {
  auto* contents = electron::api::WebContents::FromID(tab_id);
  if (!contents || !contents->web_contents())
    return nullptr;
  if (contents->web_contents()->GetBrowserContext() != browser_context ||
      !IsElectronBrowserTab(contents->web_contents()))
    return nullptr;
  return contents;
}

}  // namespace extensions
