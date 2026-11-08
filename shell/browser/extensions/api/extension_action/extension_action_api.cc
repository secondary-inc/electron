// Copyright (c) 2023 Microsoft, GmbH
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/api/extension_action/extension_action_api.h"

#include <stddef.h>

#include <utility>

#include "base/logging.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "base/no_destructor.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/icon_util.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "extensions/browser/extension_registry.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "url/origin.h"
#include "content/public/common/color_parser.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_util.h"
#include "extensions/common/mojom/view_type.mojom.h"

using content::WebContents;

namespace extensions {
namespace {
bool ParseActionColor(const base::Value& value, SkColor* color) {
  if (value.is_string())
    return content::ParseCssColorString(value.GetString(), color);
  if (!value.is_list() || value.GetList().size() != 4) return false;
  const auto& values = value.GetList();
  for (const auto& component : values)
    if (!component.is_int() || component.GetInt() < 0 || component.GetInt() > 255)
      return false;
  *color = SkColorSetARGB(values[3].GetInt(), values[0].GetInt(),
                          values[1].GetInt(), values[2].GetInt());
  return true;
}
base::ListValue ActionColorValues(SkColor color) {
  return base::ListValue().Append(static_cast<int>(SkColorGetR(color)))
      .Append(static_cast<int>(SkColorGetG(color)))
      .Append(static_cast<int>(SkColorGetB(color)))
      .Append(static_cast<int>(SkColorGetA(color)));
}
}  // namespace

//
// ExtensionActionAPI::Observer
//

void ExtensionActionAPI::Observer::OnExtensionActionUpdated(
    ExtensionAction* extension_action,
    content::WebContents* web_contents,
    content::BrowserContext* browser_context) {}

void ExtensionActionAPI::Observer::OnExtensionActionAPIShuttingDown() {}

//
// ExtensionActionAPI
//

ExtensionActionAPI::ExtensionActionAPI(content::BrowserContext*) {}
ExtensionActionAPI::~ExtensionActionAPI() = default;

// static
BrowserContextKeyedAPIFactory<ExtensionActionAPI>*
ExtensionActionAPI::GetFactoryInstance() {
  static base::NoDestructor<BrowserContextKeyedAPIFactory<ExtensionActionAPI>>
      instance;
  return instance.get();
}

// static
ExtensionActionAPI* ExtensionActionAPI::Get(content::BrowserContext* context) {
  return BrowserContextKeyedAPIFactory<ExtensionActionAPI>::Get(context);
}

void ExtensionActionAPI::Shutdown() {
  for (auto& observer : observers_) observer.OnExtensionActionAPIShuttingDown();
  observers_.Clear();
}
void ExtensionActionAPI::NotifyChange(ExtensionAction* action,
                                      content::WebContents* contents,
                                      content::BrowserContext* context) {
  for (auto& observer : observers_)
    observer.OnExtensionActionUpdated(action, contents, context);
}

void ExtensionActionAPI::ClearAllValuesForTab(content::WebContents* contents) {
  auto* tab = electron::api::WebContents::From(contents);
  if (!tab) return;
  auto* context = contents->GetBrowserContext();
  auto* manager = ExtensionActionManager::Get(context);
  for (const auto& extension : ExtensionRegistry::Get(context)->enabled_extensions()) {
    if (auto* action = manager->GetExtensionAction(*extension))
      action->ClearAllValuesForTab(tab->ID());
  }
}

//
 // ExtensionActionFunction
//

ExtensionActionFunction::ExtensionActionFunction() = default;

ExtensionFunction::ResponseAction ExtensionActionFunction::Run() {
  action_ = ExtensionActionManager::Get(browser_context())->GetExtensionAction(*extension());
  if (!action_) return RespondNow(Error("This extension has no action"));
  if (!args().empty()) {
    if (args()[0].is_dict())
      tab_id_ = args()[0].GetDict().FindInt("tabId").value_or(-1);
    else if (args()[0].is_int())
      tab_id_ = args()[0].GetInt();
  }
  if (tab_id_ != -1 && !GetElectronTabById(tab_id_, browser_context()))
    return RespondNow(Error("No such tab"));
  return RunExtensionAction();
}
void ExtensionActionFunction::NotifyActionChange() {
  auto* tab = GetElectronTabById(tab_id_, browser_context());
  ExtensionActionAPI::Get(browser_context())->NotifyChange(
      action_, tab ? tab->web_contents() : nullptr, browser_context());
}

ExtensionFunction::ResponseAction
ExtensionActionShowFunction::RunExtensionAction() {
  action_->SetIsVisible(tab_id_, true);
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ExtensionActionHideFunction::RunExtensionAction() {
  action_->SetIsVisible(tab_id_, false);
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ActionIsEnabledFunction::RunExtensionAction() {
  return RespondNow(WithArguments(action_->GetIsVisibleIgnoringDeclarative(tab_id_)));
}

ExtensionFunction::ResponseAction
ExtensionActionSetIconFunction::RunExtensionAction() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_dict());
  const auto& details = args()[0].GetDict();
  const auto* images = details.FindDict("imageData");
  if (!images) {
    if (details.FindInt("iconIndex")) return RespondNow(NoArguments());
    return RespondNow(Error("Icon image data is required"));
  }
  gfx::ImageSkia icon;
  if (ParseIconFromCanvasDictionary(*images, &icon) != IconParseResult::kSuccess ||
      icon.isNull())
    return RespondNow(Error("Invalid extension action icon"));
  action_->SetIcon(tab_id_, gfx::Image(icon));
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ExtensionActionOpenPopupFunction::RunExtensionAction() {
  LOG(INFO) << "chrome.action.openPopup is not supported in Electron";

  return RespondNow(WithArguments(""));
}

ExtensionFunction::ResponseAction
ExtensionActionSetTitleFunction::RunExtensionAction() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_dict());
  const auto* title = args()[0].GetDict().FindString("title");
  EXTENSION_FUNCTION_VALIDATE(title);
  action_->SetTitle(tab_id_, *title);
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ExtensionActionSetPopupFunction::RunExtensionAction() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_dict());
  const auto* path = args()[0].GetDict().FindString("popup");
  EXTENSION_FUNCTION_VALIDATE(path);
  const GURL url = path->empty() ? GURL() : extension()->GetResourceURL(*path);
  if (!path->empty() && (!url.is_valid() ||
      !extension()->origin().IsSameOriginWith(url::Origin::Create(url))))
    return RespondNow(Error("Popup must belong to this extension"));
  action_->SetPopupUrl(tab_id_, url);
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ExtensionActionSetBadgeTextFunction::RunExtensionAction() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_dict());
  const auto* text = args()[0].GetDict().FindString("text");
  if (text) action_->SetBadgeText(tab_id_, *text);
  else action_->ClearBadgeText(tab_id_);
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ExtensionActionSetBadgeBackgroundColorFunction::RunExtensionAction() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_dict());
  const auto* value = args()[0].GetDict().Find("color");
  EXTENSION_FUNCTION_VALIDATE(value);
  SkColor color = 0;
  if (!ParseActionColor(*value, &color))
    return RespondNow(Error("Invalid badge color"));
  action_->SetBadgeBackgroundColor(tab_id_, color);
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ActionSetBadgeTextColorFunction::RunExtensionAction() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_dict());
  const auto* value = args()[0].GetDict().Find("color");
  EXTENSION_FUNCTION_VALIDATE(value);
  SkColor color = 0;
  if (!ParseActionColor(*value, &color) || SkColorGetA(color) == 0)
    return RespondNow(Error("Invalid badge color"));
  action_->SetBadgeTextColor(tab_id_, color);
  NotifyActionChange();
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
ExtensionActionGetTitleFunction::RunExtensionAction() {
  return RespondNow(WithArguments(action_->GetTitle(tab_id_)));
}

