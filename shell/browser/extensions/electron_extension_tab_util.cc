// Copyright (c) 2026 Anthropic PBC
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/electron_extension_tab_util.h"

#include "content/public/browser/web_contents.h"
#include "content/public/browser/browser_context.h"
#include "base/supports_user_data.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/extension_web_contents_observer.h"
#include "extensions/common/mojom/frame.mojom.h"
#include "shell/browser/api/electron_api_web_contents.h"

namespace extensions {
namespace {
const char kManagedBrowserContext[] = "iris-managed-browser-context";
const char kBrowserTabState[] = "iris-browser-tab-state";
const char kBrowserTabFrames[] = "iris-browser-tab-frames";
// Installed once when the embedder registers a browser tab. Frame identity must
// match the ID used by tab-specific extension permissions, not keyboard focus.
class BrowserTabFrames : public base::SupportsUserData::Data,
                         public content::WebContentsObserver {
 public:
  explicit BrowserTabFrames(content::WebContents* contents)
      : content::WebContentsObserver(contents) {
    contents->ForEachRenderFrameHost([this](content::RenderFrameHost* frame) {
      SetTabId(frame);
    });
  }
  void RenderFrameCreated(content::RenderFrameHost* frame) override {
    SetTabId(frame);
  }
 private:
  void SetTabId(content::RenderFrameHost* frame) {
    if (!frame->IsRenderFrameLive()) return;
    auto* observer = ExtensionWebContentsObserver::GetForWebContents(web_contents());
    auto* tab = electron::api::WebContents::From(web_contents());
    if (observer && tab) {
      if (auto* local = observer->GetLocalFrame(frame))
        local->SetTabId(tab->ID());
    }
  }
};
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
  if (!contents->GetUserData(kBrowserTabFrames))
    contents->SetUserData(kBrowserTabFrames, std::make_unique<BrowserTabFrames>(contents));
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
