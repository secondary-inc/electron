// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_API_ELECTRON_API_EXTENSIONS_H_
#define ELECTRON_SHELL_BROWSER_API_ELECTRON_API_EXTENSIONS_H_

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "shell/browser/extensions/api/extension_action/extension_action_api.h"
#include "shell/common/gin_helper/error_thrower.h"
#include "extensions/browser/extension_registry_observer.h"
#include "gin/wrappable.h"
#include "shell/browser/event_emitter_mixin.h"

namespace electron {

class ElectronBrowserContext;

namespace api {

class Extensions final : public gin::Wrappable<Extensions>,
                         public gin_helper::EventEmitterMixin<Extensions>,
                          private extensions::ExtensionRegistryObserver,
                          private extensions::ExtensionActionAPI::Observer {
 public:
  static Extensions* Create(v8::Isolate* isolate,
                            ElectronBrowserContext* browser_context);

  // gin::Wrappable
  static const gin::WrapperInfo kWrapperInfo;
  const gin::WrapperInfo* wrapper_info() const override;
  const char* GetHumanReadableName() const override;
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;
  const char* GetClassName() const { return "Extensions"; }

  // Make public for cppgc::MakeGarbageCollected.
  explicit Extensions(ElectronBrowserContext* browser_context);
  ~Extensions() override;

  v8::Local<v8::Promise> LoadExtension(v8::Isolate* isolate,
                                       const base::FilePath& extension_path,
                                       gin::Arguments* args);
  v8::Local<v8::Promise> PrepareCrx(v8::Isolate* isolate,
                                    const base::FilePath& source,
                                    const base::FilePath& parent);
  void RemoveExtension(const std::string& extension_id);
  v8::Local<v8::Promise> RevokeOptionalPermissions(v8::Isolate* isolate,
                                                 const std::string& extension_id);
  base::DictValue GetContextMenuState(gin_helper::ErrorThrower thrower,
                                      const std::string& extension_id, int tab_id,
                                      base::DictValue details);
  void ClickContextMenu(gin_helper::ErrorThrower thrower,
                        const std::string& extension_id, int tab_id,
                        int revision, const std::string& key, base::DictValue details);
  base::DictValue GetPermissionState(gin_helper::ErrorThrower thrower,
                                     const std::string& extension_id);
  void ClearPermissionGrants(gin_helper::ErrorThrower thrower,
                             const std::string& extension_id);
  v8::Local<v8::Value> GetExtension(v8::Isolate* isolate,
                                    const std::string& extension_id);
  v8::Local<v8::Value> GetAllExtensions(v8::Isolate* isolate);
  base::DictValue GetActionState(gin_helper::ErrorThrower thrower,
                                 const std::string& extension_id, int tab_id);
  void SetActionPinned(gin_helper::ErrorThrower thrower,
                       const std::string& extension_id, bool pinned);
  void ActivateActionPopup(gin_helper::ErrorThrower thrower,
                           const std::string& extension_id, int tab_id);
  base::ListValue GetCommands(gin_helper::ErrorThrower thrower,
                              const std::string& extension_id);
  void SetCommandShortcuts(gin_helper::ErrorThrower thrower,
                           const std::string& extension_id, base::DictValue bindings);
  base::DictValue GetUserScriptsState(gin_helper::ErrorThrower thrower,
                                      const std::string& extension_id);
  void SetUserScriptsAllowed(gin_helper::ErrorThrower thrower,
                            const std::string& extension_id, bool allowed);
  void NotifyInstalled(gin_helper::ErrorThrower thrower,
                       const std::string& extension_id,
                       const std::string& previous_version);
  void NotifyUninstalled(gin_helper::ErrorThrower thrower,
                         const std::string& extension_id);
  void RunCommand(gin_helper::ErrorThrower thrower,
                  const std::string& extension_id, const std::string& name,
                  int tab_id);
  void ClickAction(gin_helper::ErrorThrower thrower,
                   const std::string& extension_id, int tab_id);
  void OnExtensionActionUpdated(extensions::ExtensionAction* action,
                                content::WebContents* contents,
                                content::BrowserContext* context) override;
  void OnExtensionActionAPIShuttingDown() override;

  // extensions::ExtensionRegistryObserver:
  void OnExtensionLoaded(content::BrowserContext* browser_context,
                         const extensions::Extension* extension) override;
  void OnExtensionReady(content::BrowserContext* browser_context,
                        const extensions::Extension* extension) override;
  void OnExtensionUnloaded(content::BrowserContext* browser_context,
                           const extensions::Extension* extension,
                           extensions::UnloadedExtensionReason reason) override;

  // disable copy
  Extensions(const Extensions&) = delete;
  Extensions& operator=(const Extensions&) = delete;

 private:
  content::BrowserContext* browser_context() const {
    return browser_context_.get();
  }

  const raw_ptr<content::BrowserContext> browser_context_;
  raw_ptr<extensions::ExtensionActionAPI> action_api_ = nullptr;
};

}  // namespace api

}  // namespace electron

#endif  // ELECTRON_SHELL_BROWSER_API_ELECTRON_API_EXTENSIONS_H_
