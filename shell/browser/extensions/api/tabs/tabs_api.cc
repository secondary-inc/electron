// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/api/tabs/tabs_api.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "extensions/browser/extension_registry.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "base/containers/fixed_flat_set.h"
#include "base/strings/pattern.h"
#include "base/types/expected_macros.h"
#include "chrome/common/url_constants.h"
#include "components/url_formatter/url_fixer.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/browser_context.h"
#include "extensions/browser/extension_api_frame_id_map.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/common/error_utils.h"
#include "extensions/common/extension_features.h"
#include "extensions/common/feature_switch.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/common/mojom/host_id.mojom.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/common/switches.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/browser/native_window.h"
#include "shell/browser/web_contents_zoom_controller.h"
#include "shell/browser/window_list.h"
#include "shell/common/extensions/api/tabs.h"
#include "third_party/blink/public/common/chrome_debug_urls.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "url/gurl.h"

using electron::WebContentsZoomController;

namespace extensions {

namespace tabs = api::tabs;

constexpr std::string_view kFrameNotFoundError = "No frame with id * in tab *.";
const char kPerOriginOnlyInAutomaticError[] =
    "Can only set scope to "
    "\"per-origin\" in \"automatic\" mode.";

using api::extension_types::InjectDetails;

namespace {
void ZoomModeToZoomSettings(WebContentsZoomController::ZoomMode zoom_mode,
                            api::tabs::ZoomSettings* zoom_settings) {
  DCHECK(zoom_settings);
  switch (zoom_mode) {
    case WebContentsZoomController::ZOOM_MODE_DEFAULT:
      zoom_settings->mode = api::tabs::ZoomSettingsMode::kAutomatic;
      zoom_settings->scope = api::tabs::ZoomSettingsScope::kPerOrigin;
      break;
    case WebContentsZoomController::ZOOM_MODE_ISOLATED:
      zoom_settings->mode = api::tabs::ZoomSettingsMode::kAutomatic;
      zoom_settings->scope = api::tabs::ZoomSettingsScope::kPerTab;
      break;
    case WebContentsZoomController::ZOOM_MODE_MANUAL:
      zoom_settings->mode = api::tabs::ZoomSettingsMode::kManual;
      zoom_settings->scope = api::tabs::ZoomSettingsScope::kPerTab;
      break;
    case WebContentsZoomController::ZOOM_MODE_DISABLED:
      zoom_settings->mode = api::tabs::ZoomSettingsMode::kDisabled;
      zoom_settings->scope = api::tabs::ZoomSettingsScope::kPerTab;
      break;
  }
}

// Returns true if either |boolean| is disengaged, or if |boolean| and
// |value| are equal. This function is used to check if a tab's parameters match
// those of the browser.
bool MatchesBool(const std::optional<bool>& boolean, bool value) {
  return !boolean || *boolean == value;
}

api::tabs::MutedInfo CreateMutedInfo(content::WebContents* contents) {
  DCHECK(contents);
  api::tabs::MutedInfo info;
  info.muted = contents->IsAudioMuted();
  info.reason = api::tabs::MutedInfoReason::kUser;
  return info;
}

// "title" and "url" properties are considered privileged data and can only
// be exposed if the extension has the "tabs" permission or it has access to
// the WebContents's origin.
bool CanAccessPrivilegedTabFields(const Extension* extension,
                                  int tab_id,
                                  const GURL& url) {
  return extension->permissions_data()->HasAPIPermissionForTab(
             tab_id, mojom::APIPermissionID::kTab) ||
         extension->permissions_data()->HasHostPermission(url);
}

}  // namespace

ExecuteCodeInTabFunction::ExecuteCodeInTabFunction() : execute_tab_id_(-1) {}

ExecuteCodeInTabFunction::~ExecuteCodeInTabFunction() = default;

ExecuteCodeFunction::InitResult ExecuteCodeInTabFunction::Init() {
  if (init_result_)
    return init_result_.value();

  if (args().size() < 2)
    return set_init_result(VALIDATION_FAILURE);

  const auto& tab_id_value = args()[0];
  // |tab_id| is optional so it's ok if it's not there.
  int tab_id = -1;
  if (tab_id_value.is_int()) {
    // But if it is present, it needs to be non-negative.
    tab_id = tab_id_value.GetInt();
    if (tab_id < 0) {
      return set_init_result(VALIDATION_FAILURE);
    }
  }

  // |details| are not optional.
  const base::Value& details_value = args()[1];
  if (!details_value.is_dict())
    return set_init_result(VALIDATION_FAILURE);
  auto details = InjectDetails::FromValue(details_value.GetDict());
  if (!details) {
    return set_init_result(VALIDATION_FAILURE);
  }

  if (tab_id == -1) {
    // There's no useful concept of a "default tab" in Electron.
    // TODO(nornagon): we could potentially kick this to an event to allow the
    // app to decide what "default tab" means for them?
    return set_init_result(VALIDATION_FAILURE);
  }

  execute_tab_id_ = tab_id;
  details_ = std::move(details);
  set_host_id(
      mojom::HostID(mojom::HostID::HostType::kExtensions, extension()->id()));
  return set_init_result(SUCCESS);
}

bool ExecuteCodeInTabFunction::CanExecuteScriptOnPage(std::string* error) {
  // If |tab_id| is specified, look for the tab. Otherwise default to selected
  // tab in the current window.
  CHECK_GE(execute_tab_id_, 0);
  auto* contents = GetElectronTabById(execute_tab_id_, browser_context());
  if (!contents) {
    return false;
  }

  int frame_id = details_->frame_id ? *details_->frame_id
                                    : ExtensionApiFrameIdMap::kTopFrameId;
  content::RenderFrameHost* rfh =
      ExtensionApiFrameIdMap::GetRenderFrameHostById(contents->web_contents(),
                                                     frame_id);
  if (!rfh) {
    *error = ErrorUtils::FormatErrorMessage(
        kFrameNotFoundError, base::NumberToString(frame_id),
        base::NumberToString(execute_tab_id_));
    return false;
  }

  // Content scripts declared in manifest.json can access frames at about:-URLs
  // if the extension has permission to access the frame's origin, so also allow
  // programmatic content scripts at about:-URLs for allowed origins.
  GURL effective_document_url(rfh->GetLastCommittedURL());
  bool is_about_url = effective_document_url.SchemeIs(url::kAboutScheme);
  if (is_about_url && details_->match_about_blank &&
      *details_->match_about_blank) {
    effective_document_url = GURL(rfh->GetLastCommittedOrigin().Serialize());
  }

  if (!effective_document_url.is_valid()) {
    // Unknown URL, e.g. because no load was committed yet. Allow for now, the
    // renderer will check again and fail the injection if needed.
    return true;
  }

  // NOTE: This can give the wrong answer due to race conditions, but it is OK,
  // we check again in the renderer.
  if (!extension()->permissions_data()->CanAccessPage(effective_document_url,
                                                      execute_tab_id_, error)) {
    if (is_about_url &&
        extension()->permissions_data()->active_permissions().HasAPIPermission(
            extensions::mojom::APIPermissionID::kTab)) {
      *error = ErrorUtils::FormatErrorMessage(
          manifest_errors::kCannotAccessAboutUrl,
          rfh->GetLastCommittedURL().spec(),
          rfh->GetLastCommittedOrigin().Serialize());
    }
    return false;
  }

  return true;
}

ScriptExecutor* ExecuteCodeInTabFunction::GetScriptExecutor(
    std::string* error) {
  auto* contents = GetElectronTabById(execute_tab_id_, browser_context());
  if (!contents)
    return nullptr;
  return contents->script_executor();
}

bool ExecuteCodeInTabFunction::IsWebView() const {
  return false;
}

int ExecuteCodeInTabFunction::GetRootFrameId() const {
  return ExtensionApiFrameIdMap::kTopFrameId;
}

const GURL& ExecuteCodeInTabFunction::GetWebViewSrc() const {
  return GURL::EmptyGURL();
}

bool TabsExecuteScriptFunction::ShouldInsertCSS() const {
  return false;
}

bool TabsExecuteScriptFunction::ShouldRemoveCSS() const {
  return false;
}

ExtensionFunction::ResponseAction TabsGoBackFunction::Run() {
  auto params = tabs::GoBack::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  auto* tab = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                            : GetActiveElectronTab(browser_context());
  if (!tab) return RespondNow(Error("No such tab"));
  auto& controller = tab->web_contents()->GetController();
  if (!controller.CanGoBack()) return RespondNow(Error("No previous page in tab history"));
  controller.GoBack();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction TabsGoForwardFunction::Run() {
  auto params = tabs::GoForward::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  auto* tab = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                            : GetActiveElectronTab(browser_context());
  if (!tab) return RespondNow(Error("No such tab"));
  auto& controller = tab->web_contents()->GetController();
  if (!controller.CanGoForward()) return RespondNow(Error("No next page in tab history"));
  controller.GoForward();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction TabsReloadFunction::Run() {
  std::optional<tabs::Reload::Params> params =
      tabs::Reload::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  bool bypass_cache = false;
  if (params->reload_properties && params->reload_properties->bypass_cache) {
    bypass_cache = *params->reload_properties->bypass_cache;
  }

  auto* contents = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                                  : GetActiveElectronTab(browser_context());
  if (!contents)
    return RespondNow(Error("No such tab"));

  contents->web_contents()->GetController().Reload(
      bypass_cache ? content::ReloadType::BYPASSING_CACHE
                   : content::ReloadType::NORMAL,
      true);

  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction TabsQueryFunction::Run() {
  std::optional<tabs::Query::Params> params =
      tabs::Query::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  URLPatternSet url_patterns;
  if (params->query_info.url) {
    std::vector<std::string> url_pattern_strings;
    if (params->query_info.url->as_string)
      url_pattern_strings.push_back(*params->query_info.url->as_string);
    else if (params->query_info.url->as_strings)
      url_pattern_strings.swap(*params->query_info.url->as_strings);
    // It is o.k. to use URLPattern::SCHEME_ALL here because this function does
    // not grant access to the content of the tabs, only to seeing their URLs
    // and meta data.
    std::string error;
    if (!url_patterns.Populate(url_pattern_strings, URLPattern::SCHEME_ALL,
                               true, &error)) {
      return RespondNow(Error(std::move(error)));
    }
  }

  std::string title = params->query_info.title.value_or(std::string());
  std::optional<bool> audible = params->query_info.audible;
  std::optional<bool> muted = params->query_info.muted;

  base::ListValue result;

  // Filter out webContents that don't belong to the current browser context.
  auto* bc = browser_context();
  auto all_contents = electron::api::WebContents::GetWebContentsList();
  all_contents.remove_if([&bc](electron::api::WebContents* wc) {
    return (bc != wc->web_contents()->GetBrowserContext()) ||
           !IsElectronBrowserTab(wc->web_contents());
  });

  for (auto* contents : all_contents) {
    if (!contents || !contents->web_contents())
      continue;

    auto* wc = contents->web_contents();
    const int managed_window_id = ElectronBrowserTabWindowId(wc);
    if (managed_window_id >= 0) {
      if (params->query_info.window_id && *params->query_info.window_id >= 0 &&
          *params->query_info.window_id != managed_window_id)
        continue;
      if (params->query_info.index &&
          *params->query_info.index != ElectronBrowserTabIndex(wc))
        continue;
    }

    if (!MatchesBool(params->query_info.pinned, IsElectronBrowserTabPinned(wc)))
      continue;
    if (!MatchesBool(params->query_info.highlighted, IsElectronBrowserTabActive(contents)))
      continue;
    if (params->query_info.status != tabs::TabStatus::kNone &&
        params->query_info.status != (wc->IsLoading() ? tabs::TabStatus::kLoading :
                                                       tabs::TabStatus::kComplete))
      continue;

    // Match webContents audible value.
    if (!MatchesBool(audible, wc->IsCurrentlyAudible()))
      continue;

    // Match webContents muted value.
    if (!MatchesBool(muted, wc->IsAudioMuted()))
      continue;

    // Match webContents active status.
    if (!MatchesBool(params->query_info.active, IsElectronBrowserTabActive(contents)))
      continue;

    const GURL& committed_url = wc->GetLastCommittedURL();
    const bool has_privileged_access = CanAccessPrivilegedTabFields(
        extension(), contents->ID(), committed_url);

    if (!title.empty() || !url_patterns.is_empty()) {
      // Without privileged access, this tab is considered not matched.
      if (!has_privileged_access)
        continue;

      // Match webContents title.
      if (!title.empty() &&
          !base::MatchPattern(wc->GetTitle(), base::UTF8ToUTF16(title)))
        continue;

      // Match webContents url.
      if (!url_patterns.is_empty() && !url_patterns.MatchesURL(committed_url))
        continue;
    }

    tabs::Tab tab;
    tab.id = contents->ID();
    if (has_privileged_access) {
      tab.title = base::UTF16ToUTF8(wc->GetTitle());
      tab.url = committed_url.spec();
      if (auto icon = ElectronBrowserTabFavicon(wc); !icon.empty()) tab.fav_icon_url = std::move(icon);
    }
    tab.active = IsElectronBrowserTabActive(contents);
    tab.selected = tab.active;
    tab.highlighted = tab.active;
    tab.pinned = IsElectronBrowserTabPinned(wc);
    if (const int opener = ElectronBrowserTabOpener(wc); opener >= 0) tab.opener_tab_id = opener;
    tab.incognito = wc->GetBrowserContext()->IsOffTheRecord();
    tab.group_id = -1;
    tab.status = wc->IsLoading() ? tabs::TabStatus::kLoading : tabs::TabStatus::kComplete;
    tab.last_accessed = wc->GetLastActiveTime().InMillisecondsFSinceUnixEpoch();
    if (const int window_id = ElectronBrowserTabWindowId(wc); window_id >= 0) {
      tab.window_id = window_id;
      tab.index = ElectronBrowserTabIndex(wc);
    }
    tab.audible = contents->IsCurrentlyAudible();
    tab.muted_info = CreateMutedInfo(wc);
    // TODO: Add proper support for split views
    tab.split_view_id = -1;

    result.Append(tab.ToValue());
  }

  return RespondNow(WithArguments(std::move(result)));
}

ExtensionFunction::ResponseAction TabsGetFunction::Run() {
  std::optional<tabs::Get::Params> params = tabs::Get::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  int tab_id = params->tab_id;

  auto* contents = GetElectronTabById(tab_id, browser_context());
  if (!contents)
    return RespondNow(Error("No such tab"));

  tabs::Tab tab;
  tab.id = tab_id;

  auto* wc = contents->web_contents();
  if (CanAccessPrivilegedTabFields(extension(), contents->ID(),
                                   wc->GetLastCommittedURL())) {
    tab.url = wc->GetLastCommittedURL().spec();
    tab.title = base::UTF16ToUTF8(wc->GetTitle());
    if (auto icon = ElectronBrowserTabFavicon(wc); !icon.empty()) tab.fav_icon_url = std::move(icon);
  }

  tab.active = IsElectronBrowserTabActive(contents);
  tab.selected = tab.active;
  tab.highlighted = tab.active;
  tab.pinned = IsElectronBrowserTabPinned(wc);
    if (const int opener = ElectronBrowserTabOpener(wc); opener >= 0) tab.opener_tab_id = opener;
  tab.incognito = wc->GetBrowserContext()->IsOffTheRecord();
  tab.group_id = -1;
  tab.status = wc->IsLoading() ? tabs::TabStatus::kLoading : tabs::TabStatus::kComplete;
  if (const int window_id = ElectronBrowserTabWindowId(wc); window_id >= 0) {
    tab.window_id = window_id;
    tab.index = ElectronBrowserTabIndex(wc);
  }
  tab.last_accessed = wc->GetLastActiveTime().InMillisecondsFSinceUnixEpoch();
  // TODO: Add proper support for split views
  tab.split_view_id = -1;

  return RespondNow(ArgumentList(tabs::Get::Results::Create(std::move(tab))));
}

ExtensionFunction::ResponseAction TabsSetZoomFunction::Run() {
  std::optional<tabs::SetZoom::Params> params =
      tabs::SetZoom::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  auto* contents = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                                  : GetActiveElectronTab(browser_context());
  if (!contents)
    return RespondNow(Error("No such tab"));

  auto* web_contents = contents->web_contents();
  GURL url(web_contents->GetVisibleURL());
  std::string error;
  if (extension()->permissions_data()->IsRestrictedUrl(url, &error))
    return RespondNow(Error(error));

  auto* zoom_controller = contents->GetZoomController();
  double zoom_level = params->zoom_factor > 0
                          ? blink::ZoomFactorToZoomLevel(params->zoom_factor)
                          : zoom_controller->GetDefaultZoomLevel();

  if (!zoom_controller->SetZoomLevel(zoom_level))
    return RespondNow(Error("Cannot zoom this tab in its current state"));

  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction TabsGetZoomFunction::Run() {
  std::optional<tabs::GetZoom::Params> params =
      tabs::GetZoom::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  auto* contents = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                                  : GetActiveElectronTab(browser_context());
  if (!contents)
    return RespondNow(Error("No such tab"));

  const double zoom_factor = contents->GetZoomFactor();
  return RespondNow(ArgumentList(tabs::GetZoom::Results::Create(zoom_factor)));
}

ExtensionFunction::ResponseAction TabsGetZoomSettingsFunction::Run() {
  std::optional<tabs::GetZoomSettings::Params> params =
      tabs::GetZoomSettings::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  auto* contents = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                                  : GetActiveElectronTab(browser_context());
  if (!contents)
    return RespondNow(Error("No such tab"));

  const auto* zoom_controller = contents->GetZoomController();
  const WebContentsZoomController::ZoomMode zoom_mode =
      zoom_controller->zoom_mode();
  tabs::ZoomSettings zoom_settings;
  ZoomModeToZoomSettings(zoom_mode, &zoom_settings);
  zoom_settings.default_zoom_factor =
      blink::ZoomLevelToZoomFactor(zoom_controller->GetDefaultZoomLevel());

  return RespondNow(
      ArgumentList(tabs::GetZoomSettings::Results::Create(zoom_settings)));
}

ExtensionFunction::ResponseAction TabsSetZoomSettingsFunction::Run() {
  using tabs::ZoomSettings;

  std::optional<tabs::SetZoomSettings::Params> params =
      tabs::SetZoomSettings::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);

  auto* contents = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                                  : GetActiveElectronTab(browser_context());
  if (!contents)
    return RespondNow(Error("No such tab"));

  std::string error;
  GURL url(contents->web_contents()->GetVisibleURL());
  if (extension()->permissions_data()->IsRestrictedUrl(url, &error))
    return RespondNow(Error(error));

  // "per-origin" scope is only available in "automatic" mode.
  if (params->zoom_settings.scope == tabs::ZoomSettingsScope::kPerOrigin &&
      params->zoom_settings.mode != tabs::ZoomSettingsMode::kAutomatic &&
      params->zoom_settings.mode != tabs::ZoomSettingsMode::kNone) {
    return RespondNow(Error(kPerOriginOnlyInAutomaticError));
  }

  // Determine the correct internal zoom mode to set |web_contents| to from the
  // user-specified |zoom_settings|.
  WebContentsZoomController::ZoomMode zoom_mode =
      WebContentsZoomController::ZOOM_MODE_DEFAULT;
  switch (params->zoom_settings.mode) {
    case tabs::ZoomSettingsMode::kNone:
    case tabs::ZoomSettingsMode::kAutomatic:
      switch (params->zoom_settings.scope) {
        case tabs::ZoomSettingsScope::kNone:
        case tabs::ZoomSettingsScope::kPerOrigin:
          zoom_mode = WebContentsZoomController::ZOOM_MODE_DEFAULT;
          break;
        case tabs::ZoomSettingsScope::kPerTab:
          zoom_mode = WebContentsZoomController::ZOOM_MODE_ISOLATED;
      }
      break;
    case tabs::ZoomSettingsMode::kManual:
      zoom_mode = WebContentsZoomController::ZOOM_MODE_MANUAL;
      break;
    case tabs::ZoomSettingsMode::kDisabled:
      zoom_mode = WebContentsZoomController::ZOOM_MODE_DISABLED;
  }

  contents->GetZoomController()->SetZoomMode(zoom_mode);

  return RespondNow(NoArguments());
}

namespace {

bool IsKillURL(const GURL& url) {
#if DCHECK_IS_ON()
  // Caller should ensure that |url| is already "fixed up" by
  // url_formatter::FixupURL, which (among many other things) takes care
  // of rewriting about:kill into chrome://kill/.
  if (url.SchemeIs(url::kAboutScheme))
    DCHECK(url.IsAboutBlank() || url.IsAboutSrcdoc());
#endif

  // Disallow common renderer debug URLs.
  // Note: this would also disallow JavaScript URLs, but we already explicitly
  // check for those before calling into here from PrepareURLForNavigation.
  if (blink::IsRendererDebugURL(url)) {
    return true;
  }

  if (!url.SchemeIs(content::kChromeUIScheme)) {
    return false;
  }

  // Also disallow a few more hosts which are not covered by the check above.
  constexpr auto kKillHosts = base::MakeFixedFlatSet<std::string_view>({
      chrome::kChromeUIDelayedHangUIHost,
      chrome::kChromeUIHangUIHost,
      chrome::kChromeUIQuitHost,
      chrome::kChromeUIRestartHost,
      content::kChromeUIBrowserCrashHost,
      content::kChromeUIMemoryExhaustHost,
  });

  return kKillHosts.contains(url.host());
}

GURL ResolvePossiblyRelativeURL(const std::string& url_string,
                                const Extension* extension) {
  GURL url = GURL(url_string);
  if (!url.is_valid() && extension)
    url = extension->ResolveExtensionURL(url_string);

  return url;
}

bool AllowFileAccess(const ExtensionId& extension_id,
                     content::BrowserContext* context) {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
             switches::kDisableExtensionsFileAccessCheck) ||
         ExtensionPrefs::Get(context)->AllowFileAccess(extension_id);
}

base::expected<GURL, std::string> PrepareURLForNavigation(
    const std::string& url_string,
    const Extension* extension,
    content::BrowserContext* browser_context) {
  GURL url = ResolvePossiblyRelativeURL(url_string, extension);

  // Ideally, the URL would only be "fixed" for user input (e.g. for URLs
  // entered into the Omnibox), but some extensions rely on the legacy behavior
  // where all navigations were subject to the "fixing".  See also
  // https://crbug.com/1145381.
  url = url_formatter::FixupURL(url.spec(), "" /* = desired_tld */);

  // Reject invalid URLs.
  if (!url.is_valid()) {
    const char kInvalidUrlError[] = "Invalid url: \"*\".";
    return base::unexpected(
        ErrorUtils::FormatErrorMessage(kInvalidUrlError, url_string));
  }

  // Don't let the extension use JavaScript URLs in API triggered navigations.
  if (url.SchemeIs(url::kJavaScriptScheme)) {
    const char kJavaScriptUrlsNotAllowedInExtensionNavigations[] =
        "JavaScript URLs are not allowed in API based extension navigations. "
        "Use "
        "chrome.scripting.executeScript instead.";
    return base::unexpected(kJavaScriptUrlsNotAllowedInExtensionNavigations);
  }

  // Don't let the extension crash the browser or renderers.
  if (IsKillURL(url)) {
    const char kNoCrashBrowserError[] =
        "I'm sorry. I'm afraid I can't do that.";
    return base::unexpected(kNoCrashBrowserError);
  }

  // Don't let the extension navigate directly to devtools scheme pages, unless
  // they have applicable permissions.
  if (url.SchemeIs(content::kChromeDevToolsScheme)) {
    bool has_permission =
        extension && (extension->permissions_data()->HasAPIPermission(
                          mojom::APIPermissionID::kDevtools) ||
                      extension->permissions_data()->HasAPIPermission(
                          mojom::APIPermissionID::kDebugger));
    if (!has_permission) {
      const char kCannotNavigateToDevtools[] =
          "Cannot navigate to a devtools:// page without either the devtools "
          "or "
          "debugger permission.";
      return base::unexpected(kCannotNavigateToDevtools);
    }
  }

  // Don't let the extension navigate directly to chrome-untrusted scheme pages.
  if (url.SchemeIs(content::kChromeUIUntrustedScheme)) {
    const char kCannotNavigateToChromeUntrusted[] =
        "Cannot navigate to a chrome-untrusted:// page.";
    return base::unexpected(kCannotNavigateToChromeUntrusted);
  }

  // Don't let the extension navigate directly to file scheme pages, unless
  // they have file access.
  if (url.SchemeIsFile() &&
      !AllowFileAccess(extension->id(), browser_context)) {
    const char kFileUrlsNotAllowedInExtensionNavigations[] =
        "Cannot navigate to a file URL without local file access.";
    return base::unexpected(kFileUrlsNotAllowedInExtensionNavigations);
  }

  return url;
}

}  // namespace

ExtensionFunction::ResponseAction TabsMoveFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().size() == 2 && args()[1].is_dict());
  const auto& properties = args()[1].GetDict();
  const auto index = properties.FindInt("index");
  EXTENSION_FUNCTION_VALIDATE(index && *index >= -1);
  const bool multiple = args()[0].is_list();
  std::vector<int> ids;
  if (args()[0].is_int()) ids.push_back(args()[0].GetInt());
  else {
    EXTENSION_FUNCTION_VALIDATE(multiple);
    if (args()[0].GetList().size() > 100)
      return RespondNow(Error("Move between one and 100 browser tabs"));
    for (const auto& value : args()[0].GetList()) {
      EXTENSION_FUNCTION_VALIDATE(value.is_int());
      if (std::find(ids.begin(), ids.end(), value.GetInt()) != ids.end())
        return RespondNow(Error("Duplicate browser tab in move"));
      ids.push_back(value.GetInt());
    }
  }
  if (ids.empty()) return RespondNow(Error("Move between one and 100 browser tabs"));
  auto* selected = GetActiveElectronTab(browser_context());
  if (!selected || ElectronBrowserTabWindowId(selected->web_contents()) < 0)
    return RespondNow(Error("No selected browser tab in this profile"));
  const int window_id = ElectronBrowserTabWindowId(selected->web_contents());
  if (const auto* value = properties.Find("windowId")) {
    EXTENSION_FUNCTION_VALIDATE(value->is_int());
    if (value->GetInt() != -2 && value->GetInt() != window_id)
      return RespondNow(Error("Only the selected browser window is supported"));
  }
  for (int id : ids) {
    auto* target = GetElectronTabById(id, browser_context());
    if (!target || ElectronBrowserTabWindowId(target->web_contents()) != window_id ||
        !target->owner_window() || target->owner_window()->base_window_id() != window_id)
      return RespondNow(Error("No such browser tab in the selected profile window"));
  }
  auto indices = base::MakeRefCounted<base::RefCountedData<std::vector<int>>>();
  auto reply = base::BindRepeating(
      [](scoped_refptr<base::RefCountedData<std::vector<int>>> indices,
         const std::vector<int>& values) { indices->data = values; }, indices);
  if (!selected->Emit("browser-tabs-move", extension()->id(), ids, *index, reply) ||
      indices->data.size() != ids.size())
    return RespondNow(Error("The browser refused to move these tabs"));
  base::ListValue result;
  for (size_t position = 0; position < ids.size(); ++position) {
    const int id = ids[position];
    auto* target = GetElectronTabById(id, browser_context());
    if (!target) return RespondNow(Error("The tab closed during move"));
    auto* wc = target->web_contents();
    tabs::Tab tab;
    tab.id = id;
    tab.window_id = window_id;
    tab.index = indices->data[position];
    tab.active = IsElectronBrowserTabActive(target);
    tab.selected = tab.active;
    tab.highlighted = tab.active;
    tab.pinned = IsElectronBrowserTabPinned(wc);
    if (const int opener = ElectronBrowserTabOpener(wc); opener >= 0) tab.opener_tab_id = opener;
    tab.incognito = browser_context()->IsOffTheRecord();
    tab.group_id = -1;
    tab.split_view_id = -1;
    tab.status = wc->IsLoading() ? tabs::TabStatus::kLoading : tabs::TabStatus::kComplete;
    tab.audible = target->IsCurrentlyAudible();
    tab.muted_info = CreateMutedInfo(wc);
    if (CanAccessPrivilegedTabFields(extension(), id, wc->GetLastCommittedURL())) {
      tab.url = wc->GetLastCommittedURL().spec();
      tab.title = base::UTF16ToUTF8(wc->GetTitle());
      if (auto icon = ElectronBrowserTabFavicon(wc); !icon.empty()) tab.fav_icon_url = std::move(icon);
    }
    result.Append(tab.ToValue());
  }
  if (ids.size() == 1) return RespondNow(WithArguments(std::move(result[0])));
  return RespondNow(WithArguments(std::move(result)));
}

ExtensionFunction::ResponseAction TabsRemoveFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().size() == 1);
  std::vector<int> ids;
  if (args()[0].is_int()) ids.push_back(args()[0].GetInt());
  else {
    EXTENSION_FUNCTION_VALIDATE(args()[0].is_list());
    if (args()[0].GetList().size() > 100)
      return RespondNow(Error("Remove between one and 100 browser tabs"));
    for (const auto& value : args()[0].GetList()) {
      EXTENSION_FUNCTION_VALIDATE(value.is_int());
      if (std::find(ids.begin(), ids.end(), value.GetInt()) == ids.end())
        ids.push_back(value.GetInt());
    }
  }
  if (ids.empty() || ids.size() > 100)
    return RespondNow(Error("Remove between one and 100 browser tabs"));
  auto* selected = GetActiveElectronTab(browser_context());
  if (!selected || ElectronBrowserTabWindowId(selected->web_contents()) < 0)
    return RespondNow(Error("No selected browser tab in this profile"));
  const int window_id = ElectronBrowserTabWindowId(selected->web_contents());
  for (int id : ids) {
    auto* target = GetElectronTabById(id, browser_context());
    if (!target || ElectronBrowserTabWindowId(target->web_contents()) != window_id ||
        !target->owner_window() || target->owner_window()->base_window_id() != window_id)
      return RespondNow(Error("No such browser tab in the selected profile window"));
  }
  using Pending = base::RefCountedData<base::OnceCallback<void(bool)>>;
  auto pending = base::MakeRefCounted<Pending>(base::BindOnce(
      [](scoped_refptr<TabsRemoveFunction> function, std::vector<int> ids, bool closed) {
        auto* context = function->browser_context();
        if (closed && context) {
          for (int id : ids) if (GetElectronTabById(id, context)) closed = false;
        } else closed = false;
        function->Respond(closed ? function->NoArguments()
                                 : function->Error("The browser could not close all requested tabs"));
      }, scoped_refptr<TabsRemoveFunction>(this), ids));
  auto reply = base::BindRepeating([](scoped_refptr<Pending> pending, bool closed) {
    if (pending->data) std::move(pending->data).Run(closed);
  }, pending);
  if (!selected->Emit("browser-tabs-remove", extension()->id(), ids, reply))
    reply.Run(false);
  if (pending->data)
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(reply, false), base::Seconds(15));
  return did_respond() ? AlreadyResponded() : RespondLater();
}

