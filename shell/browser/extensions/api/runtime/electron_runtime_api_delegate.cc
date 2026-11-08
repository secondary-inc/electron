// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/api/runtime/electron_runtime_api_delegate.h"

#include <string>
#include <string_view>

#include "base/logging.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_handlers/options_page_info.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "components/update_client/update_query_params.h"
#include "extensions/common/api/runtime.h"
#include "shell/browser/extensions/electron_extension_system.h"

using extensions::api::runtime::PlatformInfo;

namespace extensions {

ElectronRuntimeAPIDelegate::ElectronRuntimeAPIDelegate(
    content::BrowserContext* browser_context)
    : browser_context_(browser_context) {
  DCHECK(browser_context_);
}

ElectronRuntimeAPIDelegate::~ElectronRuntimeAPIDelegate() = default;

void ElectronRuntimeAPIDelegate::AddUpdateObserver(UpdateObserver* observer) {}

void ElectronRuntimeAPIDelegate::RemoveUpdateObserver(
    UpdateObserver* observer) {}

void ElectronRuntimeAPIDelegate::ReloadExtension(
    const std::string& extension_id) {
  static_cast<ElectronExtensionSystem*>(ExtensionSystem::Get(browser_context_))
      ->ReloadExtension(extension_id);
}

bool ElectronRuntimeAPIDelegate::CheckForUpdates(
    const std::string& extension_id,
    UpdateCheckCallback callback) {
  LOG(INFO) << "chrome.runtime.requestUpdateCheck is not supported in Electron";
  return false;
}

void ElectronRuntimeAPIDelegate::OpenURL(const GURL& uninstall_url) {
  LOG(INFO) << "chrome.runtime.openURL is not supported in Electron";
}

void ElectronRuntimeAPIDelegate::OpenOptionsPage(
    const Extension* extension,
    content::BrowserContext* browser_context,
    base::OnceCallback<void(bool)> callback) {
  if (browser_context != browser_context_ || !extension ||
      !ExtensionRegistry::Get(browser_context)->enabled_extensions().Contains(
          extension->id()) ||
      !OptionsPageInfo::HasOptionsPage(extension)) {
    std::move(callback).Run(false);
    return;
  }
  auto* contents = GetActiveElectronTab(browser_context);
  if (!contents || ElectronBrowserTabWindowId(contents->web_contents()) < 0) {
    std::move(callback).Run(false);
    return;
  }
  // Only the main-process host may acknowledge an actual options document.
  // Complete exactly once, including a missing host or an abandoned request.
  using Pending = base::RefCountedData<base::OnceCallback<void(bool)>>;
  auto pending = base::MakeRefCounted<Pending>(std::move(callback));
  auto respond = base::BindRepeating(
      [](scoped_refptr<Pending> pending, bool success) {
        if (pending->data)
          std::move(pending->data).Run(success);
      },
      pending);
  if (!contents->Emit("browser-extension-options", extension->id(), respond)) {
    respond.Run(false);
    return;
  }
  if (pending->data)
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(respond, false), base::Seconds(15));
}

bool ElectronRuntimeAPIDelegate::GetPlatformInfo(PlatformInfo* info) {
  if (const std::string_view os = update_client::UpdateQueryParams::GetOS();
      os == "mac") {
    info->os = extensions::api::runtime::PlatformOs::kMac;
  } else if (os == "win") {
    info->os = extensions::api::runtime::PlatformOs::kWin;
  } else if (os == "linux") {
    info->os = extensions::api::runtime::PlatformOs::kLinux;
  } else if (os == "openbsd") {
    info->os = extensions::api::runtime::PlatformOs::kOpenbsd;
  } else {
    NOTREACHED();
  }

  if (const std::string_view arch = update_client::UpdateQueryParams::GetArch();
      arch == "arm") {
    info->arch = extensions::api::runtime::PlatformArch::kArm;
  } else if (arch == "arm64") {
    info->arch = extensions::api::runtime::PlatformArch::kArm64;
  } else if (arch == "x86") {
    info->arch = extensions::api::runtime::PlatformArch::kX86_32;
  } else if (arch == "x64") {
    info->arch = extensions::api::runtime::PlatformArch::kX86_64;
  } else {
    NOTREACHED();
  }

  return true;
}

bool ElectronRuntimeAPIDelegate::RestartDevice(std::string* error_message) {
  *error_message = "Restart is not supported in Electron";
  return false;
}

}  // namespace extensions
