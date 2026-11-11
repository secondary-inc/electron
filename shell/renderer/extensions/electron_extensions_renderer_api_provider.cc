// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/renderer/extensions/electron_extensions_renderer_api_provider.h"

#include "chrome/renderer/extensions/api/extension_hooks_delegate.h"
#include "electron/grit/electron_resources.h"
#include "chrome/renderer/extensions/api/tabs_hooks_delegate.h"
#include "extensions/renderer/bindings/api_bindings_system.h"
#include "extensions/renderer/dispatcher.h"
#include "extensions/renderer/lazy_background_page_native_handler.h"
#include "extensions/renderer/module_system.h"
#include "extensions/renderer/native_extension_bindings_system.h"
#include "extensions/renderer/native_handler.h"
#include "extensions/renderer/resource_bundle_source_map.h"
#include "extensions/renderer/script_context.h"

namespace electron {

void ElectronExtensionsRendererAPIProvider::RegisterNativeHandlers(
    extensions::ModuleSystem* module_system,
    extensions::NativeExtensionBindingsSystem* bindings_system,
    extensions::V8SchemaRegistry* v8_schema_registry,
    extensions::ScriptContext* context) const {
  module_system->RegisterNativeHandler(
      "lazy_background_page",
      std::make_unique<extensions::LazyBackgroundPageNativeHandler>(context));
}

void ElectronExtensionsRendererAPIProvider::AddBindingsSystemHooks(
    extensions::Dispatcher* dispatcher,
    extensions::NativeExtensionBindingsSystem* bindings_system) const {
  extensions::APIBindingsSystem* bindings = bindings_system->api_system();
  bindings->RegisterHooksDelegate(
      "extension", std::make_unique<extensions::ExtensionHooksDelegate>(
                       bindings_system->messaging_service()));
  bindings->RegisterHooksDelegate(
      "tabs", std::make_unique<extensions::TabsHooksDelegate>(
                  bindings_system->messaging_service()));
}

void ElectronExtensionsRendererAPIProvider::PopulateSourceMap(
    extensions::ResourceBundleSourceMap* source_map) const {
  static constexpr auto kSources =
      base::MakeFixedFlatMap<std::string_view, int>({
          {"action", IDR_IRIS_ACTION_BINDINGS},
          {"browserAction", IDR_IRIS_BROWSER_ACTION_BINDINGS},
          {"pageAction", IDR_IRIS_PAGE_ACTION_BINDINGS},
      });
  source_map->RegisterSources(kSources);
}

void ElectronExtensionsRendererAPIProvider::EnableCustomElementAllowlist()
    const {}

void ElectronExtensionsRendererAPIProvider::RequireWebViewModules(
    extensions::ScriptContext* context) const {}

}  // namespace electron
