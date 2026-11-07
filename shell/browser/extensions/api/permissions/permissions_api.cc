// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/api/permissions/permissions_api.h"

#include <utility>

#include "extensions/browser/extension_prefs.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_handlers/permissions_parser.h"
#include "extensions/common/permissions/permissions_data.h"
#include "shell/browser/extensions/api/permissions/permissions_api_helpers.h"
#include "shell/common/extensions/api/permissions.h"

namespace extensions {

ExtensionFunction::ResponseAction PermissionsContainsFunction::Run() {
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

ExtensionFunction::ResponseAction PermissionsGetAllFunction::Run() {
  auto permissions = permissions_api_helpers::PackPermissionSet(
      extension()->permissions_data()->active_permissions());
  return RespondNow(ArgumentList(
      api::permissions::GetAll::Results::Create(*permissions)));
}

ExtensionFunction::ResponseAction PermissionsRequestFunction::Run() {
  auto params = api::permissions::Request::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  // No implicit grants. Mutation needs a trusted embedder consent UI and
  // synchronized permission persistence, renderer updates and event dispatch.
  return RespondNow(Error(
      "Optional extension permission requests are not supported by this host"));
}

ExtensionFunction::ResponseAction PermissionsRemoveFunction::Run() {
  auto params = api::permissions::Remove::Params::Create(args());
  EXTENSION_FUNCTION_VALIDATE(params);
  return RespondNow(Error(
      "Extension permission removal is not supported by this host"));
}

}  // namespace extensions