ExtensionFunction::ResponseAction
ExtensionActionGetPopupFunction::RunExtensionAction() {
  return RespondNow(WithArguments(action_->GetPopupUrl(tab_id_).spec()));
}

ExtensionFunction::ResponseAction
ExtensionActionGetBadgeTextFunction::RunExtensionAction() {
  return RespondNow(WithArguments(action_->GetExplicitlySetBadgeText(tab_id_)));
}

ExtensionFunction::ResponseAction
ExtensionActionGetBadgeBackgroundColorFunction::RunExtensionAction() {
  return RespondNow(WithArguments(
      ActionColorValues(action_->GetBadgeBackgroundColor(tab_id_))));
}

ExtensionFunction::ResponseAction
ActionGetBadgeTextColorFunction::RunExtensionAction() {
  return RespondNow(WithArguments(
      ActionColorValues(action_->GetBadgeTextColor(tab_id_))));
}

ActionGetUserSettingsFunction::ActionGetUserSettingsFunction() = default;
ActionGetUserSettingsFunction::~ActionGetUserSettingsFunction() = default;

ExtensionFunction::ResponseAction ActionGetUserSettingsFunction::Run() {
  bool pinned = false;
  ExtensionPrefs::Get(browser_context())->ReadPrefAsBoolean(
      extension_id(), "iris_toolbar_pinned", &pinned);
  base::DictValue ui_settings;
  ui_settings.Set("isOnToolbar", pinned);
  return RespondNow(WithArguments(std::move(ui_settings)));
}

ExtensionFunction::ResponseAction
ActionOpenPopupFunction::RunExtensionAction() {
  auto* tab = GetActiveElectronTab(browser_context());
  if (!tab || ElectronBrowserTabWindowId(tab->web_contents()) < 0)
    return RespondNow(Error("No active managed browser window"));
  const int window_id = ElectronBrowserTabWindowId(tab->web_contents());
  if (!args().empty() && args()[0].is_dict()) {
    const auto* requested = args()[0].GetDict().Find("windowId");
    if (requested) {
      EXTENSION_FUNCTION_VALIDATE(requested->is_int());
      if (requested->GetInt() != -2 && requested->GetInt() != window_id)
        return RespondNow(Error("No active managed browser window"));
    }
  }
  const int tab_id = tab->ID();
  if (!action_->GetIsVisibleIgnoringDeclarative(tab_id) ||
      !action_->HasPopup(tab_id))
    return RespondNow(Error("No enabled popup for the selected tab"));
  using Pending = base::RefCountedData<base::OnceCallback<void(bool)>>;
  auto pending = base::MakeRefCounted<Pending>(base::BindOnce(
      [](scoped_refptr<ActionOpenPopupFunction> function, bool success) {
        if (success)
          function->Respond(NoArguments());
        else
          function->Respond(Error("Failed to open popup."));
      },
      scoped_refptr<ActionOpenPopupFunction>(this)));
  auto respond = base::BindRepeating(
      [](scoped_refptr<Pending> pending, bool success) {
        if (pending->data)
          std::move(pending->data).Run(success);
      }, pending);
  if (!tab->Emit("browser-extension-popup", extension_id(), respond))
    respond.Run(false);
  if (pending->data)
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(respond, false), base::Seconds(15));
  return did_respond() ? AlreadyResponded() : RespondLater();
}

}  // namespace extensions
