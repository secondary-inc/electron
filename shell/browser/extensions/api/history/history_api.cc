// Copyright 2026 Electron contributors.
#include "shell/browser/extensions/api/history/history_api.h"
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
struct HistoryRequestCheck {base::RepeatingCallback<bool()> callback;};
}
namespace gin {
template<> struct Converter<extensions::HistoryRequestCheck>{
 static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,const extensions::HistoryRequestCheck& value){
  return gin_helper::CallbackToV8Leaked(isolate,value.callback);
 }
};
}
namespace extensions {
namespace {
std::map<std::string,int>& Counts(){static base::NoDestructor<std::map<std::string,int>> counts;return *counts;}
class HistoryFunction:public ExtensionFunction{
 public:
 bool Valid(){
  auto* context=browser_context();
  if(!pending_||!context||!extension())return false;
  const auto* current=ExtensionRegistry::Get(context)->enabled_extensions().GetByID(extension_id());
  return current==extension()&&current->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kHistory);
 }
 void Finish(base::DictValue response){
  if(!pending_)return;
  std::string error;
  if(!Valid())error="History permission or extension changed";
  else if(auto* value=response.FindString("error"))error=value->substr(0,1000);
  auto* result=response.FindList("result");
  if(error.empty()&&!result)error="Invalid history response";
  if(error.empty()){auto json=base::WriteJson(*result);if(!json||json->size()>16*1024*1024)error="History response is too large";}
  pending_=false;timer_.Stop();
  if(--Counts()[extension_id()]==0)Counts().erase(extension_id());
  if(did_respond())return;
  if(error.empty())Respond(ArgumentList(result->Clone()));else Respond(Error(error));
 }
 protected:
 ResponseAction Run() override {
  if(!browser_context()||!extension()||source_context_type()!=mojom::ContextType::kPrivilegedExtension||
     !extension()->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kHistory))
   return RespondNow(Error("History permission is required"));
  auto serialized=base::WriteJson(args());
  if(!serialized||serialized->size()>32768)return RespondNow(Error("History request is too large"));
  if(Counts().size()>=64||Counts()[extension_id()]>=16)return RespondNow(Error("Too many pending history requests"));
  pending_=true;++Counts()[extension_id()];
  auto self=scoped_refptr<HistoryFunction>(this);
  auto reply=base::BindRepeating([](scoped_refptr<HistoryFunction> self,base::DictValue response){self->Finish(std::move(response));},self);
  auto valid=base::BindRepeating([](scoped_refptr<HistoryFunction> self){return self->Valid();},self);
  timer_.Start(FROM_HERE,base::Seconds(30),base::BindOnce([](scoped_refptr<HistoryFunction> self){
   base::DictValue error;error.Set("error","History request expired");self->Finish(std::move(error));
  },self));
  auto* session=electron::api::Session::FromBrowserContext(browser_context());
  if(!session||!session->Get()||!session->Get()->Emit("browser-history-request",extension_id(),std::string(name()),args().Clone(),reply,HistoryRequestCheck{valid})){
   base::DictValue error;error.Set("error","Browser history integration is unavailable");Finish(std::move(error));
  }
  return did_respond()?AlreadyResponded():RespondLater();
 }
 ~HistoryFunction() override=default;
 private:
 bool pending_=false;
 base::OneShotTimer timer_;
};
#define HISTORY_FUNCTION(Class,Name,Histogram) class Class:public HistoryFunction{public:DECLARE_EXTENSION_FUNCTION(Name,Histogram) protected:~Class()override=default;};
HISTORY_FUNCTION(HistorySearch,"history.search",HISTORY_SEARCH)
HISTORY_FUNCTION(HistoryVisits,"history.getVisits",HISTORY_GETVISITS)
HISTORY_FUNCTION(HistoryAdd,"history.addUrl",HISTORY_ADDURL)
HISTORY_FUNCTION(HistoryDelete,"history.deleteUrl",HISTORY_DELETEURL)
HISTORY_FUNCTION(HistoryRange,"history.deleteRange",HISTORY_DELETERANGE)
HISTORY_FUNCTION(HistoryClear,"history.deleteAll",HISTORY_DELETEALL)
#undef HISTORY_FUNCTION
}
void RegisterElectronHistoryFunctions(ExtensionFunctionRegistry* registry){
 registry->RegisterFunction<HistorySearch>();registry->RegisterFunction<HistoryVisits>();
 registry->RegisterFunction<HistoryAdd>();registry->RegisterFunction<HistoryDelete>();
 registry->RegisterFunction<HistoryRange>();registry->RegisterFunction<HistoryClear>();
}
void DispatchElectronHistoryEvent(content::BrowserContext* context,const std::string& name,base::ListValue args){
 if(name!="history.onVisited"&&name!="history.onVisitRemoved")return;
 auto* registry=ExtensionRegistry::Get(context);auto* router=EventRouter::Get(context);
 if(!registry||!router)return;
 auto histogram=name=="history.onVisited"?events::HISTORY_ON_VISITED:events::HISTORY_ON_VISIT_REMOVED;
 for(const auto& extension:registry->enabled_extensions()){
  if(!extension->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kHistory))continue;
  router->DispatchEventToExtension(extension->id(),std::make_unique<Event>(histogram,name,args.Clone(),context));
 }
}
}
