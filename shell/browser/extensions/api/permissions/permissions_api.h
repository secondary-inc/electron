// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_PERMISSIONS_PERMISSIONS_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_PERMISSIONS_PERMISSIONS_API_H_

#include "extensions/browser/extension_function.h"

namespace extensions {
class PermissionsContainsFunction : public ExtensionFunction {
 private:
  ~PermissionsContainsFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.contains", PERMISSIONS_CONTAINS)
};
class PermissionsGetAllFunction : public ExtensionFunction {
 private:
  ~PermissionsGetAllFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.getAll", PERMISSIONS_GETALL)
};
class PermissionsRequestFunction : public ExtensionFunction {
 private:
  ~PermissionsRequestFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.request", PERMISSIONS_REQUEST)
};
class PermissionsRemoveFunction : public ExtensionFunction {
 private:
  ~PermissionsRemoveFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("permissions.remove", PERMISSIONS_REMOVE)
};
}  // namespace extensions
#endif  // ELECTRON_SHELL_BROWSER_EXTENSIONS_API_PERMISSIONS_PERMISSIONS_API_H_
