// Copyright (c) 2026 Anthropic PBC
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/browser/extensions/api/web_navigation/web_navigation_api.h"

#include "content/public/browser/web_contents.h"
#include "content/public/browser/browser_context.h"
#include "base/supports_user_data.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/navigation_handle.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/permissions/permissions_data.h"
#include "shell/common/extensions/api/tabs.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/extension_web_contents_observer.h"
#include "extensions/browser/event_router.h"
#include "extensions/common/mojom/frame.mojom.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/web_contents_zoom_observer.h"
#include "shell/browser/native_window.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/public/mojom/favicon/favicon_url.mojom.h"

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
    last_url_ = contents->GetLastCommittedURL().spec();
    last_title_ = base::UTF16ToUTF8(contents->GetTitle());
    loading_ = contents->IsLoading();
    muted_ = contents->IsAudioMuted();
    audible_ = contents->IsCurrentlyAudible();
    favicon_ = last_favicon_ = PickFavicon(contents->GetFaviconURLs());
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
  void WebContentsDestroyed() override {
    auto* wc = web_contents();
    if (!wc || tab_id_ < 0 || window_id_ < 0) return;
    auto* context = wc->GetBrowserContext();
    auto* router = EventRouter::Get(context);
    if (!router) return;
    base::DictValue info;
    info.Set("windowId", window_id_);
    info.Set("isWindowClosing", had_owner_ && (!owner_ || owner_->IsClosed()));
    base::ListValue args;
    args.Append(tab_id_);
    args.Append(std::move(info));
    tab_id_ = -1;
    router->BroadcastEvent(std::make_unique<Event>(
        events::TABS_ON_REMOVED, "tabs.onRemoved", std::move(args), context));
  }
  const std::string& favicon() const { return favicon_; }
  void DidStartLoading() override { Updated(); }
  void DidStopLoading() override { Updated(); }
  void TitleWasSet(content::NavigationEntry*) override { Updated(); }
  void DidUpdateAudioMutingState(bool) override { Updated(); }
  void OnAudioStateChanged(bool) override { Updated(); }
  void DidUpdateFaviconURL(content::RenderFrameHost* frame,
      const std::vector<blink::mojom::FaviconURLPtr>& candidates) override {
    if (frame != web_contents()->GetPrimaryMainFrame()) return;
    favicon_ = PickFavicon(candidates);
    Updated();
  }
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (!navigation->IsInPrimaryMainFrame() || !navigation->HasCommitted()) return;
    if (!navigation->IsSameDocument()) favicon_.clear();
    Updated();
  }
  void Updated(bool created = false) {
    auto* wc = web_contents();
    if (!wc || !wc->GetUserData(kBrowserTabState)) return;
    auto* contents = electron::api::WebContents::From(wc);
    if (!contents) return;
    tab_id_ = contents->ID();
    window_id_ = ElectronBrowserTabWindowId(wc);
    if (auto* owner = contents->owner_window()) {
      owner_ = owner->GetWeakPtr();
      had_owner_ = true;
    }
    const auto url = wc->GetLastCommittedURL();
    const std::string title = base::UTF16ToUTF8(wc->GetTitle());
    const bool loading = wc->IsLoading(), muted = wc->IsAudioMuted();
    const bool audible = wc->IsCurrentlyAudible(), pinned = IsElectronBrowserTabPinned(wc);
    base::DictValue changes;
    if (url.spec() != last_url_) changes.Set("url", url.spec());
    if (favicon_ != last_favicon_) changes.Set("favIconUrl", favicon_);
    last_favicon_ = favicon_;
    if (!loading && title != last_title_) changes.Set("title", title);
    if (loading != loading_) changes.Set("status", loading ? "loading" : "complete");
    if (pinned != pinned_) changes.Set("pinned", pinned);
    if (audible != audible_) changes.Set("audible", audible);
    if (muted != muted_) {
      base::DictValue info;
      info.Set("muted", muted);
      changes.Set("mutedInfo", std::move(info));
    }
    last_url_ = url.spec();
    if (!loading) last_title_ = title;
    loading_ = loading; muted_ = muted; audible_ = audible; pinned_ = pinned;
    if (!created && changes.empty()) return;
    auto* context = wc->GetBrowserContext();
    auto* router = EventRouter::Get(context);
    auto* registry = ExtensionRegistry::Get(context);
    if (!router || !registry) return;
    for (const auto& extension : registry->enabled_extensions()) {
      const char* event_name = created ? "tabs.onCreated" : "tabs.onUpdated";
      if (!router->ExtensionHasEventListener(extension->id(), event_name)) continue;
      const auto* permissions = extension->permissions_data();
      // This observer can run before ActiveTabPermissionGranter revokes a
      // previous origin. A tab-scoped API grant alone is not sufficient.
      const bool privileged = permissions->HasAPIPermission(mojom::APIPermissionID::kTab) ||
          permissions->HasHostPermission(url) ||
          (permissions->HasAPIPermissionForTab(contents->ID(), mojom::APIPermissionID::kTab) &&
           permissions->GetPageAccess(url, contents->ID(), nullptr) == PermissionsData::PageAccess::kAllowed);
      auto change = changes.Clone();
      if (!privileged) { change.Remove("url"); change.Remove("title"); change.Remove("favIconUrl"); }
      if (!created && change.empty()) continue;
      api::tabs::Tab tab;
      tab.id = contents->ID();
      tab.window_id = ElectronBrowserTabWindowId(wc);
      tab.index = ElectronBrowserTabIndex(wc);
      tab.active = IsElectronBrowserTabActive(contents);
      tab.selected = tab.active; tab.highlighted = tab.active;
      tab.pinned = pinned;
      if (const int opener = ElectronBrowserTabOpener(wc); opener >= 0) tab.opener_tab_id = opener;
      tab.incognito = context->IsOffTheRecord();
      tab.group_id = -1; tab.split_view_id = -1;
      tab.status = loading ? api::tabs::TabStatus::kLoading : api::tabs::TabStatus::kComplete;
      tab.last_accessed = wc->GetLastActiveTime().InMillisecondsFSinceUnixEpoch();
      tab.audible = audible;
      api::tabs::MutedInfo info; info.muted = muted;
      tab.muted_info = std::move(info);
      if (privileged) {
        tab.url = url.spec();
        if (!favicon_.empty()) tab.fav_icon_url = favicon_;
        // Do not attribute a previous document's title to a new origin while loading.
        if (!loading) tab.title = title;
      }
      base::ListValue args;
      if (!created) {
        args.Append(contents->ID());
        args.Append(std::move(change));
      }
      args.Append(tab.ToValue());
      router->DispatchEventToExtension(extension->id(), std::make_unique<Event>(
          created ? events::TABS_ON_CREATED : events::TABS_ON_UPDATED,
          event_name, std::move(args), context));
    }
  }
  void RenderFrameCreated(content::RenderFrameHost* frame) override {
    SetTabId(frame);
  }
 private:
  static std::string PickFavicon(const std::vector<blink::mojom::FaviconURLPtr>& candidates) {
    for (const auto& icon : candidates)
      if (icon->icon_type == blink::mojom::FaviconIconType::kFavicon && icon->icon_url.is_valid())
        return icon->icon_url.spec();
    return {};
  }
  int tab_id_ = -1, window_id_ = -1;
  bool had_owner_ = false;
  base::WeakPtr<electron::NativeWindow> owner_;
  std::string last_url_, last_title_, favicon_, last_favicon_;
  bool loading_ = false, muted_ = false, audible_ = false, pinned_ = false;
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
  int opener_tab_id = -1;
};
BrowserTabState* State(content::WebContents* contents) {
  return static_cast<BrowserTabState*>(contents->GetUserData(kBrowserTabState));
}
}  // namespace

