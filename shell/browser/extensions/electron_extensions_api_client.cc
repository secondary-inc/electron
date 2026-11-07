// Copyright (c) 2019 Slack Technologies, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/electron_extensions_api_client.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "electron/buildflags/buildflags.h"
#include "components/value_store/value_store_factory.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/api/storage/backend_task_runner.h"
#include "extensions/browser/api/storage/settings_storage_quota_enforcer.h"
#include "extensions/browser/api/storage/value_store_cache.h"
#include "extensions/browser/api/storage/value_store_util.h"
#include "extensions/common/api/storage.h"
#include "extensions/common/extension.h"
#include "extensions/browser/guest_view/extensions_guest_view_manager_delegate.h"
#include "extensions/browser/guest_view/mime_handler_view/mime_handler_view_guest_delegate.h"
#include "printing/buildflags/buildflags.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/extensions/api/management/electron_management_api_delegate.h"
#include "shell/browser/extensions/electron_extension_web_contents_observer.h"
#include "shell/browser/extensions/electron_messaging_delegate.h"
#include "shell/common/gin_helper/handle.h"
#include "v8/include/v8.h"

#if BUILDFLAG(ENABLE_PRINTING)
#include "components/printing/browser/print_manager_utils.h"
#include "shell/browser/printing/print_view_manager_electron.h"
#endif

namespace extensions {

namespace {
// chrome.storage.sync remains usable offline, as in a signed-out browser.
// This is a separate profile-local namespace, NOT a cloud synchronization service.
class OfflineSyncValueStoreCache final : public ValueStoreCache {
 public:
  explicit OfflineSyncValueStoreCache(
      scoped_refptr<value_store::ValueStoreFactory> factory)
      : factory_(std::move(factory)) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  }
  ~OfflineSyncValueStoreCache() override { DCHECK(IsOnBackendSequence()); }

  void RunWithValueStoreForExtension(
      StorageCallback callback,
      scoped_refptr<const Extension> extension) override {
    DCHECK(IsOnBackendSequence());
    auto& store = stores_[extension->id()];
    if (!store) {
      const SettingsStorageQuotaEnforcer::Limits limits = {
          static_cast<size_t>(api::storage::sync::QUOTA_BYTES),
          static_cast<size_t>(api::storage::sync::QUOTA_BYTES_PER_ITEM),
          static_cast<size_t>(api::storage::sync::MAX_ITEMS)};
      auto backend = value_store_util::CreateSettingsStore(
          settings_namespace::SYNC,
          extension->is_app() ? value_store_util::ModelType::APP
                             : value_store_util::ModelType::EXTENSION,
          extension->id(), factory_);
      store = std::make_unique<SettingsStorageQuotaEnforcer>(
          limits, std::move(backend));
    }
    std::move(callback).Run(store.get());
  }

  void DeleteStorageSoon(const ExtensionId& extension_id) override {
    DCHECK(IsOnBackendSequence());
    stores_.erase(extension_id);
    for (auto model : {value_store_util::ModelType::APP,
                       value_store_util::ModelType::EXTENSION}) {
      value_store_util::DeleteValueStore(settings_namespace::SYNC, model,
                                        extension_id, factory_);
    }
  }

 private:
  const scoped_refptr<value_store::ValueStoreFactory> factory_;
  std::map<ExtensionId, std::unique_ptr<value_store::ValueStore>> stores_;
};
}  // namespace

class ElectronGuestViewManagerDelegate
    : public ExtensionsGuestViewManagerDelegate {
 public:
  ElectronGuestViewManagerDelegate() = default;
  ~ElectronGuestViewManagerDelegate() override = default;

  // disable copy
  ElectronGuestViewManagerDelegate(const ElectronGuestViewManagerDelegate&) =
      delete;
  ElectronGuestViewManagerDelegate& operator=(
      const ElectronGuestViewManagerDelegate&) = delete;

  // GuestViewManagerDelegate:
  void OnGuestAdded(content::WebContents* guest_web_contents) const override {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);
    electron::api::WebContents::FromOrCreate(isolate, guest_web_contents);
  }
};

class ElectronMimeHandlerViewGuestDelegate
    : public MimeHandlerViewGuestDelegate {
 public:
  ElectronMimeHandlerViewGuestDelegate() = default;
  ~ElectronMimeHandlerViewGuestDelegate() override = default;

  // disable copy
  ElectronMimeHandlerViewGuestDelegate(
      const ElectronMimeHandlerViewGuestDelegate&) = delete;
  ElectronMimeHandlerViewGuestDelegate& operator=(
      const ElectronMimeHandlerViewGuestDelegate&) = delete;

  // MimeHandlerViewGuestDelegate.
  bool HandleContextMenu(content::RenderFrameHost& render_frame_host,
                         const content::ContextMenuParams& params) override {
    auto* web_contents =
        content::WebContents::FromRenderFrameHost(&render_frame_host);
    if (!web_contents)
      return true;

    electron::api::WebContents* api_web_contents =
        electron::api::WebContents::From(
            web_contents->GetOutermostWebContents());
    if (api_web_contents)
      api_web_contents->HandleContextMenu(render_frame_host, params);
    return true;
  }

  void RecordLoadMetric(bool in_main_frame,
                        const std::string& mime_type,
                        content::BrowserContext* browser_context) override {}
};

ElectronExtensionsAPIClient::ElectronExtensionsAPIClient() = default;
ElectronExtensionsAPIClient::~ElectronExtensionsAPIClient() = default;

void ElectronExtensionsAPIClient::AddAdditionalValueStoreCaches(
    content::BrowserContext*,
    const scoped_refptr<value_store::ValueStoreFactory>& factory,
    SettingsChangedCallback,
    std::map<settings_namespace::Namespace,
             raw_ptr<ValueStoreCache, CtnExperimental>>* caches) {
  (*caches)[settings_namespace::SYNC] = new OfflineSyncValueStoreCache(factory);
}

MessagingDelegate* ElectronExtensionsAPIClient::GetMessagingDelegate() {
  if (!messaging_delegate_)
    messaging_delegate_ = std::make_unique<ElectronMessagingDelegate>();
  return messaging_delegate_.get();
}

void ElectronExtensionsAPIClient::AttachWebContentsHelpers(
    content::WebContents* web_contents) const {
#if BUILDFLAG(ENABLE_PRINTING)
  electron::PrintViewManagerElectron::CreateForWebContents(web_contents);
  printing::CreateCompositeClientIfNeeded(web_contents, std::string());
#endif

  extensions::ElectronExtensionWebContentsObserver::CreateForWebContents(
      web_contents);
}

ManagementAPIDelegate*
ElectronExtensionsAPIClient::CreateManagementAPIDelegate() const {
  return new ElectronManagementAPIDelegate;
}

std::unique_ptr<MimeHandlerViewGuestDelegate>
ElectronExtensionsAPIClient::CreateMimeHandlerViewGuestDelegate(
    MimeHandlerViewGuest* guest) const {
  return std::make_unique<ElectronMimeHandlerViewGuestDelegate>();
}

std::unique_ptr<guest_view::GuestViewManagerDelegate>
ElectronExtensionsAPIClient::CreateGuestViewManagerDelegate() const {
  return std::make_unique<ElectronGuestViewManagerDelegate>();
}

}  // namespace extensions
