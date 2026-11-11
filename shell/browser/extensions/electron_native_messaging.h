// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license.
#ifndef ELECTRON_SHELL_BROWSER_EXTENSIONS_ELECTRON_NATIVE_MESSAGING_H_
#define ELECTRON_SHELL_BROWSER_EXTENSIONS_ELECTRON_NATIVE_MESSAGING_H_
#include <memory>
#include <string>
#include "base/values.h"
#include "extensions/browser/api/messaging/message_port.h"
namespace content { class BrowserContext; }
namespace extensions {
bool ConfigureElectronNativeMessaging(content::BrowserContext*, const base::ListValue&);
bool ElectronNativeHostAllowed(content::BrowserContext*, const std::string&);
std::unique_ptr<MessagePort> CreateElectronNativeMessagePort(
 content::BrowserContext*, base::WeakPtr<MessagePort::ChannelDelegate>,
 const PortId&, const std::string& extension_id, const std::string& host);
bool ElectronNativeMessageReady(content::BrowserContext*, const std::string&);
bool ElectronNativeMessageReply(content::BrowserContext*, const std::string&, const std::string&);
bool ElectronNativeMessageClose(content::BrowserContext*, const std::string&, const std::string&);
}
#endif
