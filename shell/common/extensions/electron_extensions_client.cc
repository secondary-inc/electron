// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/common/extensions/electron_extensions_client.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/no_destructor.h"
#include "base/notimplemented.h"
#include "components/version_info/version_info.h"
#include "extensions/common/core_extensions_api_provider.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/features/simple_feature.h"
#include "extensions/common/permissions/permission_message_provider.h"
#include "extensions/common/url_pattern_set.h"
#include "shell/common/application_info.h"
#include "extensions/common/user_scripts_availability.h"
#include "shell/common/extensions/electron_extensions_api_provider.h"

using extensions::ExtensionsClient;

namespace electron {

namespace {

// Chromium's immutable core feature owns webstorePrivate. Its delegated check
// must not enable the core testing branch for arbitrary websites. Document,
// managed-window, visibility and gesture checks remain in the native API.
bool IsOfficialWebStoreContext(
    std::string_view api_full_name,
    const extensions::Extension* extension,
    extensions::mojom::ContextType context,
    const GURL& url,
    extensions::Feature::Platform platform,
    int context_id,
    bool check_developer_mode,
    const extensions::ContextData& context_data) {
  return !extension && context == extensions::mojom::ContextType::kWebPage &&
         url.is_valid() && url.SchemeIs("https") &&
         url.host_piece() == "chromewebstore.google.com" &&
         url.EffectiveIntPort() == 443 && !url.has_username() &&
         !url.has_password();
}

// TODO(jamescook): Refactor ChromePermissionsMessageProvider so we can share
// code. For now, this implementation does nothing.
class ElectronPermissionMessageProvider
    : public extensions::PermissionMessageProvider {
 public:
  ElectronPermissionMessageProvider() = default;
  ~ElectronPermissionMessageProvider() override = default;

  // disable copy
  ElectronPermissionMessageProvider(const ElectronPermissionMessageProvider&) =
      delete;
  ElectronPermissionMessageProvider& operator=(
      const ElectronPermissionMessageProvider&) = delete;

  // PermissionMessageProvider implementation.
  [[nodiscard]] extensions::PermissionMessages GetPermissionMessages(
      const extensions::PermissionIDSet& permissions) const override {
    return extensions::PermissionMessages();
  }

  [[nodiscard]] bool IsPrivilegeIncrease(
      const extensions::PermissionSet& granted_permissions,
      const extensions::PermissionSet& requested_permissions,
      extensions::Manifest::Type extension_type) const override {
    // Ensure we implement this before shipping.
    NOTREACHED();
  }

  [[nodiscard]] extensions::PermissionIDSet GetAllPermissionIDs(
      const extensions::PermissionSet& permissions,
      extensions::Manifest::Type extension_type) const override {
    return {};
  }
};

}  // namespace

ElectronExtensionsClient::ElectronExtensionsClient()
    : webstore_base_url_(extension_urls::kChromeWebstoreBaseURL),
      new_webstore_base_url_(extension_urls::kNewChromeWebstoreBaseURL),
      webstore_update_url_(extension_urls::kChromeWebstoreUpdateURL) {
  auto availability =
      extensions::user_scripts_availability::CreateAvailabilityCheckMap();
  availability.emplace("webstorePrivate", &IsOfficialWebStoreContext);
  SetFeatureDelegatedAvailabilityCheckMap(std::move(availability));
  AddAPIProvider(std::make_unique<extensions::CoreExtensionsAPIProvider>());
  AddAPIProvider(std::make_unique<ElectronExtensionsAPIProvider>());
}

ElectronExtensionsClient::~ElectronExtensionsClient() = default;

void ElectronExtensionsClient::Initialize() {
  // TODO(jamescook): Do we need to whitelist any extensions?
}

void ElectronExtensionsClient::InitializeWebStoreUrls(
    base::CommandLine* command_line) {}

const extensions::PermissionMessageProvider&
ElectronExtensionsClient::GetPermissionMessageProvider() const {
  NOTIMPLEMENTED();

  static base::NoDestructor<ElectronPermissionMessageProvider> instance;
  return *instance;
}

const std::string ElectronExtensionsClient::GetProductName() {
  return GetApplicationName();
}

void ElectronExtensionsClient::FilterHostPermissions(
    const extensions::URLPatternSet& hosts,
    extensions::URLPatternSet* new_hosts,
    extensions::PermissionIDSet* permissions) const {
  NOTIMPLEMENTED();
}

void ElectronExtensionsClient::SetScriptingAllowlist(
    const ExtensionsClient::ScriptingAllowlist& allowlist) {
  scripting_allowlist_ = allowlist;
}

const ExtensionsClient::ScriptingAllowlist&
ElectronExtensionsClient::GetScriptingAllowlist() const {
  // TODO(jamescook): Real whitelist.
  return scripting_allowlist_;
}

extensions::URLPatternSet
ElectronExtensionsClient::GetPermittedChromeSchemeHosts(
    const extensions::Extension* extension,
    const extensions::APIPermissionSet& api_permissions) const {
  return {};
}

bool ElectronExtensionsClient::IsScriptableURL(const GURL& url,
                                               std::string* error) const {
  // No restrictions on URLs.
  return true;
}

const GURL& ElectronExtensionsClient::GetWebstoreBaseURL() const {
  return webstore_base_url_;
}

const GURL& ElectronExtensionsClient::GetNewWebstoreBaseURL() const {
  return new_webstore_base_url_;
}

const GURL& ElectronExtensionsClient::GetWebstoreUpdateURL() const {
  return webstore_update_url_;
}

bool ElectronExtensionsClient::IsBlocklistUpdateURL(const GURL& url) const {
  return false;
}

}  // namespace electron
