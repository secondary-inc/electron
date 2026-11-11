// Copyright 2026 Electron contributors.
#include "shell/browser/extensions/api/sessions/sessions_api.h"
#include <map>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/timer/timer.h"
#include "url/gurl.h"
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
struct SessionsRequestCheck {base::RepeatingCallback<bool()> callback;};
}
namespace gin {
template<> struct Converter<extensions::SessionsRequestCheck>{
 static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,const extensions::SessionsRequestCheck& value){
  return gin_helper::CallbackToV8Leaked(isolate,value.callback);
 }
};
}
namespace extensions {
namespace {
std::map<std::string,int>& Counts(){static base::NoDestructor<std::map<std::string,int>> counts;return *counts;}
class SessionsFunction:public ExtensionFunction{
 public:
 bool Valid(){
  auto* context=browser_context();
  if(!pending_||!context||!extension())return false;
  const auto* current=ExtensionRegistry::Get(context)->enabled_extensions().GetByID(extension_id());
  return current==extension()&&current->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kSessions);
 }
 void Finish(base::DictValue response){
  if(!pending_)return;
  std::string error;
  if(!Valid())error="Sessions permission or extension changed";
  else if(auto* value=response.FindString("error"))error=value->substr(0,1000);
  auto* result=response.FindList("result");
  if(error.empty()&&!result)error="Invalid sessions response";
  if(error.empty()){auto json=base::WriteJson(*result);if(!json||json->size()>16*1024*1024)error="Sessions response is too large";}
  pending_=false;timer_.Stop();
  if(--Counts()[extension_id()]==0)Counts().erase(extension_id());
  if(did_respond())return;
  if(error.empty()){
   auto filtered=result->Clone();
   for(auto& value:filtered){
    if(value.is_list()){for(auto& row:value.GetList())Scrub(row);}
    else Scrub(value);
   }
   Respond(ArgumentList(std::move(filtered)));
  }else Respond(Error(error));
 }
 protected:
 void Scrub(base::Value& value){
  if(!value.is_dict())return;
  auto* tab=value.GetDict().FindDict("tab");if(!tab)return;
  const auto* url=tab->FindString("url");
  if(!extension()->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kTab)&&
     (!url||!extension()->permissions_data()->HasHostPermission(GURL(*url)))){
   tab->Remove("url");tab->Remove("title");tab->Remove("favIconUrl");tab->Remove("pendingUrl");
  }
 }
 ResponseAction Run() override {
  if(!browser_context()||!extension()||source_context_type()!=mojom::ContextType::kPrivilegedExtension||
     !extension()->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kSessions))
   return RespondNow(Error("Sessions permission is required"));
  auto serialized=base::WriteJson(args());
  if(!serialized||serialized->size()>32768)return RespondNow(Error("Sessions request is too large"));
  if(Counts().size()>=64||Counts()[extension_id()]>=16)return RespondNow(Error("Too many pending sessions requests"));
  pending_=true;++Counts()[extension_id()];
  auto self=scoped_refptr<SessionsFunction>(this);
  auto reply=base::BindRepeating([](scoped_refptr<SessionsFunction> self,base::DictValue response){self->Finish(std::move(response));},self);
  auto valid=base::BindRepeating([](scoped_refptr<SessionsFunction> self){return self->Valid();},self);
  timer_.Start(FROM_HERE,base::Seconds(30),base::BindOnce([](scoped_refptr<SessionsFunction> self){
   base::DictValue error;error.Set("error","Sessions request expired");self->Finish(std::move(error));
  },self));
  auto* session=electron::api::Session::FromBrowserContext(browser_context());
  if(!session||!session->Get()||!session->Get()->Emit("browser-sessions-request",extension_id(),std::string(name()),args().Clone(),reply,SessionsRequestCheck{valid})){
   base::DictValue error;error.Set("error","Browser sessions integration is unavailable");Finish(std::move(error));
  }
  return did_respond()?AlreadyResponded():RespondLater();
 }
 ~SessionsFunction() override=default;
 private:
 bool pending_=false;
 base::OneShotTimer timer_;
};
#define HISTORY_FUNCTION(Class,Name,Histogram) class Class:public SessionsFunction{public:DECLARE_EXTENSION_FUNCTION(Name,Histogram) protected:~Class()override=default;};
HISTORY_FUNCTION(SessionsRecent,"sessions.getRecentlyClosed",SESSIONS_GETRECENTLYCLOSED)
HISTORY_FUNCTION(SessionsDevices,"sessions.getDevices",SESSIONS_GETDEVICES)
HISTORY_FUNCTION(SessionsRestore,"sessions.restore",SESSIONS_RESTORE)
#undef HISTORY_FUNCTION
}
void RegisterElectronSessionsFunctions(ExtensionFunctionRegistry* registry){
 registry->RegisterFunction<SessionsRecent>();registry->RegisterFunction<SessionsDevices>();registry->RegisterFunction<SessionsRestore>();
}
void DispatchElectronSessionsEvent(content::BrowserContext* context,const std::string& name,base::ListValue args){
 if(name!="sessions.onChanged")return;
 auto* registry=ExtensionRegistry::Get(context);auto* router=EventRouter::Get(context);
 if(!registry||!router)return;
 auto histogram=events::SESSIONS_ON_CHANGED;
 for(const auto& extension:registry->enabled_extensions()){
  if(!extension->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kSessions))continue;
  router->DispatchEventToExtension(extension->id(),std::make_unique<Event>(histogram,name,args.Clone(),context));
 }
}
}
