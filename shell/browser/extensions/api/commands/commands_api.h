// Copyright 2026 The Electron Authors
// Use of this source code is governed by the MIT license.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_COMMANDS_COMMANDS_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_COMMANDS_COMMANDS_API_H_
#include "extensions/browser/extension_function.h"
namespace extensions {
std::string WorkCommandShortcut(content::BrowserContext* context,
                                const std::string& id, const std::string& name);
class CommandsGetAllFunction : public ExtensionFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("commands.getAll", COMMANDS_GETALL)
 protected:
  ~CommandsGetAllFunction() override = default;
  ResponseAction Run() override;
};
}
#endif
