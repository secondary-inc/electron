// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_CONTEXT_MENUS_CONTEXT_MENUS_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_CONTEXT_MENUS_CONTEXT_MENUS_API_H_
#include "extensions/browser/extension_function.h"
namespace extensions {
base::DictValue ReadWorkContextMenus(content::BrowserContext* context, const std::string& id);
void WriteWorkContextMenus(content::BrowserContext* context, const std::string& id, base::DictValue state, std::string preferred_radio = {});
std::string WorkContextMenuKey(const base::Value& id);
bool WorkContextMenuMatches(const base::DictValue& item, const base::DictValue& details);
class ContextMenusCreateFunction : public ExtensionFunction {
 private:
  ~ContextMenusCreateFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("contextMenus.create", CONTEXTMENUS_CREATE)
};
class ContextMenusUpdateFunction : public ExtensionFunction {
 private:
  ~ContextMenusUpdateFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("contextMenus.update", CONTEXTMENUS_UPDATE)
};
class ContextMenusRemoveFunction : public ExtensionFunction {
 private:
  ~ContextMenusRemoveFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("contextMenus.remove", CONTEXTMENUS_REMOVE)
};
class ContextMenusRemoveAllFunction : public ExtensionFunction {
 private:
  ~ContextMenusRemoveAllFunction() override = default;
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("contextMenus.removeAll", CONTEXTMENUS_REMOVEALL)
};
}
#endif
