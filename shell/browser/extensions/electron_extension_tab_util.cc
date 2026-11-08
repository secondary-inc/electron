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
#include "extensions/browser/event_router.h"
#include "extensions/common/mojom/frame.mojom.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/web_contents_zoom_observer.h"
#include "third_party/blink/public/common/page/page_zoom.h"

namespace extensions {
namespace {
const char kManagedBrowserContext[] = "iris-managed-browser-context";
const char kBrowserTabState[] = "iris-browser-tab-state";
const char kBrowserTabFrames[] = "iris-browser-tab-frames";
// Installed once when the embedder registers a browser tab. Frame identity must
// match the ID used by tab-specific extension permissions, not keyboard focus.
class BrowserTabFrames : public base::SupportsUserData::Data,
                         public content::WebContentsObserver,
                         public electron::WebContentsZoomObserver {
 public:
  explicit BrowserTabFrames(content::WebContents* contents)
      : content::WebContentsObserver(contents) {
    zoom_ = electron::WebContentsZoomController::FromWebContents(contents);
    if (zoom_) {
      zoom_level_ = zoom_->GetZoomLevel();
      zoom_mode_ = zoom_->zoom_mode();
      zoom_->AddObserver(this);
    }
    contents->ForEachRenderFrameHost([this](content::RenderFrameHost* frame) {
      SetTabId(frame);
    });
  }
  ~BrowserTabFrames() override {
    if (zoom_) zoom_->RemoveObserver(this);
  }
  void OnZoomControllerDestroyed(electron::WebContentsZoomController* controller) override {
    controller->RemoveObserver(this);
    zoom_ = nullptr;
  }
  void OnZoomChanged(const electron::WebContentsZoomController::ZoomChangedEventData& data) override {
    if (!web_contents() || !web_contents()->GetUserData(kBrowserTabState)) return;
    if (blink::ZoomValuesEqual(zoom_level_, data.new_zoom_level) && zoom_mode_ == data.zoom_mode) return;
    const double old_level = zoom_level_;
    zoom_level_ = data.new_zoom_level;
    zoom_mode_ = data.zoom_mode;
    auto* tab = electron::api::WebContents::From(web_contents());
    auto* router = EventRouter::Get(web_contents()->GetBrowserContext());
    if (!tab || !router) return;
    using Controller = electron::WebContentsZoomController;
    base::DictValue settings;
    settings.Set("mode", zoom_mode_ == Controller::ZOOM_MODE_MANUAL ? "manual" :
        zoom_mode_ == Controller::ZOOM_MODE_DISABLED ? "disabled" : "automatic");
    settings.Set("scope", zoom_mode_ == Controller::ZOOM_MODE_DEFAULT ? "per-origin" : "per-tab");
    base::DictValue info;
    info.Set("tabId", tab->ID());
    info.Set("oldZoomFactor", blink::ZoomLevelToZoomFactor(old_level));
    info.Set("newZoomFactor", blink::ZoomLevelToZoomFactor(zoom_level_));
    info.Set("zoomSettings", std::move(settings));
    base::ListValue args;
    args.Append(std::move(info));
    router->BroadcastEvent(std::make_unique<Event>(
        events::TABS_ON_ZOOM_CHANGE, "tabs.onZoomChange", std::move(args),
        web_contents()->GetBrowserContext()));
  }
  void RenderFrameCreated(content::RenderFrameHost* frame) override {
    SetTabId(frame);
  }
 private:
  raw_ptr<electron::WebContentsZoomController> zoom_ = nullptr;
  double zoom_level_ = 0;
  electron::WebContentsZoomController::ZoomMode zoom_mode_ = electron::WebContentsZoomController::ZOOM_MODE_DEFAULT;
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
  const auto* previous = State(contents);
  const bool activated = active && (!previous || !previous->active || previous->window_id != window_id);
  auto state = std::make_unique<BrowserTabState>();
  state->active = active;
  state->pinned = pinned;
  state->window_id = window_id;
  state->index = index;
  contents->SetUserData(kBrowserTabState, std::move(state));
  contents->GetBrowserContext()->SetUserData(
      kManagedBrowserContext, std::make_unique<BrowserTabState>());
  if (activated && window_id >= 0) {
    auto* tab = electron::api::WebContents::From(contents);
    auto* router = EventRouter::Get(contents->GetBrowserContext());
    if (tab && router) {
      base::DictValue info;
      info.Set("tabId", tab->ID());
      info.Set("windowId", window_id);
      base::ListValue args;
      args.Append(std::move(info));
      router->BroadcastEvent(std::make_unique<Event>(
          events::TABS_ON_ACTIVATED, "tabs.onActivated", std::move(args),
          contents->GetBrowserContext()));
    }
  }
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

electron::api::WebContents* GetActiveElectronTab(content::BrowserContext* context) {
  electron::api::WebContents* selected = nullptr;
  for (auto* tab : electron::api::WebContents::GetWebContentsList()) {
    if (!tab || !tab->web_contents() ||
        tab->web_contents()->GetBrowserContext() != context ||
        !IsElectronBrowserTab(tab->web_contents()) ||
        !IsElectronBrowserTabActive(tab)) continue;
    if (selected) return nullptr;
    selected = tab;
  }
  return selected;
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
