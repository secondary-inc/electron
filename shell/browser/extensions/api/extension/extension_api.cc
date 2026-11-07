// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "shell/browser/extensions/api/extension/extension_api.h"
#include "extensions/browser/extension_util.h"

namespace extensions {
ExtensionFunction::ResponseAction
ExtensionIsAllowedIncognitoAccessFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().empty());
  return RespondNow(WithArguments(
      util::IsIncognitoEnabled(extension_id(), browser_context())));
}
ExtensionFunction::ResponseAction
ExtensionIsAllowedFileSchemeAccessFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().empty());
  return RespondNow(WithArguments(
      util::AllowFileAccess(extension_id(), browser_context())));
}
}  // namespace extensions
