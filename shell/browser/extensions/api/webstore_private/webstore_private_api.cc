// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "shell/browser/extensions/api/webstore_private/webstore_private_api.h"

#include <map>
#include <memory>
#include <type_traits>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/timer/timer.h"
#include "base/time/time.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/weak_document_ptr.h"
#include "extensions/browser/api/management/management_api.h"
#include "extensions/common/api/management.h"
#include "extensions/browser/extension_function_registry.h"
#include "shell/browser/api/electron_api_web_contents.h"
#include "shell/browser/extensions/electron_extension_tab_util.h"
#include "shell/browser/native_window.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/value_converter.h"

namespace extensions {
struct StoreDocumentCheck { base::RepeatingCallback<bool()> callback; };
}
namespace gin {
template <> struct Converter<extensions::StoreDocumentCheck> {
  static v8::Local<v8::Value> ToV8(v8::Isolate* isolate, const extensions::StoreDocumentCheck& check) {
    return gin_helper::CallbackToV8Leaked(isolate, check.callback);
  }
};
}
namespace extensions {
namespace {
using Reply = base::OnceCallback<void(std::string, base::ListValue)>;
using Counts = std::map<int, int>;
Counts& PendingCounts() {
  static base::NoDestructor<Counts> counts;
  return *counts;
}
bool IsStoreDocument(content::RenderFrameHost* frame,
                     electron::api::WebContents* owner) {
  auto* contents = owner ? owner->web_contents() : nullptr;
  return contents && frame && contents->GetPrimaryMainFrame() == frame &&
      frame->IsRenderFrameLive() &&
      frame->GetLastCommittedOrigin().Serialize() == "https://chromewebstore.google.com" &&
      contents->GetLastCommittedURL().SchemeIs("https") &&
      contents->GetVisibility() == content::Visibility::VISIBLE &&
      ElectronBrowserTabWindowId(contents) > 0 && owner->owner_window() &&
      owner->owner_window()->base_window_id() == ElectronBrowserTabWindowId(contents) &&
      owner->owner_window()->IsVisible() && !owner->owner_window()->IsMinimized();
}
void DispatchStorePage(ExtensionFunction* function, base::ListValue args, Reply reply) {
  auto* contents = function->GetSenderWebContents();
  auto* owner = contents ? electron::api::WebContents::From(contents) : nullptr;
  auto* frame = function->render_frame_host();
  if (function->extension() || !IsStoreDocument(frame, owner)) {
    std::move(reply).Run("Web Store APIs require a visible managed store document", {});
    return;
  }
  const std::string method = function->name();
  const bool gesture = method == "webstorePrivate.beginInstallWithManifest3" ||
      method == "management.setEnabled" || method == "management.uninstall";
  if (gesture && !function->user_gesture()) {
    std::move(reply).Run("A user gesture is required", {});
    return;
  }
  const auto serialized = base::WriteJson(args);
  if (!serialized || serialized->size() > 300 * 1024) {
    std::move(reply).Run("Store request is too large", {});
    return;
  }
  const int id = owner->ID();
  if (PendingCounts().size() >= 64 || PendingCounts()[id] >= 8) {
    std::move(reply).Run("Too many pending store requests", {});
    return;
  }
  ++PendingCounts()[id];
  struct Pending {
    Reply reply;
    base::OneShotTimer timer;
    base::WeakPtr<electron::api::WebContents> owner;
    content::WeakDocumentPtr document;
    int id;
    base::TimeTicks expires = base::TimeTicks::Now() + base::Minutes(2);
    bool Valid() {
      return base::TimeTicks::Now() < expires && IsStoreDocument(document.AsRenderFrameHostIfValid(), owner.get());
    }
    void Finish(base::DictValue response) {
      if (!reply) return;
      timer.Stop();
      std::string error;
      if (!Valid()) error = "Store document changed";
      else if (auto* message = response.FindString("error")) error = message->substr(0, 1000);
      auto* result = response.FindList("result");
      if (error.empty() && !result) error = "Invalid store response";
      if (--PendingCounts()[id] == 0) PendingCounts().erase(id);
      std::move(reply).Run(std::move(error), result ? result->Clone() : base::ListValue());
    }
  };
  auto pending = std::make_shared<Pending>();
  pending->reply = std::move(reply);
  pending->owner = owner->GetWeakPtr();
  pending->document = frame->GetWeakDocumentPtr();
  pending->id = id;
  auto respond = base::BindRepeating([](std::shared_ptr<Pending> pending, base::DictValue response) {
    pending->Finish(std::move(response));
  }, pending);
  auto valid = base::BindRepeating([](std::shared_ptr<Pending> pending) {
    return pending->Valid();
  }, pending);
  if (!owner->Emit("browser-store-request", method, std::move(args), respond, StoreDocumentCheck{valid})) {
    base::DictValue response;
    response.Set("error", "Store integration is unavailable");
    respond.Run(std::move(response));
  }
  if (pending->reply) pending->timer.Start(FROM_HERE, base::Minutes(2),
      base::BindOnce([](std::shared_ptr<Pending> pending) {
        base::DictValue response;
        response.Set("error", "Store request expired");
        pending->Finish(std::move(response));
      }, pending));
}
// ManagementUninstallFunction's destructor is private. Preserve its exact
// argument decoding and delegate to the same protected Uninstall implementation.
class StoreManagementUninstallBase : public ManagementUninstallFunctionBase {
 public:
  DECLARE_EXTENSION_FUNCTION("management.uninstall", MANAGEMENT_UNINSTALL)
 protected:
  ~StoreManagementUninstallBase() override = default;
  ResponseAction Run() override {
    auto params = api::management::Uninstall::Params::Create(args());
    EXTENSION_FUNCTION_VALIDATE(params);
    return Uninstall(params->id,
        params->options && params->options->show_confirm_dialog.value_or(false));
  }
};
template <class Base>
class StorePageFunction : public Base {
 protected:
  typename ExtensionFunction::ResponseAction Run() override {
    if constexpr (!std::is_same_v<Base, ExtensionFunction>) {
      if (this->source_context_type() != mojom::ContextType::kWebPage)
        return Base::Run();
    }
    DispatchStorePage(this, this->args().Clone(),
        base::BindOnce([](scoped_refptr<StorePageFunction<Base>> function,
                          std::string error, base::ListValue result) {
          if (error.empty()) function->Respond(function->ArgumentList(std::move(result)));
          else function->Respond(function->Error(std::move(error)));
        }, scoped_refptr<StorePageFunction<Base>>(this)));
    return this->did_respond() ? this->AlreadyResponded() : this->RespondLater();
  }
  ~StorePageFunction() override = default;
};
#define STORE_FUNCTION(Class, Name, Histogram) \
class Class : public StorePageFunction<ExtensionFunction> { \
 public: DECLARE_EXTENSION_FUNCTION(Name, Histogram) \
 protected: ~Class() override = default; \
};
STORE_FUNCTION(StoreBegin, "webstorePrivate.beginInstallWithManifest3", WEBSTOREPRIVATE_BEGININSTALLWITHMANIFEST3)
STORE_FUNCTION(StoreComplete, "webstorePrivate.completeInstall", WEBSTOREPRIVATE_COMPLETEINSTALL)
STORE_FUNCTION(StoreStatus, "webstorePrivate.getExtensionStatus", WEBSTOREPRIVATE_GETEXTENSIONSTATUS)
STORE_FUNCTION(StoreVersion, "webstorePrivate.getFullChromeVersion", WEBSTOREPRIVATE_GETFULLCHROMEVERSION)
STORE_FUNCTION(StoreIncognito, "webstorePrivate.isInIncognitoMode", WEBSTOREPRIVATE_ISININCOGNITOMODEFUNCTION)
STORE_FUNCTION(StoreReferrer, "webstorePrivate.getReferrerChain", WEBSTOREPRIVATE_GETREFERRERCHAIN)
STORE_FUNCTION(StorePromotion, "webstorePrivate.shouldShowEnterprisePromotionBanner", WEBSTOREPRIVATE_SHOULDSHOWENTERPRISEPROMOTIONBANNER)
#undef STORE_FUNCTION
}  // namespace
void RegisterStorePageFunctions(ExtensionFunctionRegistry* registry) {
  registry->RegisterFunction<StoreBegin>();
  registry->RegisterFunction<StoreComplete>();
  registry->RegisterFunction<StoreStatus>();
  registry->RegisterFunction<StoreVersion>();
  registry->RegisterFunction<StoreIncognito>();
  registry->RegisterFunction<StoreReferrer>();
  registry->RegisterFunction<StorePromotion>();
  registry->RegisterFunction<StorePageFunction<ManagementGetAllFunction>>();
  registry->RegisterFunction<StorePageFunction<ManagementGetFunction>>();
  registry->RegisterFunction<StorePageFunction<ManagementSetEnabledFunction>>();
  registry->RegisterFunction<StorePageFunction<StoreManagementUninstallBase>>();
}
}  // namespace extensions
