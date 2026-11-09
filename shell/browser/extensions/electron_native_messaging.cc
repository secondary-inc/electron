// Copyright (c) 2026 Electron contributors.
// Use of this source code is governed by the MIT license.
#include "shell/browser/extensions/electron_native_messaging.h"
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/supports_user_data.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/permissions_manager.h"
#include "extensions/common/api/messaging/message.h"
#include "extensions/common/extension.h"
#include "extensions/common/permissions/permissions_data.h"
#include "shell/browser/api/electron_api_session.h"

namespace extensions {
namespace {
constexpr char kRegistryKey[] = "iris-native-message-registry";
constexpr size_t kRequestLimit = 64 * 1024 * 1024;
constexpr size_t kReplyLimit = 1024 * 1024;
class NativePort;
bool ValidName(const std::string& name) {
 if(name.empty() || name.size()>255 || name.front()=='.' || name.back()=='.') return false;
 char previous=0;
 for(char c:name){if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.')||(c=='.'&&previous=='.'))return false;previous=c;}
 return true;
}
bool ValidExtension(const std::string& id) {
 if(id.size()!=32)return false;
 for(char c:id)if(c<'a'||c>'p')return false;
 return true;
}
class Registry : public base::SupportsUserData::Data,
                 public ExtensionRegistryObserver,
                 public PermissionsManager::Observer {
 public:
 explicit Registry(content::BrowserContext* context):context(context) {
  extension_observation.Observe(ExtensionRegistry::Get(context));
  permission_observation.Observe(PermissionsManager::Get(context));
 }
 ~Registry() override;
 bool Allowed(const std::string& extension_id,const std::string& host) const {
  if(shutdown||!rules.contains({extension_id,host}))return false;
  const Extension* extension=ExtensionRegistry::Get(context)->enabled_extensions().GetByID(extension_id);
  return extension&&extension->permissions_data()->HasAPIPermission(mojom::APIPermissionID::kNativeMessaging);
 }
 void Revalidate();
 void Shutdown();
 void OnExtensionUnloaded(content::BrowserContext*,const Extension*,UnloadedExtensionReason) override { Revalidate(); }
 void OnExtensionPermissionsUpdated(const Extension&,const PermissionSet&,PermissionsManager::UpdateReason) override { Revalidate(); }
 void OnShutdown(ExtensionRegistry*) override {extension_observation.Reset();Shutdown();}
 void OnPermissionsManagerShutdown() override {permission_observation.Reset();Shutdown();}
 raw_ptr<content::BrowserContext> context;
 bool shutdown=false;
 std::set<std::pair<std::string,std::string>> rules;
 std::map<std::string,base::WeakPtr<NativePort>> ports;
 base::ScopedObservation<ExtensionRegistry,ExtensionRegistryObserver> extension_observation{this};
 base::ScopedObservation<PermissionsManager,PermissionsManager::Observer> permission_observation{this};
 base::WeakPtrFactory<Registry> weak_factory{this};
};
Registry* Get(content::BrowserContext* context,bool create=false){
 auto* registry=static_cast<Registry*>(context->GetUserData(kRegistryKey));
 if(!registry&&create){auto owned=std::make_unique<Registry>(context);registry=owned.get();context->SetUserData(kRegistryKey,std::move(owned));}
 return registry;
}
class NativePort : public MessagePort {
 public:
 NativePort(Registry* registry,base::WeakPtr<ChannelDelegate> delegate,const PortId& port,
            std::string extension_id,std::string host)
  :MessagePort(std::move(delegate),port),registry_(registry->weak_factory.GetWeakPtr()),
   extension_id_(std::move(extension_id)),host_(std::move(host)),id_(base::Uuid::GenerateRandomV4().AsLowercaseString()){
  registry->ports.emplace(id_,weak_factory_.GetWeakPtr());
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,base::BindOnce(&NativePort::Start,weak_factory_.GetWeakPtr()));
 }
 ~NativePort() override {
  if(registry_){registry_->ports.erase(id_);Emit("native-message-closed",id_);}
 }
 bool IsValidPort() override {return registry_&&registry_->Allowed(extension_id_,host_);}
 void DispatchOnMessage(Message message) override {
  if(!IsValidPort()){Close("Native messaging permission was revoked");return;}
  std::string text=message.data();
  if(text.empty()||text.size()>kRequestLimit){Close("Native message request exceeds its byte limit");return;}
  if(!ready_){
   if(pending_.size()>=32||pending_bytes_>kRequestLimit-text.size()){Close("Native messaging queue is full");return;}
   pending_bytes_+=text.size();pending_.push_back(std::move(text));return;
  }
  Emit("native-message-data",id_,text);
 }
 bool Ready(){
  if(!IsValidPort()||ready_)return false;
  ready_=true;deadline_.Stop();
  auto messages=std::move(pending_);pending_bytes_=0;
  auto weak=weak_factory_.GetWeakPtr();
  for(const auto& message:messages){if(!weak||!weak->IsValidPort())return false;weak->Emit("native-message-data",weak->id_,message);}
  return true;
 }
 bool Reply(const std::string& text){
  if(!ready_||!IsValidPort()||text.empty()||text.size()>kReplyLimit||!base::JSONReader::Read(text,base::JSON_PARSE_RFC))return false;
  if(!weak_channel_delegate_)return false;
  weak_channel_delegate_->PostMessage(port_id_,Message(text,false));return true;
 }
 void Close(const std::string& error){
  deadline_.Stop();
  if(weak_channel_delegate_)weak_channel_delegate_->CloseChannel(port_id_,error.substr(0,256));
 }
 private:
 template<typename... Args> void Emit(const char* event,const Args&... args){
  if(!registry_)return;
  auto* session=electron::api::Session::FromBrowserContext(registry_->context);
  if(session&&session->Get())session->Get()->Emit(event,args...);
 }
 void Start(){
  if(!IsValidPort()){Close("Native messaging permission was revoked");return;}
  deadline_.Start(FROM_HERE,base::Seconds(10),base::BindOnce(&NativePort::Close,weak_factory_.GetWeakPtr(),"Native messaging host did not start"));
  Emit("native-message-open",id_,extension_id_,host_);
 }
 base::WeakPtr<Registry> registry_;
 std::string extension_id_,host_,id_;
 bool ready_=false;
 size_t pending_bytes_=0;
 std::deque<std::string> pending_;
 base::OneShotTimer deadline_;
 base::WeakPtrFactory<NativePort> weak_factory_{this};
};
Registry::~Registry(){Shutdown();}
void Registry::Revalidate(){
 std::vector<base::WeakPtr<NativePort>> current;
 for(const auto& [id,port]:ports)current.push_back(port);
 for(auto& port:current)if(port&&!port->IsValidPort())port->Close("Native messaging permission was revoked");
}
void Registry::Shutdown(){if(shutdown)return;shutdown=true;rules.clear();Revalidate();}
base::WeakPtr<NativePort> Find(content::BrowserContext* context,const std::string& id){
 auto* registry=Get(context);if(!registry)return {};
 auto entry=registry->ports.find(id);return entry==registry->ports.end()?base::WeakPtr<NativePort>():entry->second;
}
}  // namespace
bool ConfigureElectronNativeMessaging(content::BrowserContext* context,const base::ListValue& entries){
 DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
 if(entries.size()>128)return false;
 std::set<std::pair<std::string,std::string>> rules;
 for(const auto& value:entries){
  const auto* dict=value.GetIfDict();if(!dict)return false;
  const auto* id=dict->FindString("extensionId");const auto* host=dict->FindString("hostName");
  if(!id||!host||!ValidExtension(*id)||!ValidName(*host)||!rules.emplace(*id,*host).second)return false;
 }
 auto* registry=Get(context,true);if(registry->shutdown)return false;
 registry->rules=std::move(rules);registry->Revalidate();return true;
}
bool ElectronNativeHostAllowed(content::BrowserContext* context,const std::string& host){
 auto* registry=Get(context);if(!registry||registry->shutdown||!ValidName(host))return false;
 for(const auto& [id,name]:registry->rules)if(name==host&&registry->Allowed(id,name))return true;
 return false;
}
std::unique_ptr<MessagePort> CreateElectronNativeMessagePort(
 content::BrowserContext* context,base::WeakPtr<MessagePort::ChannelDelegate> delegate,
 const PortId& port,const std::string& extension_id,const std::string& host){
 auto* registry=Get(context);
 if(!registry||!registry->Allowed(extension_id,host)||registry->ports.size()>=16)return nullptr;
 return std::make_unique<NativePort>(registry,std::move(delegate),port,extension_id,host);
}
bool ElectronNativeMessageReady(content::BrowserContext* context,const std::string& id){auto port=Find(context,id);return port&&port->Ready();}
bool ElectronNativeMessageReply(content::BrowserContext* context,const std::string& id,const std::string& text){auto port=Find(context,id);return port&&port->Reply(text);}
bool ElectronNativeMessageClose(content::BrowserContext* context,const std::string& id,const std::string& error){auto port=Find(context,id);if(!port)return false;port->Close(error);return true;}
}  // namespace extensions
