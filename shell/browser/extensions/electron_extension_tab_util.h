// Copyright (c) 2026 Anthropic PBC
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_ELECTRON_EXTENSION_TAB_UTIL_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_ELECTRON_EXTENSION_TAB_UTIL_H_

#include <string>

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace electron::api {
class WebContents;
}  // namespace electron::api

namespace extensions {

// Main-process managed tab identity is independent of transient keyboard focus.
void SetElectronBrowserTabState(content::WebContents* contents, bool active,
                                int window_id, int index, bool pinned);
bool IsElectronBrowserTab(content::WebContents* contents);
bool IsElectronBrowserTabPinned(content::WebContents* contents);
bool IsElectronBrowserTabActive(electron::api::WebContents* contents);
int ElectronBrowserTabWindowId(content::WebContents* contents);
int ElectronBrowserTabIndex(content::WebContents* contents);
std::string ElectronBrowserTabFavicon(content::WebContents* contents);

// Returns the unique selected tab in this profile; ambiguity fails closed.
electron::api::WebContents* GetActiveElectronTab(content::BrowserContext* browser_context);

// Resolves |tab_id| to an electron::api::WebContents only if the underlying
// WebContents belongs to |browser_context|. Tabs in other BrowserContexts are
// treated as nonexistent so that an extension loaded into one Session cannot
// observe or operate on windows belonging to another Session, matching
// Chrome's profile-scoped ExtensionTabUtil::GetTabById semantics.
electron::api::WebContents* GetElectronTabById(
    int tab_id,
    content::BrowserContext* browser_context);

}  // namespace extensions

#endif  // ELECTRON_SHELL_BROWSER_EXTENSIONS_ELECTRON_EXTENSION_TAB_UTIL_H_