ExtensionFunction::ResponseAction TabsDuplicateFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().size() == 1 && args()[0].is_int());
  const int source_id = args()[0].GetInt();
  auto* selected = GetActiveElectronTab(browser_context());
  auto* source = GetElectronTabById(source_id, browser_context());
  if (!selected || !source || !source->owner_window())
    return RespondNow(Error("A selected managed tab and a source in this profile are required"));
  const int window_id = ElectronBrowserTabWindowId(selected->web_contents());
  if (window_id < 0 || ElectronBrowserTabWindowId(source->web_contents()) != window_id ||
      source->owner_window()->base_window_id() != window_id)
    return RespondNow(Error("Only tabs in the selected browser window can be duplicated"));
  using Pending = base::RefCountedData<base::OnceCallback<void(int)>>;
  auto pending = base::MakeRefCounted<Pending>(base::BindOnce(
      [](scoped_refptr<TabsDuplicateFunction> function, int window_id, int id) {
        auto* context = function->browser_context();
        auto* created = context ? GetElectronTabById(id, context) : nullptr;
        if (!context ||
            ExtensionRegistry::Get(context)->enabled_extensions().GetByID(function->extension_id()) != function->extension() ||
            !created || ElectronBrowserTabWindowId(created->web_contents()) != window_id ||
            !created->owner_window() || created->owner_window()->base_window_id() != window_id) {
          function->Respond(function->Error("The browser refused to duplicate this tab"));
          return;
        }
        function->web_contents_ = created->web_contents();
        function->Respond(function->GetResult());
      }, scoped_refptr<TabsDuplicateFunction>(this), window_id));
  auto reply = base::BindRepeating([](scoped_refptr<Pending> pending, int id) {
    if (pending->data) std::move(pending->data).Run(id);
  }, pending);
  if (!selected->Emit("browser-tab-duplicate", extension()->id(), source_id, reply))
    reply.Run(-1);
  if (pending->data)
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(reply, -1), base::Seconds(15));
  return did_respond() ? AlreadyResponded() : RespondLater();
}

