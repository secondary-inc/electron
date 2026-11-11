// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_WEB_NAVIGATION_WEB_NAVIGATION_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_WEB_NAVIGATION_WEB_NAVIGATION_API_H_
#include "extensions/browser/extension_function.h"
namespace content { class WebContents; }
namespace extensions {
void ObserveElectronWebNavigation(content::WebContents* contents);
class WebNavigationGetFrameFunction : public ExtensionFunction {
 private:
  ~WebNavigationGetFrameFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("webNavigation.getFrame", WEBNAVIGATION_GETFRAME)
};
class WebNavigationGetAllFramesFunction : public ExtensionFunction {
 private:
  ~WebNavigationGetAllFramesFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("webNavigation.getAllFrames", WEBNAVIGATION_GETALLFRAMES)
};
}
#endif
