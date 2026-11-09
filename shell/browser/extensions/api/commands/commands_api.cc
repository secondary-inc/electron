// Copyright 2026 The Electron Authors
// Use of this source code is governed by the MIT license.
#include "shell/browser/extensions/api/commands/commands_api.h"
#include "base/strings/utf_string_conversions.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/common/api/commands/commands_handler.h"
namespace extensions {
std::string WorkCommandShortcut(content::BrowserContext* context,
                                const std::string& id, const std::string& name) {
  const auto* stored = ExtensionPrefs::Get(context)->ReadPrefAsDictionary(id, "iris_command_shortcuts");
  const auto* value = stored ? stored->FindString(name) : nullptr;
  return value && value->size() <= 64 ? *value : "";
}
ExtensionFunction::ResponseAction CommandsGetAllFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().empty());
  base::ListValue result;
  auto append = [this, &result](const ui::Command& command) {
    result.Append(base::DictValue()
        .Set("name", command.command_name())
        .Set("description", base::UTF16ToUTF8(command.description()))
        // Only host-assigned local shortcuts, never unregistered suggestions.
        .Set("shortcut", WorkCommandShortcut(browser_context(), extension_id(), command.command_name())));
  };
  if (const auto* commands = CommandsInfo::GetNamedCommands(extension()))
    for (const auto& [name, command] : *commands) append(command);
  if (const auto* command = CommandsInfo::GetActionCommand(extension())) append(*command);
  if (const auto* command = CommandsInfo::GetBrowserActionCommand(extension())) append(*command);
  if (const auto* command = CommandsInfo::GetPageActionCommand(extension())) append(*command);
  return RespondNow(WithArguments(std::move(result)));
}
}
