// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_EXTENSION_EXTENSION_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_EXTENSION_EXTENSION_API_H_
#include "extensions/browser/extension_function.h"
namespace extensions {
class ExtensionIsAllowedIncognitoAccessFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("extension.isAllowedIncognitoAccess",
                             EXTENSION_ISALLOWEDINCOGNITOACCESS)
 protected:
  ~ExtensionIsAllowedIncognitoAccessFunction() override = default;
  ResponseAction Run() override;
};
class ExtensionIsAllowedFileSchemeAccessFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("extension.isAllowedFileSchemeAccess",
                             EXTENSION_ISALLOWEDFILESCHEMEACCESS)
 protected:
  ~ExtensionIsAllowedFileSchemeAccessFunction() override = default;
  ResponseAction Run() override;
};
}  // namespace extensions
#endif
