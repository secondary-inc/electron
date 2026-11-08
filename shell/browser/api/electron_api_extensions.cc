// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/api/electron_api_extensions.h"

#include "chrome/browser/browser_process.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/event_router.h"
#include "base/base64.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image_skia.h"
#include "extensions/common/permissions/permissions_data.h"
#include "base/strings/utf_string_conversions.h"
#include "shell/common/extensions/api/tabs.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "gin/data_object_builder.h"
#include "gin/object_template_builder.h"
#include "shell/browser/electron_browser_context.h"
#include "shell/browser/extensions/electron_extension_system.h"
#include "shell/browser/javascript_environment.h"
#include "shell/common/gin_converters/extension_converter.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_converters/gurl_converter.h"
#include "shell/common/gin_converters/value_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/promise.h"
#include "shell/common/gin_helper/wrappable_pointer_tags.h"
#include "shell/common/node_util.h"
#include "v8/include/cppgc/allocation.h"

namespace electron::api {

const gin::WrapperInfo Extensions::kWrapperInfo =
    electron::MakeWrapperInfo(electron::kElectronExtensions);

Extensions::Extensions(ElectronBrowserContext* browser_context)
    : browser_context_{browser_context} {
  extensions::ExtensionRegistry::Get(browser_context)->AddObserver(this);
  action_api_ = extensions::ExtensionActionAPI::Get(browser_context);
  action_api_->AddObserver(this);
}

Extensions::~Extensions() {
  if (action_api_) action_api_->RemoveObserver(this);
  extensions::ExtensionRegistry::Get(browser_context())->RemoveObserver(this);
}

// static
Extensions* Extensions::Create(v8::Isolate* isolate,
                               ElectronBrowserContext* browser_context) {
  return cppgc::MakeGarbageCollected<Extensions>(
      isolate->GetCppHeap()->GetAllocationHandle(), browser_context);
}

v8::Local<v8::Promise> Extensions::LoadExtension(
    v8::Isolate* isolate,
    const base::FilePath& extension_path,
    gin::Arguments* args) {
  gin_helper::Promise<const extensions::Extension*> promise(isolate);
  v8::Local<v8::Promise> handle = promise.GetHandle();

  if (!extension_path.IsAbsolute()) {
    promise.RejectWithErrorMessage(
        "The path to the extension in 'loadExtension' must be absolute");
    return handle;
  }

  if (browser_context()->IsOffTheRecord()) {
    promise.RejectWithErrorMessage(
        "Extensions cannot be loaded in a temporary session");
    return handle;
  }

  int load_flags = extensions::Extension::FOLLOW_SYMLINKS_ANYWHERE;
  gin_helper::Dictionary options;
  if (args->GetNext(&options)) {
    bool allowFileAccess = false;
    options.Get("allowFileAccess", &allowFileAccess);
    if (allowFileAccess)
      load_flags |= extensions::Extension::ALLOW_FILE_ACCESS;
  }

  auto* extension_system = static_cast<extensions::ElectronExtensionSystem*>(
      extensions::ExtensionSystem::Get(browser_context()));
  extension_system->LoadExtension(
      extension_path, load_flags,
      base::BindOnce(
          [](gin_helper::Promise<const extensions::Extension*> promise,
             const extensions::Extension* extension,
             const std::string& error_msg) {
            if (extension) {
              if (!error_msg.empty())
                util::EmitWarning(promise.isolate(), error_msg,
                                  "ExtensionLoadWarning");
              promise.Resolve(extension);
            } else {
              promise.RejectWithErrorMessage(error_msg);
            }
          },
          std::move(promise)));

  return handle;
}

void Extensions::RemoveExtension(const std::string& extension_id) {
  auto* extension_system = static_cast<extensions::ElectronExtensionSystem*>(
      extensions::ExtensionSystem::Get(browser_context()));
  extension_system->RemoveExtension(extension_id);
}

v8::Local<v8::Value> Extensions::GetExtension(v8::Isolate* isolate,
                                              const std::string& extension_id) {
  auto* registry = extensions::ExtensionRegistry::Get(browser_context());
  const extensions::Extension* extension =
      registry->GetInstalledExtension(extension_id);
  if (extension) {
    return gin::ConvertToV8(isolate, extension);
  } else {
    return v8::Null(isolate);
  }
}

v8::Local<v8::Value> Extensions::GetAllExtensions(v8::Isolate* isolate) {
  auto* registry = extensions::ExtensionRegistry::Get(browser_context());
  const extensions::ExtensionSet extensions =
      registry->GenerateInstalledExtensionsSet();
  std::vector<const extensions::Extension*> extensions_vector;
  for (const auto& extension : extensions) {
    if (extension->location() !=
        extensions::mojom::ManifestLocation::kComponent)
      extensions_vector.emplace_back(extension.get());
  }
  return gin::ConvertToV8(isolate, extensions_vector);
}

base::DictValue Extensions::GetActionState(gin_helper::ErrorThrower thrower,
                                            const std::string& extension_id,
                                            int tab_id) {
  auto* extension = extensions::ExtensionRegistry::Get(browser_context())
                        ->enabled_extensions().GetByID(extension_id);
  if (!extension || (tab_id != -1 &&
      !extensions::GetElectronTabById(tab_id, browser_context()))) {
    thrower.ThrowError("Extension or tab not found in this profile");
    return {};
  }
  auto* action = extensions::ExtensionActionManager::Get(browser_context())
                     ->GetExtensionAction(*extension);
  if (!action) return {};
  auto icon = action->GetExplicitlySetIcon(tab_id);
  // Electron does not package Chrome's default icon placeholder resources.
  // Leave the icon empty until the extension supplies explicit image data.
  std::string icon_url;
  if (!icon.IsEmpty()) {
    const auto& bitmap = icon.AsImageSkia().GetRepresentation(1.0f).GetBitmap();
    if (!bitmap.isNull() && bitmap.width() <= 512 && bitmap.height() <= 512) {
      auto png = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
      if (png && png->size() <= 768 * 1024)
        icon_url = "data:image/png;base64," + base::Base64Encode(*png);
    }
  }
  return base::DictValue()
      .Set("icon", icon_url.size() <= 1024 * 1024 ? icon_url : "")
      .Set("title", action->GetTitle(tab_id))
      .Set("popup", action->GetPopupUrl(tab_id).spec())
      .Set("badgeText", action->GetDisplayBadgeText(tab_id))
      .Set("badgeBackgroundColor", static_cast<double>(action->GetBadgeBackgroundColor(tab_id)))
      .Set("badgeTextColor", static_cast<double>(action->GetBadgeTextColor(tab_id)))
      .Set("enabled", action->GetIsVisibleIgnoringDeclarative(tab_id));
}
void Extensions::ClickAction(gin_helper::ErrorThrower thrower,
                             const std::string& extension_id, int tab_id) {
  auto* extension = extensions::ExtensionRegistry::Get(browser_context())
                        ->enabled_extensions().GetByID(extension_id);
  auto* tab = extensions::GetElectronTabById(tab_id, browser_context());
  if (!extension || !tab ||
      extensions::ElectronBrowserTabWindowId(tab->web_contents()) < 0 ||
      !extensions::IsElectronBrowserTabActive(tab)) {
    thrower.ThrowError("Enabled extension and selected managed tab required");
    return;
  }
  auto* action = extensions::ExtensionActionManager::Get(browser_context())
                     ->GetExtensionAction(*extension);
  if (!action || !action->GetIsVisibleIgnoringDeclarative(tab_id)) {
    thrower.ThrowError("Extension action is disabled");
    return;
  }
  if (action->HasPopup(tab_id)) {
    thrower.ThrowError("Open the extension popup instead");
    return;
  }
  auto* contents = tab->web_contents();
  extensions::api::tabs::Tab value;
  value.id = tab_id;
  value.active = true;
  value.pinned = extensions::IsElectronBrowserTabPinned(contents);
  value.selected = true;
  value.highlighted = true;
  value.window_id = extensions::ElectronBrowserTabWindowId(contents);
  value.index = extensions::ElectronBrowserTabIndex(contents);
  value.incognito = browser_context()->IsOffTheRecord();
  value.group_id = -1;
  value.split_view_id = -1;
  value.last_accessed = contents->GetLastActiveTime().InMillisecondsFSinceUnixEpoch();
  value.status = contents->IsLoading() ? extensions::api::tabs::TabStatus::kLoading :
                                        extensions::api::tabs::TabStatus::kComplete;
  const auto& url = contents->GetLastCommittedURL();
  // Dispatch does not grant new host or activeTab permissions.
  if (extension->permissions_data()->HasAPIPermissionForTab(
          tab_id, extensions::mojom::APIPermissionID::kTab) ||
      extension->permissions_data()->HasHostPermission(url)) {
    value.url = url.spec();
    value.title = base::UTF16ToUTF8(contents->GetTitle());
  }
  const bool legacy = action->action_type() == extensions::ActionInfo::Type::kBrowser;
  const bool page = action->action_type() == extensions::ActionInfo::Type::kPage;
  auto event = std::make_unique<extensions::Event>(
      page ? extensions::events::PAGE_ACTION_ON_CLICKED :
      legacy ? extensions::events::BROWSER_ACTION_ON_CLICKED : extensions::events::ACTION_ON_CLICKED,
      page ? "pageAction.onClicked" : legacy ? "browserAction.onClicked" : "action.onClicked",
      base::ListValue().Append(value.ToValue()), browser_context());
  event->user_gesture = extensions::EventRouter::UserGestureState::kEnabled;
  extensions::EventRouter::Get(browser_context())->DispatchEventToExtension(extension_id, std::move(event));
}
void Extensions::OnExtensionActionUpdated(extensions::ExtensionAction* action,
                                           content::WebContents* contents,
                                           content::BrowserContext* context) {
  if (context != browser_context()) return;
  auto* tab = contents ? WebContents::From(contents) : nullptr;
  Emit("action-updated", action->extension_id(), tab ? tab->ID() : -1);
}
void Extensions::OnExtensionActionAPIShuttingDown() {
  action_api_ = nullptr;
}

void Extensions::OnExtensionLoaded(content::BrowserContext* browser_context,
                                   const extensions::Extension* extension) {
  Emit("extension-loaded", extension);
}

void Extensions::OnExtensionUnloaded(
    content::BrowserContext* browser_context,
    const extensions::Extension* extension,
    extensions::UnloadedExtensionReason reason) {
  Emit("extension-unloaded", extension);
}

void Extensions::OnExtensionReady(content::BrowserContext* browser_context,
                                  const extensions::Extension* extension) {
  Emit("extension-ready", extension);
}

// static
gin::ObjectTemplateBuilder Extensions::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin_helper::EventEmitterMixin<Extensions>::GetObjectTemplateBuilder(
             isolate)
      .SetMethod("loadExtension", &Extensions::LoadExtension)
      .SetMethod("removeExtension", &Extensions::RemoveExtension)
      .SetMethod("getExtension", &Extensions::GetExtension)
      .SetMethod("getAllExtensions", &Extensions::GetAllExtensions)
      .SetMethod("getActionState", &Extensions::GetActionState)
      .SetMethod("clickAction", &Extensions::ClickAction);
}

const gin::WrapperInfo* Extensions::wrapper_info() const {
  return &kWrapperInfo;
}

const char* Extensions::GetHumanReadableName() const {
  return "Electron / Extensions";
}

}  // namespace electron::api
