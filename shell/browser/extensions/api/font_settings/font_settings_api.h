// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_FONT_SETTINGS_FONT_SETTINGS_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_FONT_SETTINGS_FONT_SETTINGS_API_H_
#include "extensions/browser/extension_function.h"
namespace extensions {
class FontSettingsGetFontListFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("fontSettings.getFontList", FONTSETTINGS_GETFONTLIST)
 protected:
  ~FontSettingsGetFontListFunction() override = default;
  ResponseAction Run() override;
 private:
  void FontListHasLoaded(base::ListValue fonts);
};
}  // namespace extensions
#endif
