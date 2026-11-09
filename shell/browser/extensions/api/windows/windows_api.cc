// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#include "shell/browser/extensions/api/windows/windows_api.h"
#include <algorithm>
#include <map>
#include <vector>
#include "base/strings/utf_string_conversions.h"
#include "base/no_destructor.h"
#include "base/memory/raw_ptr.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "extensions/common/extension.h"
#include "extensions/common/permissions/permissions_data.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/browser/native_window.h"
#include "shell/browser/window_list.h"
#include "shell/common/extensions/api/tabs.h"
namespace extensions {
namespace {
using Contents = electron::api::WebContents;
struct Window {
  raw_ptr<electron::NativeWindow> native = nullptr;
  std::vector<Contents*> tabs;
};
using Windows = std::map<int, Window>;
Windows ProfileWindows(content::BrowserContext* context) {
  Windows windows;
  for (auto* contents : Contents::GetWebContentsList()) {
    auto* wc = contents->web_contents();
    if (!wc || wc->GetBrowserContext() != context || !IsElectronBrowserTab(wc)) continue;
    const int id = ElectronBrowserTabWindowId(wc);
    auto* owner = contents->owner_window();
    // The host's managed identity must agree with the actual native owner.
    if (id < 0 || !owner || owner->IsClosed() || owner->base_window_id() != id) continue;
    auto& window = windows[id];
    window.native = owner;
    window.tabs.push_back(contents);
  }
  for (auto& [id, window] : windows)
    std::stable_sort(window.tabs.begin(), window.tabs.end(), [](Contents* a, Contents* b) {
      return ElectronBrowserTabIndex(a->web_contents()) < ElectronBrowserTabIndex(b->web_contents());
    });
  return windows;
}
bool MatchType(const base::DictValue& query) {
  const auto* types = query.FindList("windowTypes");
  if (!types) return true;
  for (const auto& type : *types)
    if (type.is_string() && type.GetString() == "normal") return true;
  return false;
}
int CurrentWindow(const Windows& windows, content::BrowserContext* context) {
  int focused = -1;
  for (const auto& [id, window] : windows) {
    if (!window.native->IsFocused()) continue;
    if (focused >= 0) return -1;
    focused = id;
  }
  if (focused >= 0) return focused;
  auto* selected = GetActiveElectronTab(context);
  if (!selected) return -1;
  const int id = ElectronBrowserTabWindowId(selected->web_contents());
  return windows.contains(id) ? id : -1;
}
base::DictValue Describe(int id, const Window& window, bool populate,
                         const Extension* extension) {
  auto* native = window.native.get();
  const auto bounds = native->GetBounds();
  base::DictValue result;
  result.Set("id", id); result.Set("type", "normal");
  result.Set("focused", native->IsFocused());
  result.Set("incognito", window.tabs.front()->web_contents()->GetBrowserContext()->IsOffTheRecord());
  result.Set("alwaysOnTop", native->GetZOrderLevel() != ui::ZOrderLevel::kNormal);
  result.Set("left", bounds.x()); result.Set("top", bounds.y());
  result.Set("width", bounds.width()); result.Set("height", bounds.height());
  result.Set("state", native->IsFullscreen() ? "fullscreen" : native->IsMinimized() ? "minimized" : native->IsMaximized() ? "maximized" : "normal");
  if (populate) {
    base::ListValue tabs;
    for (auto* contents : window.tabs) {
      auto* wc = contents->web_contents();
      api::tabs::Tab tab;
      tab.id = contents->ID(); tab.window_id = id; tab.index = ElectronBrowserTabIndex(wc);
      tab.active = IsElectronBrowserTabActive(contents); tab.selected = tab.active; tab.highlighted = tab.active;
      if (const int opener = ElectronBrowserTabOpener(wc); opener >= 0) tab.opener_tab_id = opener;
      tab.pinned = IsElectronBrowserTabPinned(wc); tab.incognito = wc->GetBrowserContext()->IsOffTheRecord();
      tab.group_id = -1; tab.split_view_id = -1;
      tab.status = wc->IsLoading() ? api::tabs::TabStatus::kLoading : api::tabs::TabStatus::kComplete;
      tab.audible = wc->IsCurrentlyAudible();
      tab.muted_info = api::tabs::MutedInfo();
      tab.muted_info->muted = wc->IsAudioMuted();
      const auto& url = wc->GetLastCommittedURL();
      if (extension->permissions_data()->HasAPIPermissionForTab(contents->ID(), mojom::APIPermissionID::kTab) ||
          extension->permissions_data()->HasHostPermission(url)) {
        tab.url = url.spec(); tab.title = base::UTF16ToUTF8(wc->GetTitle());
        if (auto icon = ElectronBrowserTabFavicon(wc); !icon.empty()) tab.fav_icon_url = std::move(icon);
      }
      tabs.Append(tab.ToValue());
    }
    result.Set("tabs", std::move(tabs));
  }
  return result;
}
const base::DictValue& Query(const base::ListValue& args, size_t index) {
  static const base::NoDestructor<base::DictValue> empty;
  return args.size() > index && args[index].is_dict() ? args[index].GetDict() : *empty;
}
}
ExtensionFunction::ResponseAction WindowsGetAllFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().empty() || args()[0].is_none() || args()[0].is_dict());
  const auto& query = Query(args(), 0);
  base::ListValue result;
  if (MatchType(query))
    for (const auto& [id, window] : ProfileWindows(browser_context()))
      result.Append(Describe(id, window, query.FindBool("populate").value_or(false), extension()));
  return RespondNow(WithArguments(std::move(result)));
}
ExtensionFunction::ResponseAction WindowsGetFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_int());
  EXTENSION_FUNCTION_VALIDATE(args().size() < 2 || args()[1].is_none() || args()[1].is_dict());
  const auto windows = ProfileWindows(browser_context());
  int id = args()[0].GetInt();
  if (id == -2) id = CurrentWindow(windows, browser_context());
  const auto found = windows.find(id);
  const auto& query = Query(args(), 1);
  if (found == windows.end() || !MatchType(query)) return RespondNow(Error("No such browser window in this profile"));
  return RespondNow(WithArguments(Describe(id, found->second, query.FindBool("populate").value_or(false), extension())));
}
ExtensionFunction::ResponseAction WindowsGetLastFocusedFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().empty() || args()[0].is_none() || args()[0].is_dict());
  const auto windows = ProfileWindows(browser_context());
  const auto& query = Query(args(), 0);
  int id = -1;
  uint64_t latest = 0;
  if (MatchType(query)) {
    for (const auto& [candidate, window] : windows) {
      const auto order = electron::WindowList::GetLastFocusOrder(window.native);
      if (order > latest) { latest = order; id = candidate; }
    }
    // A hidden fixture or newly restored shell may never have received OS focus.
    // Use the unique selected managed window only in that initial state.
    if (id < 0) id = windows.size() == 1 ? windows.begin()->first : CurrentWindow(windows, browser_context());
  }
  const auto found = windows.find(id);
  if (found == windows.end()) return RespondNow(Error("No unambiguous last-focused browser window in this profile"));
  return RespondNow(WithArguments(Describe(id, found->second, query.FindBool("populate").value_or(false), extension())));
}
ExtensionFunction::ResponseAction WindowsGetCurrentFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().empty() || args()[0].is_none() || args()[0].is_dict());
  const auto windows = ProfileWindows(browser_context());
  const int id = CurrentWindow(windows, browser_context());
  const auto found = windows.find(id);
  const auto& query = Query(args(), 0);
  if (found == windows.end() || !MatchType(query)) return RespondNow(Error("No unambiguous current browser window in this profile"));
  return RespondNow(WithArguments(Describe(id, found->second, query.FindBool("populate").value_or(false), extension())));
}
}
