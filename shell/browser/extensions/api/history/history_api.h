// Copyright 2026 Electron contributors.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_HISTORY_HISTORY_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_HISTORY_HISTORY_API_H_
#include <string>
#include "base/values.h"
class ExtensionFunctionRegistry;
namespace content { class BrowserContext; }
namespace extensions {
void RegisterElectronHistoryFunctions(ExtensionFunctionRegistry*);
void DispatchElectronHistoryEvent(content::BrowserContext*,const std::string&,base::ListValue);
}
#endif
