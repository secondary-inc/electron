// Copyright (c) 2026 Secondary, Inc.
// Use of this source code is governed by the MIT license.
#include "shell/browser/extensions/api/context_menus/context_menus_api.h"
#include <set>
#include "base/strings/string_number_conversions.h"
#include "base/json/json_writer.h"
#include "extensions/browser/pref_types.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/common/extension.h"
#include "extensions/common/url_pattern.h"
#include "url/gurl.h"
namespace extensions {
namespace {
constexpr char kPref[] = "iris_context_menus";
constexpr PrefMap kMenuPref{kPref, kDictionary, PrefScope::kExtensionSpecific};
bool PatternsValid(const base::Value* value) {
  if (!value) return true;
  if (!value->is_list() || value->GetList().size() > 100) return false;
  for (const auto& v : value->GetList()) {
    if (!v.is_string() || v.GetString().size() > 8192) return false;
    URLPattern p(URLPattern::SCHEME_HTTP | URLPattern::SCHEME_HTTPS);
    if (p.Parse(v.GetString()) != URLPattern::ParseResult::kSuccess) return false;
  }
  return true;
}
bool MatchesPatterns(const base::ListValue* patterns, const std::string& url) {
  if (!patterns) return true;
  for (const auto& v : *patterns) {
    URLPattern p(URLPattern::SCHEME_HTTP | URLPattern::SCHEME_HTTPS);
    if (v.is_string() && p.Parse(v.GetString()) == URLPattern::ParseResult::kSuccess && p.MatchesURL(GURL(url))) return true;
  }
  return false;
}
std::string Validate(base::DictValue& item, const base::DictValue& items, const std::string& key) {
  const auto* type = item.FindString("type");
  if (!type) { item.Set("type", "normal"); type = item.FindString("type"); }
  if (*type != "normal" && *type != "separator" && *type != "checkbox")
    return "This runtime supports normal, checkbox and separator website menu items";
  const auto* title = item.FindString("title");
  if (*type != "separator" && (!title || title->empty() || title->size() > 1000))
    return "A bounded nonempty menu title is required";
  if (const auto* contexts = item.Find("contexts")) {
    if (!contexts->is_list() || contexts->GetList().empty() || contexts->GetList().size() > 10)
      return "Invalid menu contexts";
    for (const auto& c : contexts->GetList())
      if (!c.is_string() || (c.GetString() != "all" && c.GetString() != "page" &&
          c.GetString() != "selection" && c.GetString() != "link" && c.GetString() != "editable" &&
          c.GetString() != "image" && c.GetString() != "audio" && c.GetString() != "video"))
        return "Only main-document website contexts are supported";
  } else {
    item.Set("contexts", base::ListValue().Append("page"));
  }
  for (const auto* flag : {"enabled", "visible", "checked"})
    if (item.Find(flag) && !item.Find(flag)->is_bool()) return "Invalid menu boolean";
  if (!PatternsValid(item.Find("documentUrlPatterns")) || !PatternsValid(item.Find("targetUrlPatterns")))
    return "Only valid HTTP(S) menu patterns are supported";
  if (const auto* parent = item.Find("parentId")) {
    auto parent_key = WorkContextMenuKey(*parent);
    std::set<std::string> seen{key};
    while (!parent_key.empty()) {
      if (!seen.insert(parent_key).second) return "Menu parent cycle";
      if (seen.size() > 21) return "Menu nesting exceeds 20 levels";
      const auto* p = items.FindDict(parent_key);
      if (!p || !p->FindString("type") || *p->FindString("type") != "normal")
        return "A normal parent menu item must already exist";
      const auto* next = p->Find("parentId");
      parent_key = next ? WorkContextMenuKey(*next) : "";
    }
  }
  item.Remove("onclick");
  item.Remove("generatedId");
  auto serialized = base::WriteJson(item);
  if (!serialized || serialized->size() > 32768) return "Menu item exceeds 32 KiB";
  return {};
}
}
std::string WorkContextMenuKey(const base::Value& id) {
  if (id.is_string() && !id.GetString().empty() && id.GetString().size() <= 200) return "s:" + id.GetString();
  if (id.is_int()) return "i:" + base::NumberToString(id.GetInt());
  return {};
}
base::DictValue ReadWorkContextMenus(content::BrowserContext* context, const std::string& id) {
  const auto* stored = ExtensionPrefs::Get(context)->ReadPrefAsDictionary(id, kMenuPref);
  if (stored && stored->FindInt("revision") && stored->FindDict("items") && stored->FindDict("items")->size() <= 100) {
    bool valid = true;
    const auto& items = *stored->FindDict("items");
    for (const auto [key, value] : items) {
      if (!value.is_dict()) { valid = false; break; }
      auto item = value.GetDict().Clone();
      const auto* item_id = item.Find("id");
      if (!item_id || WorkContextMenuKey(*item_id) != key || !Validate(item, items, key).empty()) {
        valid = false; break;
      }
    }
    if (valid) return stored->Clone();
  }
  return base::DictValue().Set("revision", 0).Set("items", base::DictValue());
}
void WriteWorkContextMenus(content::BrowserContext* context, const std::string& id, base::DictValue state) {
  const int previous = state.FindInt("revision").value_or(0);
  state.Set("revision", previous >= 2000000000 ? 1 : previous + 1);
  ExtensionPrefs::Get(context)->UpdateExtensionPref(id, kPref, base::Value(std::move(state)));
}
bool WorkContextMenuMatches(const base::DictValue& item, const base::DictValue& details) {
  if (!item.FindBool("visible").value_or(true)) return false;
  const auto text = [&details](const char* key) { const auto* value = details.FindString(key); return value ? *value : std::string(); };
  const auto* contexts = item.FindList("contexts");
  bool matched = false;
  if (contexts) for (const auto& c : *contexts) {
    if (!c.is_string()) continue;
    const auto& context = c.GetString();
    matched |= context == "all" || context == "page" ||
        (context == "selection" && !text("selectionText").empty()) ||
        (context == "link" && !text("linkUrl").empty()) ||
        (context == "editable" && details.FindBool("editable").value_or(false)) ||
        ((context == "image" || context == "audio" || context == "video") && context == text("mediaType"));
  }
  return matched && MatchesPatterns(item.FindList("documentUrlPatterns"), text("pageUrl")) &&
      (!item.FindList("targetUrlPatterns") ||
       MatchesPatterns(item.FindList("targetUrlPatterns"), text("linkUrl")) ||
       MatchesPatterns(item.FindList("targetUrlPatterns"), text("srcUrl")));
}
ExtensionFunction::ResponseAction ContextMenusCreateFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty() && args()[0].is_dict());
  auto item = args()[0].GetDict().Clone();
  const auto* id = item.Find("id");
  if (!id && extension()->manifest_version() >= 3) return RespondNow(Error("An explicit menu ID is required for MV3"));
  if (!id) id = item.Find("generatedId");
  if (!id) return RespondNow(Error("Menu ID missing"));
  const auto key = WorkContextMenuKey(*id);
  if (key.empty()) return RespondNow(Error("Invalid menu ID"));
  auto copied_id = id->Clone();
  auto state = ReadWorkContextMenus(browser_context(), extension_id());
  auto* items = state.FindDict("items");
  if (items->size() >= 100 || items->contains(key)) return RespondNow(Error("Duplicate menu ID or 100-item limit reached"));
  item.Set("id", std::move(copied_id));
  auto error = Validate(item, *items, key);
  if (!error.empty()) return RespondNow(Error(error));
  item.Set("order", state.FindInt("revision").value_or(0));
  items->Set(key, std::move(item));
  WriteWorkContextMenus(browser_context(), extension_id(), std::move(state));
  return RespondNow(NoArguments());
}
ExtensionFunction::ResponseAction ContextMenusUpdateFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(args().size() >= 2 && args()[1].is_dict());
  const auto key = WorkContextMenuKey(args()[0]);
  auto state = ReadWorkContextMenus(browser_context(), extension_id());
  auto* items = state.FindDict("items");
  auto* existing = items->FindDict(key);
  if (!existing) return RespondNow(Error("Menu item not found"));
  auto item = existing->Clone();
  for (const auto [name, value] : args()[1].GetDict())
    if (name != "id" && name != "order" && name != "generatedId") item.Set(name, value.Clone());
  auto error = Validate(item, *items, key);
  if (!error.empty()) return RespondNow(Error(error));
  items->Set(key, std::move(item));
  // Reparenting or changing a parent type must not invalidate descendants.
  for (const auto [child_key, value] : *items) {
    auto child = value.GetDict().Clone();
    error = Validate(child, *items, child_key);
    if (!error.empty()) return RespondNow(Error(error));
  }
  WriteWorkContextMenus(browser_context(), extension_id(), std::move(state));
  return RespondNow(NoArguments());
}
ExtensionFunction::ResponseAction ContextMenusRemoveFunction::Run() {
  EXTENSION_FUNCTION_VALIDATE(!args().empty());
  const auto key = WorkContextMenuKey(args()[0]);
  auto state = ReadWorkContextMenus(browser_context(), extension_id());
  auto* items = state.FindDict("items");
  if (!items->contains(key)) return RespondNow(Error("Menu item not found"));
  std::set<std::string> removed{key};
  bool added = true;
  while (added) {
    added = false;
    for (const auto [name, value] : *items) {
      const auto* parent = value.is_dict() ? value.GetDict().Find("parentId") : nullptr;
      if (parent && removed.contains(WorkContextMenuKey(*parent)) && removed.insert(name).second) added = true;
    }
  }
  for (const auto& name : removed) items->Remove(name);
  WriteWorkContextMenus(browser_context(), extension_id(), std::move(state));
  return RespondNow(NoArguments());
}
ExtensionFunction::ResponseAction ContextMenusRemoveAllFunction::Run() {
  auto state = ReadWorkContextMenus(browser_context(), extension_id());
  state.Set("items", base::DictValue());
  WriteWorkContextMenus(browser_context(), extension_id(), std::move(state));
  return RespondNow(NoArguments());
}
}
