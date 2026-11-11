// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "shell/browser/extensions/api/font_settings/font_settings_api.h"
#include "base/functional/bind.h"
#include "content/public/browser/font_list_async.h"
namespace extensions {
ExtensionFunction::ResponseAction FontSettingsGetFontListFunction::Run() {
  content::GetFontListAsync(
      base::BindOnce(&FontSettingsGetFontListFunction::FontListHasLoaded, this));
  return RespondLater();
}
void FontSettingsGetFontListFunction::FontListHasLoaded(base::ListValue fonts) {
  base::ListValue result;
  for (const auto& entry : fonts) {
    if (!entry.is_list()) continue;
    const auto& pair = entry.GetList();
    if (pair.size() < 2 || !pair[0].is_string() || !pair[1].is_string()) continue;
    result.Append(base::DictValue().Set("fontId", pair[0].GetString())
                                  .Set("displayName", pair[1].GetString()));
  }
  Respond(WithArguments(std::move(result)));
}
}  // namespace extensions
