// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#include "shell/browser/extensions/api/web_navigation/web_navigation_api.h"
#include <map>
#include <string>
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_api_frame_id_map.h"
#include "extensions/common/mojom/event_dispatcher.mojom.h"
#include "net/base/net_errors.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "ui/base/page_transition_types.h"
namespace extensions {
namespace {
const char kObserver[] = "iris-web-navigation";
using Frames = ExtensionApiFrameIdMap;
bool Managed(content::WebContents* wc) {
  return wc && IsElectronBrowserTab(wc) && ElectronBrowserTabWindowId(wc) >= 0;
}
bool VisibleURL(const GURL& url) {
  return url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile() ||
      url.SchemeIs("data") || url.SchemeIs("filesystem") ||
      url.IsAboutBlank() || url.IsAboutSrcdoc();
}
base::DictValue FrameDetails(content::RenderFrameHost* frame) {
  base::DictValue value;
  value.Set("url", frame->GetLastCommittedURL().spec());
  value.Set("frameId", Frames::GetFrameId(frame));
  value.Set("parentFrameId", Frames::GetParentFrameId(frame));
  value.Set("processId", frame->GetProcess()->GetDeprecatedID());
  value.Set("documentId", Frames::GetDocumentId(frame).ToString());
  value.Set("frameType", ToString(Frames::GetFrameType(frame)));
  value.Set("documentLifecycle", ToString(Frames::GetDocumentLifecycle(frame)));
  if (auto* parent = frame->GetParentOrOuterDocument())
    value.Set("parentDocumentId", Frames::GetDocumentId(parent).ToString());
  return value;
}
class NavigationObserver : public base::SupportsUserData::Data,
                           public content::WebContentsObserver {
 public:
  explicit NavigationObserver(content::WebContents* wc) : content::WebContentsObserver(wc) {
    wc->ForEachRenderFrameHost([this](content::RenderFrameHost* frame) {
      urls_[frame->GetGlobalId()] = frame->GetLastCommittedURL();
    });
  }
  void RenderFrameDeleted(content::RenderFrameHost* frame) override {
    urls_.erase(frame->GetGlobalId());
  }
  void DidFinishNavigation(content::NavigationHandle* nav) override {
    if (!Managed(web_contents()) || !VisibleURL(nav->GetURL())) return;
    base::DictValue details;
    events::HistogramValue histogram = events::WEB_NAVIGATION_ON_COMMITTED;
    std::string name = "webNavigation.onCommitted";
    if (!nav->HasCommitted() || nav->IsErrorPage()) {
      name = "webNavigation.onErrorOccurred";
      histogram = events::WEB_NAVIGATION_ON_ERROR_OCCURRED;
      details.Set("processId", -1);
      details.Set("frameId", Frames::GetFrameId(nav));
      details.Set("parentFrameId", Frames::GetParentFrameId(nav));
      details.Set("documentId", Frames::GetDocumentId(nav).ToString());
      details.Set("frameType", ToString(Frames::GetFrameType(nav)));
      details.Set("documentLifecycle", ToString(Frames::GetDocumentLifecycle(nav)));
      if (auto* parent = nav->GetParentFrameOrOuterDocument())
        details.Set("parentDocumentId", Frames::GetDocumentId(parent).ToString());
      details.Set("error", net::ErrorToString(nav->GetNetErrorCode() == net::OK ? net::ERR_ABORTED : nav->GetNetErrorCode()));
    } else {
      auto* frame = nav->GetRenderFrameHost();
      if (!frame->IsActive()) return;
      details = FrameDetails(frame);
      const auto id = frame->GetGlobalId();
      const auto previous = urls_.find(id);
      if (nav->IsSameDocument()) {
        const bool fragment = previous != urls_.end() && previous->second != nav->GetURL() && previous->second.EqualsIgnoringRef(nav->GetURL());
        name = fragment ? "webNavigation.onReferenceFragmentUpdated" : "webNavigation.onHistoryStateUpdated";
        histogram = fragment ? events::WEB_NAVIGATION_ON_REFERENCE_FRAGMENT_UPDATED : events::WEB_NAVIGATION_ON_HISTORY_STATE_UPDATED;
      }
      urls_[id] = nav->GetURL();
      auto transition = nav->GetPageTransition();
      details.Set("transitionType", ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_AUTO_TOPLEVEL) ? "start_page" : ui::PageTransitionGetCoreTransitionString(transition));
      base::ListValue qualifiers;
      if (transition & ui::PAGE_TRANSITION_CLIENT_REDIRECT) qualifiers.Append("client_redirect");
      if (nav->WasServerRedirect() || (transition & ui::PAGE_TRANSITION_SERVER_REDIRECT)) qualifiers.Append("server_redirect");
      if (transition & ui::PAGE_TRANSITION_FORWARD_BACK) qualifiers.Append("forward_back");
      if (transition & ui::PAGE_TRANSITION_FROM_ADDRESS_BAR) qualifiers.Append("from_address_bar");
      details.Set("transitionQualifiers", std::move(qualifiers));
    }
    auto* contents = electron::api::WebContents::From(web_contents());
    auto* router = EventRouter::Get(web_contents()->GetBrowserContext());
    if (!contents || !router) return;
    details.Set("tabId", contents->ID());
    details.Set("url", nav->GetURL().spec());
    details.Set("timeStamp", base::Time::Now().InMillisecondsFSinceUnixEpoch());
    base::ListValue args;
    args.Append(std::move(details));
    auto event = std::make_unique<Event>(histogram, name, std::move(args), web_contents()->GetBrowserContext());
    event->filter_info = mojom::EventFilteringInfo::New();
    event->filter_info->url = nav->GetURL();
    router->BroadcastEvent(std::move(event));
  }
 private:
  std::map<content::GlobalRenderFrameHostId, GURL> urls_;
};
bool Queryable(content::RenderFrameHost* frame) {
  return frame->IsActive() && VisibleURL(frame->GetLastCommittedURL());
}
}
void ObserveElectronWebNavigation(content::WebContents* contents) {
  if (!contents->GetUserData(kObserver))
    contents->SetUserData(kObserver, std::make_unique<NavigationObserver>(contents));
}
ExtensionFunction::ResponseAction WebNavigationGetAllFramesFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().size() == 1 && args()[0].is_dict());
  auto tab_id = args()[0].GetDict().FindInt("tabId");
  EXTENSION_FUNCTION_VALIDATE(tab_id && *tab_id >= 0);
  auto* contents = GetElectronTabById(*tab_id, browser_context());
  if (!contents || !Managed(contents->web_contents())) return RespondNow(WithArguments(base::Value()));
  base::ListValue result;
  contents->web_contents()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
    if (!Queryable(frame)) return;
    auto details = FrameDetails(frame);
    details.Set("errorOccurred", frame->IsErrorDocument());
    result.Append(std::move(details));
  });
  return RespondNow(WithArguments(std::move(result)));
}
ExtensionFunction::ResponseAction WebNavigationGetFrameFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().size() == 1 && args()[0].is_dict());
  const auto& query = args()[0].GetDict();
  auto tab_id = query.FindInt("tabId"), frame_id = query.FindInt("frameId");
  const auto* document_id = query.FindString("documentId");
  if (!document_id && (!tab_id || !frame_id))
    return RespondNow(Error("Specify documentId or tabId and frameId"));
  base::Value result;
  for (auto* contents : electron::api::WebContents::GetWebContentsList()) {
    auto* wc = contents->web_contents();
    if (!Managed(wc) || wc->GetBrowserContext() != browser_context() || (tab_id && contents->ID() != *tab_id)) continue;
    wc->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (!Queryable(frame) || (frame_id && Frames::GetFrameId(frame) != *frame_id) ||
          (document_id && Frames::GetDocumentId(frame).ToString() != *document_id)) return;
      auto details = FrameDetails(frame);
      details.Set("errorOccurred", frame->IsErrorDocument());
      result = base::Value(std::move(details));
    });
  }
  return RespondNow(WithArguments(std::move(result)));
}
}