void SetElectronBrowserTabState(content::WebContents* contents, bool active,
                                int window_id, int index, bool pinned, bool moved, int opener_tab_id) {
  if (!contents->GetUserData(kBrowserTabFrames))
    contents->SetUserData(kBrowserTabFrames, std::make_unique<BrowserTabFrames>(contents));
  const auto* previous = State(contents);
  const bool created = !previous;
  const int from_index = previous ? previous->index : -1;
  const bool notify_move = moved && previous && previous->window_id == window_id && from_index != index;
  const bool activated = active && (!previous || !previous->active || previous->window_id != window_id);
  auto state = std::make_unique<BrowserTabState>();
  state->active = active;
  state->pinned = pinned;
  state->window_id = window_id;
  state->index = index;
  state->opener_tab_id = opener_tab_id == -2 ? (previous ? previous->opener_tab_id : -1) : opener_tab_id;
  contents->SetUserData(kBrowserTabState, std::move(state));
  contents->GetBrowserContext()->SetUserData(
      kManagedBrowserContext, std::make_unique<BrowserTabState>());
  static_cast<BrowserTabFrames*>(contents->GetUserData(kBrowserTabFrames))->Updated(created);
  ObserveElectronWebNavigation(contents);
  if (notify_move && window_id >= 0) {
    auto* tab = electron::api::WebContents::From(contents);
    auto* router = EventRouter::Get(contents->GetBrowserContext());
    if (tab && router) {
      base::DictValue info;
      info.Set("windowId", window_id);
      info.Set("fromIndex", from_index);
      info.Set("toIndex", index);
      base::ListValue args;
      args.Append(tab->ID());
      args.Append(std::move(info));
      router->BroadcastEvent(std::make_unique<Event>(
          events::TABS_ON_MOVED, "tabs.onMoved", std::move(args),
          contents->GetBrowserContext()));
    }
  }
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
std::string ElectronBrowserTabFavicon(content::WebContents* contents) {
  const auto* observer = static_cast<BrowserTabFrames*>(contents->GetUserData(kBrowserTabFrames));
  return observer ? observer->favicon() : std::string();
}
int ElectronBrowserTabOpener(content::WebContents* contents) {
  const auto* state = State(contents);
  if (!state || state->opener_tab_id < 0) return -1;
  auto* opener = GetElectronTabById(state->opener_tab_id, contents->GetBrowserContext());
  return opener && opener->web_contents() != contents &&
      ElectronBrowserTabWindowId(opener->web_contents()) == state->window_id
      ? state->opener_tab_id : -1;
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
