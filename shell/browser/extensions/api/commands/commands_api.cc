// Copyright 2026 The Electron Authors
// Use of this source code is governed by the MIT license.
#include "shell/browser/extensions/api/commands/commands_api.h"
#include "base/strings/utf_string_conversions.h"
#include "extensions/common/api/commands/commands_handler.h"
namespace extensions {
ExtensionFunction::ResponseAction CommandsGetAllFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().empty());
  base::ListValue result;
  auto append = [&result](const ui::Command& command) {
    result.Append(base::DictValue()
        .Set("name", command.command_name())
        .Set("description", base::UTF16ToUTF8(command.description()))
        // Manifest suggestions are not registered host accelerators.
        .Set("shortcut", ""));
  };
  if (const auto* commands = CommandsInfo::GetNamedCommands(extension()))
    for (const auto& [name, command] : *commands) append(command);
  if (const auto* command = CommandsInfo::GetActionCommand(extension())) append(*command);
  if (const auto* command = CommandsInfo::GetBrowserActionCommand(extension())) append(*command);
  if (const auto* command = CommandsInfo::GetPageActionCommand(extension())) append(*command);
  return RespondNow(WithArguments(std::move(result)));
}
}
