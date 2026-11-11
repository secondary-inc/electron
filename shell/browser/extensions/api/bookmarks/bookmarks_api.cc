// Copyright 2026 Electron contributors.
#include "shell/browser/extensions/api/bookmarks/bookmarks_api.h"
#include <map>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/timer/timer.h"
#include "extensions/browser/extension_function.h"
#include "extensions/browser/extension_function_registry.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/event_router.h"
#include "extensions/common/extension.h"
#include "extensions/common/permissions/permissions_data.h"
#include "shell/browser/api/electron_api_session.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/value_converter.h"

namespace extensions {
struct BookmarksRequestCheck {base::RepeatingCallback<bool()> callback;};
}
namespace gin {
template<> struct Converter<extensions::BookmarksRequestCheck>{
 static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,const extensions::BookmarksRequestCheck& value){
  return gin_helper::CallbackToV8Leaked(isolate,value.callback);
 }
};
}
namespace extensions {
namespace {
std::map<std::string,int>& Counts(){static base::NoDestructor<std::map<std::string,int>> counts;return *counts;}
class BookmarksFunction:public ExtensionFunction{
 public:
 bool Valid(){
  auto* context=browser_context();
  if(!pending_||!context||!extension())return false;
  const auto* current=ExtensionRegistry::Get(context)->enabled_extensions().GetByID(extension_id());
  return current==extension()&&current->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kBookmark);
 }
 void Finish(base::DictValue response){
  if(!pending_)return;
  std::string error;
  if(!Valid())error="Bookmarks permission or extension changed";
  else if(auto* value=response.FindString("error"))error=value->substr(0,1000);
  auto* result=response.FindList("result");
  if(error.empty()&&!result)error="Invalid bookmarks response";
  if(error.empty()){auto json=base::WriteJson(*result);if(!json||json->size()>16*1024*1024)error="Bookmarks response is too large";}
  pending_=false;timer_.Stop();
  if(--Counts()[extension_id()]==0)Counts().erase(extension_id());
  if(did_respond())return;
  if(error.empty())Respond(ArgumentList(result->Clone()));else Respond(Error(error));
 }
 protected:
 ResponseAction Run() override {
  if(!browser_context()||!extension()||source_context_type()!=mojom::ContextType::kPrivilegedExtension||
     !extension()->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kBookmark))
   return RespondNow(Error("Bookmarks permission is required"));
  auto serialized=base::WriteJson(args());
  if(!serialized||serialized->size()>32768)return RespondNow(Error("Bookmarks request is too large"));
  if(Counts().size()>=64||Counts()[extension_id()]>=16)return RespondNow(Error("Too many pending bookmarks requests"));
  pending_=true;++Counts()[extension_id()];
  auto self=scoped_refptr<BookmarksFunction>(this);
  auto reply=base::BindRepeating([](scoped_refptr<BookmarksFunction> self,base::DictValue response){self->Finish(std::move(response));},self);
  auto valid=base::BindRepeating([](scoped_refptr<BookmarksFunction> self){return self->Valid();},self);
  timer_.Start(FROM_HERE,base::Seconds(30),base::BindOnce([](scoped_refptr<BookmarksFunction> self){
   base::DictValue error;error.Set("error","Bookmarks request expired");self->Finish(std::move(error));
  },self));
  auto* session=electron::api::Session::FromBrowserContext(browser_context());
  if(!session||!session->Get()||!session->Get()->Emit("browser-bookmarks-request",extension_id(),std::string(name()),args().Clone(),reply,BookmarksRequestCheck{valid})){
   base::DictValue error;error.Set("error","Browser bookmarks integration is unavailable");Finish(std::move(error));
  }
  return did_respond()?AlreadyResponded():RespondLater();
 }
 ~BookmarksFunction() override=default;
 private:
 bool pending_=false;
 base::OneShotTimer timer_;
};
#define BOOKMARKS_FUNCTION(Class,Name,Histogram) class Class:public BookmarksFunction{public:DECLARE_EXTENSION_FUNCTION(Name,Histogram) protected:~Class()override=default;};
BOOKMARKS_FUNCTION(BookmarksGet,"bookmarks.get",BOOKMARKS_GET)
BOOKMARKS_FUNCTION(BookmarksChildren,"bookmarks.getChildren",BOOKMARKS_GETCHILDREN)
BOOKMARKS_FUNCTION(BookmarksRecent,"bookmarks.getRecent",BOOKMARKS_GETRECENT)
BOOKMARKS_FUNCTION(BookmarksTree,"bookmarks.getTree",BOOKMARKS_GETTREE)
BOOKMARKS_FUNCTION(BookmarksSubTree,"bookmarks.getSubTree",BOOKMARKS_GETSUBTREE)
BOOKMARKS_FUNCTION(BookmarksSearch,"bookmarks.search",BOOKMARKS_SEARCH)
BOOKMARKS_FUNCTION(BookmarksCreate,"bookmarks.create",BOOKMARKS_CREATE)
BOOKMARKS_FUNCTION(BookmarksMove,"bookmarks.move",BOOKMARKS_MOVE)
BOOKMARKS_FUNCTION(BookmarksUpdate,"bookmarks.update",BOOKMARKS_UPDATE)
BOOKMARKS_FUNCTION(BookmarksRemove,"bookmarks.remove",BOOKMARKS_REMOVE)
BOOKMARKS_FUNCTION(BookmarksRemoveTree,"bookmarks.removeTree",BOOKMARKS_REMOVETREE)
#undef BOOKMARKS_FUNCTION
}
void RegisterElectronBookmarksFunctions(ExtensionFunctionRegistry* registry){
 registry->RegisterFunction<BookmarksGet>();
 registry->RegisterFunction<BookmarksChildren>();
 registry->RegisterFunction<BookmarksRecent>();
 registry->RegisterFunction<BookmarksTree>();
 registry->RegisterFunction<BookmarksSubTree>();
 registry->RegisterFunction<BookmarksSearch>();
 registry->RegisterFunction<BookmarksCreate>();
 registry->RegisterFunction<BookmarksMove>();
 registry->RegisterFunction<BookmarksUpdate>();
 registry->RegisterFunction<BookmarksRemove>();
 registry->RegisterFunction<BookmarksRemoveTree>();
}
void DispatchElectronBookmarksEvent(content::BrowserContext* context,const std::string& name,base::ListValue args){
 const std::map<std::string,events::HistogramValue> events_map={
  {"bookmarks.onCreated",events::BOOKMARKS_ON_CREATED},{"bookmarks.onRemoved",events::BOOKMARKS_ON_REMOVED},
  {"bookmarks.onChanged",events::BOOKMARKS_ON_CHANGED},{"bookmarks.onMoved",events::BOOKMARKS_ON_MOVED}};
 auto found=events_map.find(name);if(found==events_map.end())return;
 auto* registry=ExtensionRegistry::Get(context);auto* router=EventRouter::Get(context);
 if(!registry||!router)return;
 auto histogram=found->second;
 for(const auto& extension:registry->enabled_extensions()){
  if(!extension->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kBookmark))continue;
  router->DispatchEventToExtension(extension->id(),std::make_unique<Event>(histogram,name,args.Clone(),context));
 }
}
}