ExtensionFunction::ResponseAction TabsCreateFunction::Run() {
  auto params = tabs::Create::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  const auto& properties = params->create_properties;
  const GURL url(properties.url.value_or("about:blank"));
  if (!url.is_valid() || url.spec().size() > 8192 ||
      (!url.SchemeIsHTTPOrHTTPS() && url.spec() != "about:blank" &&
       !(url.SchemeIs(extensions::kExtensionScheme) && url.host() == extension_id())) ||
      url.has_username() || url.has_password() ||
      (url.SchemeIs(extensions::kExtensionScheme) && url.has_port()))
    return RespondNow(Error("Only web, blank and own-extension tabs are supported"));
  auto* selected = GetActiveElectronTab(browser_context());
  if (!selected || ElectronBrowserTabWindowId(selected->web_contents()) < 0)
    return RespondNow(Error("No selected browser tab in this profile"));
  const int window_id = ElectronBrowserTabWindowId(selected->web_contents());
  if (properties.window_id && *properties.window_id != -2 && *properties.window_id != window_id)
    return RespondNow(Error("Only the selected browser window is supported"));
  const int opener_id = properties.opener_tab_id.value_or(-1);
  if (properties.opener_tab_id) {
    auto* opener = GetElectronTabById(opener_id, browser_context());
    if (!opener || ElectronBrowserTabWindowId(opener->web_contents()) != window_id ||
        !opener->owner_window() || opener->owner_window()->base_window_id() != window_id)
      return RespondNow(Error("No opener tab in the selected browser window"));
  }
  // Closing an extension popup may require beforeunload. Keep one bounded
  // response alive; only the main-process host can acknowledge a registered tab.
  using Pending = base::RefCountedData<base::OnceCallback<void(int)>>;
  auto pending = base::MakeRefCounted<Pending>(base::BindOnce(
      [](scoped_refptr<TabsCreateFunction> function, int window_id, int id) {
        auto* context = function->browser_context();
        auto* created = context ? GetElectronTabById(id, context) : nullptr;
        if (!context ||
            ExtensionRegistry::Get(context)->enabled_extensions().GetByID(function->extension_id()) != function->extension() ||
            !created || ElectronBrowserTabWindowId(created->web_contents()) != window_id ||
            !created->owner_window() || created->owner_window()->base_window_id() != window_id) {
          function->Respond(function->Error("The browser refused to create this tab"));
          return;
        }
        function->web_contents_ = created->web_contents();
        function->Respond(function->GetResult());
      }, scoped_refptr<TabsCreateFunction>(this), window_id));
  auto reply = base::BindRepeating([](scoped_refptr<Pending> pending, int id) {
    if (pending->data) std::move(pending->data).Run(id);
  }, pending);
  if (!selected->Emit("browser-tab-create", extension()->id(), url.spec(),
                 properties.active.value_or(properties.selected.value_or(true)), properties.pinned.value_or(false), properties.index.value_or(-1), reply, opener_id))
    reply.Run(-1);
  if (pending->data)
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(reply, -1), base::Seconds(15));
  return did_respond() ? AlreadyResponded() : RespondLater();
}

