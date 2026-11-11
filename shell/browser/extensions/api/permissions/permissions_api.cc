// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/api/permissions/permissions_api.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/permissions/permissions_updater.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/user_script_manager.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/value_converter.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_handlers/permissions_parser.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/browser/api/permissions/permissions_api_helpers.h"
#include "extensions/common/api/permissions.h"

namespace extensions {
// Chromium's PermissionsEventRouter observes permission updates and dispatches
// events. Keep Iris consent handling here without emitting duplicate events.

ExtensionFunction::ResponseAction ElectronPermissionsContainsFunction::Run() {
  auto params = api::permissions::Contains::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  std::string error;
  auto unpacked = permissions_api_helpers::UnpackPermissionSet(
      params->permissions,
      PermissionsParser::GetRequiredPermissions(extension()),
      PermissionsParser::GetOptionalPermissions(extension()),
      ExtensionPrefs::Get(browser_context())->AllowFileAccess(extension()->id()),
      &error);
  if (!unpacked)
    return RespondNow(Error(std::move(error)));

  const auto& active = extension()->permissions_data()->active_permissions();
  const bool contains =
      unpacked->unlisted_apis.empty() &&
      unpacked->unlisted_hosts.is_empty() &&
      unpacked->restricted_file_scheme_patterns.is_empty() &&
      active.apis().Contains(unpacked->optional_apis) &&
      active.apis().Contains(unpacked->required_apis) &&
      active.explicit_hosts().Contains(unpacked->optional_explicit_hosts) &&
      active.explicit_hosts().Contains(unpacked->required_explicit_hosts) &&
      active.scriptable_hosts().Contains(unpacked->required_scriptable_hosts);
  return RespondNow(ArgumentList(
      api::permissions::Contains::Results::Create(contains)));
}

ExtensionFunction::ResponseAction ElectronPermissionsGetAllFunction::Run() {
  auto permissions = permissions_api_helpers::PackPermissionSet(
      extension()->permissions_data()->active_permissions());
  return RespondNow(ArgumentList(
      api::permissions::GetAll::Results::Create(*permissions)));
}

ExtensionFunction::ResponseAction ElectronPermissionsRequestFunction::Run() {
  auto params = api::permissions::Request::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  if (!user_gesture())
    return RespondNow(Error("This function must be called during a user gesture"));
  std::string error;
  auto unpacked = permissions_api_helpers::UnpackPermissionSet(
      params->permissions, PermissionsParser::GetRequiredPermissions(extension()),
      PermissionsParser::GetOptionalPermissions(extension()),
      ExtensionPrefs::Get(browser_context())->AllowFileAccess(extension_id()), &error);
  if (!unpacked) return RespondNow(Error(std::move(error)));
  if (!unpacked->unlisted_apis.empty() || !unpacked->unlisted_hosts.is_empty() ||
      !unpacked->restricted_file_scheme_patterns.is_empty())
    return RespondNow(Error("Only declared optional permissions with allowed file access may be requested"));
  auto requested = std::make_unique<const PermissionSet>(
      std::move(unpacked->optional_apis), ManifestPermissionSet(),
      std::move(unpacked->optional_explicit_hosts), URLPatternSet());
  auto missing = PermissionSet::CreateDifference(
      *requested, extension()->permissions_data()->active_permissions());
  if (missing->IsEmpty())
    return RespondNow(ArgumentList(api::permissions::Request::Results::Create(true)));
  auto* tab = GetActiveElectronTab(browser_context());
  if (!tab || ElectronBrowserTabWindowId(tab->web_contents()) < 0)
    return RespondNow(ArgumentList(api::permissions::Request::Results::Create(false)));
  auto details = permissions_api_helpers::PackPermissionSet(*missing)->ToValue();
  using Pending = base::RefCountedData<base::OnceCallback<void(bool)>>;
  auto pending = base::MakeRefCounted<Pending>(base::BindOnce(
      [](scoped_refptr<ElectronPermissionsRequestFunction> function,
         std::unique_ptr<const PermissionSet> requested, bool allow) {
        auto* context = function->browser_context();
        if (!allow || !context ||
            ExtensionRegistry::Get(context)->enabled_extensions().GetByID(function->extension_id()) != function->extension()) {
          function->Respond(function->ArgumentList(api::permissions::Request::Results::Create(false)));
          return;
        }
        auto changed = PermissionSet::CreateDifference(
            *requested, function->extension()->permissions_data()->active_permissions());
        PermissionsUpdater(context).GrantOptionalPermissions(*function->extension(), *changed,
            base::BindOnce([](scoped_refptr<ElectronPermissionsRequestFunction> function) {
              function->Respond(function->ArgumentList(api::permissions::Request::Results::Create(true)));
            }, function));
      }, scoped_refptr<ElectronPermissionsRequestFunction>(this), std::move(missing)));
  auto respond = base::BindRepeating([](scoped_refptr<Pending> pending, bool allow) {
    if (pending->data) std::move(pending->data).Run(allow);
  }, pending);
  if (!tab->Emit("browser-extension-permissions", extension_id(), std::move(details), respond))
    respond.Run(false);
  if (pending->data)
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(respond, false), base::Minutes(2));
  return did_respond() ? AlreadyResponded() : RespondLater();
}

ExtensionFunction::ResponseAction ElectronPermissionsRemoveFunction::Run() {
  auto params = api::permissions::Remove::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  std::string error;
  auto unpacked = permissions_api_helpers::UnpackPermissionSet(
      params->permissions, PermissionsParser::GetRequiredPermissions(extension()),
      PermissionsParser::GetOptionalPermissions(extension()),
      ExtensionPrefs::Get(browser_context())->AllowFileAccess(extension_id()), &error);
  if (!unpacked) return RespondNow(Error(std::move(error)));
  if (!unpacked->unlisted_apis.empty() || !unpacked->unlisted_hosts.is_empty())
    return RespondNow(Error("Only declared optional permissions may be removed"));
  if (!unpacked->required_apis.empty() || !unpacked->required_explicit_hosts.is_empty() ||
      !unpacked->required_scriptable_hosts.is_empty())
    return RespondNow(Error("You cannot remove required permissions."));
  PermissionSet requested(std::move(unpacked->optional_apis), ManifestPermissionSet(),
                          std::move(unpacked->optional_explicit_hosts), URLPatternSet());
  auto changed = PermissionSet::CreateIntersection(
      requested, extension()->permissions_data()->active_permissions());
  if (requested.HasAPIPermission(mojom::APIPermissionID::kUserScripts)) {
    ExtensionSystem::Get(browser_context())->user_script_manager()
        ->SetUserScriptPrefEnabled(extension_id(), false);
  }
  PermissionsUpdater(browser_context()).RevokeOptionalPermissions(
      *extension(), *changed, PermissionsUpdater::RemoveType::kSoft,
      base::BindOnce([](scoped_refptr<ElectronPermissionsRemoveFunction> function) {
        function->Respond(function->ArgumentList(api::permissions::Remove::Results::Create(true)));
      }, scoped_refptr<ElectronPermissionsRemoveFunction>(this)));
  return did_respond() ? AlreadyResponded() : RespondLater();
}

}  // namespace extensions
