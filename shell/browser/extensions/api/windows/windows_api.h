// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_WINDOWS_WINDOWS_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_WINDOWS_WINDOWS_API_H_
#include "extensions/browser/extension_function.h"
namespace extensions {
class WindowsGetAllFunction : public ExtensionFunction {
 private:
  ~WindowsGetAllFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.getAll", WINDOWS_GETALL)
};
class WindowsGetFunction : public ExtensionFunction {
 private:
  ~WindowsGetFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.get", WINDOWS_GET)
};
class WindowsGetLastFocusedFunction : public ExtensionFunction {
 private:
  ~WindowsGetLastFocusedFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.getLastFocused", WINDOWS_GETLASTFOCUSED)
};
class WindowsGetCurrentFunction : public ExtensionFunction {
 private:
  ~WindowsGetCurrentFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.getCurrent", WINDOWS_GETCURRENT)
};
}
#endif