TabsUpdateFunction::TabsUpdateFunction() : web_contents_(nullptr) {}

ExtensionFunction::ResponseAction TabsUpdateFunction::Run() {
  std::optional<tabs::Update::Params> params =
      tabs::Update::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  if (params->update_properties.opener_tab_id)
    return RespondNow(Error("Changing an existing tab opener is not supported"));

  auto* contents = params->tab_id ? GetElectronTabById(*params->tab_id, browser_context())
                                  : GetActiveElectronTab(browser_context());
  if (!contents)
    return RespondNow(Error("No such tab"));

  const int tab_id = contents->ID();
  web_contents_ = contents->web_contents();

  if (params->update_properties.active.value_or(false) &&
      ElectronBrowserTabWindowId(web_contents_) >= 0 &&
      !IsElectronBrowserTabActive(contents)) {
    contents->Emit("browser-tab-activate");
    contents = GetElectronTabById(tab_id, browser_context());
    if (!contents)
      return RespondNow(Error("The tab closed during activation"));
    web_contents_ = contents->web_contents();
    if (!IsElectronBrowserTabActive(contents))
      return RespondNow(Error("The browser could not activate this tab"));
  }

  if (params->update_properties.pinned &&
      *params->update_properties.pinned != IsElectronBrowserTabPinned(web_contents_)) {
    contents->Emit("browser-tab-pin", *params->update_properties.pinned);
    contents = GetElectronTabById(tab_id, browser_context());
    if (!contents)
      return RespondNow(Error("The tab closed while changing its pin"));
    web_contents_ = contents->web_contents();
    if (IsElectronBrowserTabPinned(web_contents_) != *params->update_properties.pinned)
      return RespondNow(Error("The browser could not change this tab's pin"));
  }

  // Navigate the tab to a new location if the url is different.
  std::string error;
  if (params->update_properties.url) {
    std::string updated_url = *params->update_properties.url;
    if (!UpdateURL(updated_url, tab_id, &error))
      return RespondNow(Error(std::move(error)));
  }

  if (params->update_properties.muted) {
    contents->SetAudioMuted(*params->update_properties.muted);
  }

  return RespondNow(GetResult());
}

