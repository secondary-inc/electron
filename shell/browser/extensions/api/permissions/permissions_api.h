// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_PERMISSIONS_PERMISSIONS_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_PERMISSIONS_PERMISSIONS_API_H_

#include "extensions/browser/extension_function.h"

namespace extensions {
class ElectronPermissionsContainsFunction : public ExtensionFunction {
 private:
  ~ElectronPermissionsContainsFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.contains", PERMISSIONS_CONTAINS)
};
class ElectronPermissionsGetAllFunction : public ExtensionFunction {
 private:
  ~ElectronPermissionsGetAllFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.getAll", PERMISSIONS_GETALL)
};
class ElectronPermissionsRequestFunction : public ExtensionFunction {
 private:
  ~ElectronPermissionsRequestFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.request", PERMISSIONS_REQUEST)
};
class ElectronPermissionsRemoveFunction : public ExtensionFunction {
 private:
  ~ElectronPermissionsRemoveFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.remove", PERMISSIONS_REMOVE)
};
}  // namespace extensions
#endif  // ELECTRON_SHELL_BROWSER_EXTENSIONS_API_PERMISSIONS_PERMISSIONS_API_H_
