// Copyright 2026 Electron contributors.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_API_SESSIONS_SESSIONS_API_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_API_SESSIONS_SESSIONS_API_H_
#include <string>
#include "base/values.h"
class ExtensionFunctionRegistry;
namespace content { class BrowserContext; }
namespace extensions {
void RegisterElectronSessionsFunctions(ExtensionFunctionRegistry*);
void DispatchElectronSessionsEvent(content::BrowserContext*,const std::string&,base::ListValue);
}
#endif