bool TabsUpdateFunction::UpdateURL(const std::string& url_string,
                                   int tab_id,
                                   std::string* error) {
  auto url =
      PrepareURLForNavigation(url_string, extension(), browser_context());
  if (!url.has_value()) {
    *error = std::move(url.error());
    return false;
  }

  const GURL current = web_contents_->GetLastCommittedURL();
  if (current.SchemeIs(extensions::kExtensionScheme) ||
      url->SchemeIs(extensions::kExtensionScheme)) {
    if (!current.SchemeIs(extensions::kExtensionScheme) ||
        !url->SchemeIs(extensions::kExtensionScheme) ||
        current.host() != extension_id() || url->host() != extension_id()) {
      *error = "Extension page navigation must remain in its owning extension tab.";
      return false;
    }
  }

  content::NavigationController::LoadURLParams load_params(*url);

  // Treat extension-initiated navigations as renderer-initiated so that the URL
  // does not show in the omnibox until it commits.  This avoids URL spoofs
  // since URLs can be opened on behalf of untrusted content.
  load_params.is_renderer_initiated = true;
  // All renderer-initiated navigations need to have an initiator origin.
  load_params.initiator_origin = extension()->origin();
  // |source_site_instance| needs to be set so that a renderer process
  // compatible with |initiator_origin| is picked by Site Isolation.
  load_params.source_site_instance = content::SiteInstance::CreateForURL(
      web_contents_->GetBrowserContext(),
      load_params.initiator_origin->GetURL());

  // Marking the navigation as initiated via an API means that the focus
  // will stay in the omnibox - see https://crbug.com/1085779.
  load_params.transition_type = ui::PAGE_TRANSITION_FROM_API;

  base::WeakPtr<content::NavigationHandle> navigation_handle =
      web_contents_->GetController().LoadURLWithParams(load_params);
  // Navigation can fail for any number of reasons at the content layer.
  // Unfortunately, we can't provide a detailed error message here, because
  // there are too many possible triggers. At least notify the extension that
  // the update failed.
  if (!navigation_handle) {
    *error = "Navigation rejected.";
    return false;
  }

  DCHECK_EQ(url,
            web_contents_->GetController().GetPendingEntry()->GetVirtualURL());

  return true;
}

ExtensionFunction::ResponseValue TabsUpdateFunction::GetResult() {
  if (!has_callback())
    return NoArguments();

  tabs::Tab tab;

  auto* api_web_contents = electron::api::WebContents::From(web_contents_);
  tab.id = (api_web_contents ? api_web_contents->ID() : -1);

  if (CanAccessPrivilegedTabFields(
          extension(), api_web_contents ? api_web_contents->ID() : -1,
          web_contents_->GetLastCommittedURL())) {
    tab.url = web_contents_->GetLastCommittedURL().spec();
    tab.title = base::UTF16ToUTF8(web_contents_->GetTitle());
    if (auto icon = ElectronBrowserTabFavicon(web_contents_); !icon.empty()) tab.fav_icon_url = std::move(icon);
  }

  if (api_web_contents)
    tab.active = IsElectronBrowserTabActive(api_web_contents);
  if (const int window_id = ElectronBrowserTabWindowId(web_contents_); window_id >= 0) {
    tab.window_id = window_id;
    tab.index = ElectronBrowserTabIndex(web_contents_);
  }
  tab.selected = tab.active;
  tab.highlighted = tab.active;
  tab.pinned = IsElectronBrowserTabPinned(web_contents_);
  if (const int opener = ElectronBrowserTabOpener(web_contents_); opener >= 0) tab.opener_tab_id = opener;
  tab.incognito = web_contents_->GetBrowserContext()->IsOffTheRecord();
  tab.group_id = -1;
  tab.status = web_contents_->IsLoading() ? tabs::TabStatus::kLoading : tabs::TabStatus::kComplete;
  tab.last_accessed = web_contents_->GetLastActiveTime().InMillisecondsFSinceUnixEpoch();
  tab.muted_info = CreateMutedInfo(web_contents_);
  tab.audible = web_contents_->IsCurrentlyAudible();
  // TODO: Add proper support for split views
  tab.split_view_id = -1;

  return ArgumentList(tabs::Get::Results::Create(std::move(tab)));
}

}  // namespace extensions
