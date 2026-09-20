#pragma once
#include "../app_state.h"
#include "../components/lr_theme.h"
#include "../components/widgets.h"

namespace lr_gui {
namespace client_detail {
using literouter::i18n::tr;

inline std::string text(const char* value) { return std::string(tr(value)); }

inline void input(eui::Ui& ui, const std::string& id, float x, float y, float width,
                  const char* label, std::string& value, const char* placeholder = "") {
    ui.text(id + ".label").position(x,y).size(width,18.0f).text(tr(label))
        .fontSize(12.0f).lineHeight(17.0f).color(palette().textMuted).build();
    ui.stack(id + ".wrap").position(x,y+21.0f).size(width,36.0f).content([&] {
        components::input(ui,id).theme(uiTokens()).size(width,36.0f)
            .value(value).placeholder(tr(placeholder)).onChange([&value](const std::string& next) {
                if (appState().clientEditor.saving) return;
                value=next;appState().clientEditor.error.clear();
            }).build();
    }).build();
}

inline void toggle(eui::Ui& ui, const std::string& id, float x, float y, float width,
                   const char* label, bool value, std::function<void(bool)> action) {
    ui.stack(id+".wrap").position(x,y).size(width,30.0f).content([&] {
        components::toggleSwitch(ui,id).theme(uiTokens()).size(width,30.0f)
            .text(tr(label)).checked(value).fontSize(12.0f).onChange(std::move(action)).build();
    }).build();
}

inline std::string joined(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value:values) { if (!result.empty()) result += ", "; result += value; }
    return result.empty() ? text("Unrestricted") : result;
}

inline void open(const literouter::ClientConfig* client = nullptr) {
    auto& editor=appState().clientEditor;
    editor=ClientEditor{};
    editor.open=true;
    if (!client) return;
    editor.originalId=client->id;
    editor.draft=*client;
    editor.modelsText=joined(client->models);
    editor.groupsText=joined(client->provider_groups);
    if (client->models.empty()) editor.modelsText.clear();
    if (client->provider_groups.empty()) editor.groupsText.clear();
    editor.rpm=std::to_string(client->requests_per_minute);
    editor.concurrent=std::to_string(client->max_concurrent);
    editor.dailyRequests=std::to_string(client->requests_per_day);
    editor.dailyTokens=std::to_string(client->tokens_per_day);
    editor.reservation=std::to_string(client->token_reservation);
    editor.budget=std::format("{}", client->budget_usd_per_day);
}

// A nonnegative real number, for the fields measured in dollars. std::stod
// accepts "1e999" as infinity and "abc" as a throw, so both are rejected
// explicitly rather than written back as a budget the server would refuse.
inline bool parseMoney(const std::string& raw, double& out) {
    const std::string value=literouter::trim(raw);
    if (value.empty()) { out=0.0; return true; }
    try {
        std::size_t consumed=0;
        const double parsed=std::stod(value,&consumed);
        if (consumed!=value.size() || !std::isfinite(parsed) || parsed<0.0) return false;
        out=parsed;
        return true;
    } catch (...) {
        return false;
    }
}

inline bool parseNumber(const std::string& raw, std::uint64_t& out) {
    const std::string value=literouter::trim(raw);
    if (value.empty() || value.find_first_not_of("0123456789")!=std::string::npos) return false;
    const auto result=std::from_chars(value.data(),value.data()+value.size(),out);
    return result.ec==std::errc{} && result.ptr==value.data()+value.size();
}

inline void save() {
    auto& state=appState();
    auto& editor=state.clientEditor;
    if (!editor.keyId.empty() || !editor.keyValue.empty()) {
        editor.error=text("Apply or cancel the key edit before saving the client.");return;
    }
    auto client=editor.draft;
    client.id=literouter::trim(client.id);
    client.name=literouter::trim(client.name);
    std::uint64_t rpm=0,concurrent=0;
    if (!parseNumber(editor.rpm,rpm) || rpm>static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        !parseNumber(editor.concurrent,concurrent) || concurrent>static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        !parseNumber(editor.dailyRequests,client.requests_per_day) ||
        !parseNumber(editor.dailyTokens,client.tokens_per_day) || !parseNumber(editor.reservation,client.token_reservation) ||
        !parseMoney(editor.budget,client.budget_usd_per_day)) {
        editor.error=text("Limits must be nonnegative whole numbers within range, and the budget a nonnegative amount.");return;
    }
    client.requests_per_minute=static_cast<int>(rpm);
    client.max_concurrent=static_cast<int>(concurrent);
    client.models=AppState::splitList(editor.modelsText);
    client.provider_groups=AppState::splitList(editor.groupsText);
    auto next=state.store.config();
    bool replaced=false;
    for (auto& item:next.clients) {
        if (item.id==editor.originalId && !editor.originalId.empty()) {item=client;replaced=true;break;}
    }
    if (!replaced) next.clients.push_back(std::move(client));
    state.saveClients(std::move(next));
}

inline void applyKey() {
    auto& editor=appState().clientEditor;
    const std::string id=literouter::trim(editor.keyId);
    if (id.empty()) {editor.error=text("Key ID is required");return;}
    for (std::size_t i=0;i<editor.draft.keys.size();++i) {
        if (editor.draft.keys[i].id==id && static_cast<int>(i)!=editor.keyIndex) {
            editor.error=text("Key ID already exists");return;
        }
    }
    if (editor.keyIndex<0) {
        if (editor.keyValue.empty()) {editor.error=text("A new key requires a value or an environment reference.");return;}
        editor.draft.keys.push_back({id,editor.keyValue,true});
    } else if (editor.keyIndex<static_cast<int>(editor.draft.keys.size())) {
        auto& key=editor.draft.keys[static_cast<std::size_t>(editor.keyIndex)];
        key.id=id;
        if (!editor.keyValue.empty()) key.api_key=editor.keyValue;
    }
    editor.keyId.clear();editor.keyValue.clear();editor.keyIndex=-1;editor.error.clear();
}

inline void editor(eui::Ui& ui,float x,float y,float width,float height) {
    auto& state=appState();
    auto& draft=state.clientEditor;
    const auto& p=palette();
    const float padding=layout::pagePadding;
    ui.text("clients.editor.title").position(x+padding,y+18.0f).size(std::max(100.0f,width-300.0f),24.0f)
        .text(tr(draft.originalId.empty()?"Add client":"Edit client")).fontSize(17.0f).fontWeight(700).color(p.text).build();
    actionButton(ui,"clients.editor.cancel",x+width-padding-212.0f,y+12.0f,100.0f,36.0f,tr("Cancel"),false,
        []{appState().clientEditor=ClientEditor{};});
    actionButton(ui,"clients.editor.save",x+width-padding-100.0f,y+12.0f,100.0f,36.0f,tr("Save client"),true,[]{save();});
    const float scrollY=y+64.0f;
    const float statusHeight=draft.error.empty()?0.0f:54.0f;
    if (!draft.error.empty()) {
        ui.text("clients.editor.error").position(x+padding,y+height-statusHeight).size(width-padding*2.0f,statusHeight)
            .text(draft.error).fontSize(12.0f).lineHeight(18.0f).wrap(true).maxWidth(width-padding*2.0f).color(p.danger).build();
    }
    components::scrollView(ui,"clients.editor.scroll").theme(uiTokens()).position(x,scrollY)
        .size(width,std::max(1.0f,height-64.0f-statusHeight)).offset(draft.scroll)
        .step(layout::scrollStep).onChange([&draft](float offset){draft.scroll=offset;})
        .content([&](eui::Ui& content,float cw,float) {
            const float inner=cw-padding*2.0f;
            const bool columns=inner>=680.0f;
            const float fieldWidth=columns?(inner-24.0f)*0.5f:inner;
            // Ten fields: five rows of two when there is room, ten rows when
            // there is not.
            const float fieldsHeight=(columns?5.0f:10.0f)*75.0f;
            const float keysY=fieldsHeight+112.0f;
            const float keysHeight=static_cast<float>(draft.draft.keys.size())*54.0f;
            content.stack("clients.editor.canvas").size(cw,keysY+keysHeight+240.0f).content([&] {
                std::string* values[]={&draft.draft.id,&draft.draft.name,&draft.modelsText,&draft.groupsText,
                    &draft.rpm,&draft.concurrent,&draft.dailyRequests,&draft.dailyTokens,&draft.reservation,
                    &draft.budget};
                const char* labels[]={"Client ID","Display name","Allowed models","Provider groups",
                    "Requests per minute","Concurrency","Requests / day","Tokens / day","Token reservation",
                    "Daily budget (USD)"};
                for (int i=0;i<10;++i) {
                    const float fx=padding+(columns?static_cast<float>(i%2)*(fieldWidth+24.0f):0.0f);
                    const float fy=static_cast<float>(columns?i/2:i)*75.0f;
                    if (i==0 && !draft.originalId.empty()) {
                        fieldValue(content,"clients.editor.id",fx,fy,fieldWidth,tr("Client ID"),draft.originalId,p.text);
                    } else {
                        input(content,"clients.editor.field."+std::to_string(i),fx,fy,fieldWidth,labels[i],*values[i],
                            (i==2||i==3)?"Empty means unrestricted; comma separated"
                                        :(i==9?"0 disables the ceiling":""));
                    }
                }
                toggle(content,"clients.editor.enabled",padding,fieldsHeight,inner,"Enabled",draft.draft.enabled,
                    [&draft](bool value){draft.draft.enabled=value;});
                content.text("clients.editor.explain").position(padding,fieldsHeight+38.0f).size(inner,64.0f)
                    .text(tr("0 disables a limit. Daily quotas and budgets reset at 00:00 UTC; a budget is checked when a request arrives, so one already in flight when the ceiling is reached still completes. Token reservations remain charged when upstream usage is missing."))
                    .fontSize(12.0f).lineHeight(18.0f).wrap(true).maxWidth(inner).color(p.textMuted).build();
                content.text("clients.editor.keys.title").position(padding,keysY).size(inner,24.0f)
                    .text(tr("Client keys")).fontSize(15.0f).fontWeight(700).color(p.text).build();
                for (std::size_t i=0;i<draft.draft.keys.size();++i) {
                    const auto& key=draft.draft.keys[i];
                    const float ky=keysY+34.0f+static_cast<float>(i)*54.0f;
                    const std::string id="clients.editor.key."+std::to_string(i);
                    content.text(id+".id").position(padding,ky).size(std::max(60.0f,inner-326.0f),20.0f)
                        .text(key.id).fontSize(13.0f).color(p.text).build();
                    content.text(id+".set").position(padding,ky+20.0f).size(std::max(60.0f,inner-326.0f),18.0f)
                        .text(tr(key.api_key.empty()?"Not set":"Configured")).fontSize(11.0f).color(p.textMuted).build();
                    toggle(content,id+".enabled",cw-padding-312.0f,ky,136.0f,"Enabled",key.enabled,
                        [i](bool value){appState().clientEditor.draft.keys[i].enabled=value;});
                    actionButton(content,id+".edit",cw-padding-168.0f,ky,78.0f,30.0f,tr("Edit"),false,[i] {
                        auto& e=appState().clientEditor;e.keyIndex=static_cast<int>(i);
                        e.keyId=e.draft.keys[i].id;e.keyValue.clear();e.error.clear();
                    });
                    actionButton(content,id+".remove",cw-padding-78.0f,ky,78.0f,30.0f,tr("Delete"),false,[i] {
                        auto& e=appState().clientEditor;e.draft.keys.erase(e.draft.keys.begin()+static_cast<long>(i));
                        e.keyIndex=-1;e.keyId.clear();e.keyValue.clear();
                    });
                }
                const float addY=keysY+keysHeight+38.0f;
                const float keyWidth=(inner-24.0f)*0.5f;
                input(content,"clients.editor.keyid",padding,addY,keyWidth,"Key ID",draft.keyId);
                input(content,"clients.editor.keyvalue",padding+keyWidth+24.0f,addY,keyWidth,"API key",draft.keyValue,"Literal key or ${ENV_VAR}");
                content.text("clients.editor.keyhint").position(padding,addY+65.0f).size(inner,40.0f)
                    .text(tr("Existing values stay hidden. A blank replacement keeps the stored key. Key edits take effect when you save the client."))
                    .fontSize(11.5f).lineHeight(17.0f).wrap(true).maxWidth(inner).color(p.textMuted).build();
                actionButton(content,"clients.editor.applykey",padding,addY+114.0f,148.0f,36.0f,
                    tr(draft.keyIndex<0?"Add key to draft":"Apply key edit"),true,[]{applyKey();});
                actionButton(content,"clients.editor.cancelkey",padding+160.0f,addY+114.0f,130.0f,36.0f,tr("Cancel key edit"),false,[] {
                    auto& e=appState().clientEditor;e.keyIndex=-1;e.keyId.clear();e.keyValue.clear();e.error.clear();
                });
            }).build();
        }).build();
}
} // namespace client_detail

inline void composeClients(eui::Ui& ui,float x,float y,float width,float height) {
    using namespace client_detail;
    auto& state=appState();
    if (state.clientEditor.open) {editor(ui,x,y,width,height);return;}
    const auto& p=palette();
    const auto& clients=state.store.config().clients;
    const float padding=layout::pagePadding;
    ui.text("clients.note").position(x+padding,y+14.0f).size(std::max(140.0f,width-240.0f),48.0f)
        .text(tr("Admin key stays separate. Client keys can call models only."))
        .fontSize(12.5f).lineHeight(18.0f).wrap(true).maxWidth(std::max(140.0f,width-240.0f)).color(p.textMuted).build();
    actionButton(ui,"clients.add",x+width-padding-138.0f,y+12.0f,138.0f,36.0f,tr("Add client"),true,[]{open();});
    const float listY=y+68.0f;
    const float listHeight=std::max(1.0f,height-68.0f);
    components::scrollView(ui,"clients.scroll").theme(uiTokens()).position(x,listY).size(width,listHeight)
        .offset(state.clientsScroll).step(layout::scrollStep).onChange([&state](float v){state.clientsScroll=v;})
        .content([&](eui::Ui& content,float cw,float) {
            const float cardWidth=cw-padding*2.0f;
            const bool narrow=cardWidth<760.0f;
            const float cardHeight=narrow?336.0f:280.0f;
            content.stack("clients.canvas").size(cw,std::max(listHeight,static_cast<float>(clients.size())*(cardHeight+16.0f)+24.0f))
                .content([&] {
                    if (clients.empty()) {
                        emptyState(content,"clients.empty",padding,12.0f,cardWidth,220.0f,tr("No clients configured"),
                            tr("Personal access works without client accounts. Set an administrator key in Settings before adding your first client."),
                            tr("Add client"),[]{open();});return;
                    }
                    for (std::size_t i=0;i<clients.size();++i) {
                        const auto& client=clients[i];
                        literouter::ClientUsage usage;usage.client=client.id;
                        for (const auto& item:state.snapshot.clients) if (item.client==client.id) {usage=item;break;}
                        const std::string base="clients.row."+std::to_string(i);
                        content.stack(base).position(padding,static_cast<float>(i)*(cardHeight+16.0f))
                            .size(cardWidth,cardHeight).content([&] {
                                content.rect(base+".bg").size(cardWidth,cardHeight).radius(p.radiusCard)
                                    .color(p.surface).border(1.0f,p.border).build();
                                content.text(base+".name").position(18.0f,14.0f).size(cardWidth-270.0f,24.0f)
                                    .text(client.name.empty()?client.id:client.name).fontSize(16.0f).fontWeight(700).color(p.text).build();
                                content.text(base+".id").position(18.0f,41.0f).size(cardWidth-36.0f,20.0f)
                                    .text(client.id+" · "+std::to_string(client.keys.size())+" "+text("Keys")+" · "+text(client.enabled?"enabled":"disabled"))
                                    .fontSize(12.0f).color(p.textMuted).build();
                                actionButton(content,base+".edit",cardWidth-174.0f,14.0f,72.0f,32.0f,tr("Edit"),false,[i] {
                                    open(&appState().store.config().clients[i]);
                                });
                                actionButton(content,base+".delete",cardWidth-90.0f,14.0f,72.0f,32.0f,tr("Delete"),false,[i] {
                                    appState().requestConfirm(ConfirmKind::DeleteClient,static_cast<int>(i),text("Delete client?"),
                                        text("All keys belonging to this client will stop working. Usage remains in the quota ledger."),text("Delete"));
                                });
                                content.text(base+".permissions").position(18.0f,70.0f).size(cardWidth-36.0f,36.0f)
                                    .text(text("Allowed models")+": "+joined(client.models)+"\n"+text("Provider groups")+": "+joined(client.provider_groups))
                                    .fontSize(11.5f).lineHeight(17.0f).color(p.textMuted).build();
                                const std::string unlimited=text("Unlimited");
                                auto quota=[&](std::uint64_t used,std::uint64_t cap) {
                                    return literouter::humanCount(used)+" / "+(cap?literouter::humanCount(cap):unlimited);
                                };
                                const char* labels[]={"Requests today","Tokens today","IN FLIGHT","Estimated cost","TOTAL REQUESTS","SUCCESS / FAILURES"};
                                const std::string values[]={quota(usage.requests_today,client.requests_per_day),quota(usage.tokens_today,client.tokens_per_day),
                                    quota(usage.active_requests,static_cast<std::uint64_t>(client.max_concurrent)),std::format("${:.4f}",usage.cost_usd),
                                    literouter::humanCount(usage.requests),std::to_string(usage.successes)+" / "+std::to_string(usage.failures)};
                                const int columns=narrow?3:6;
                                const float fieldWidth=(cardWidth-36.0f)/static_cast<float>(columns);
                                for (int j=0;j<6;++j) {
                                    fieldValue(content,base+".metric."+std::to_string(j),18.0f+static_cast<float>(j%columns)*fieldWidth,
                                        119.0f+static_cast<float>(j/columns)*56.0f,fieldWidth-10.0f,tr(labels[j]),values[j],p.text);
                                }
                                const float noteY=narrow?236.0f:180.0f;
                                content.text(base+".limits").position(18.0f,noteY).size(cardWidth-36.0f,22.0f)
                                    .text(text("RPM")+": "+(client.requests_per_minute?std::to_string(client.requests_per_minute):unlimited)+" · "+
                                        text("Daily budget")+": "+(client.budget_usd_per_day>0.0
                                            ?std::format("${:.4f} / ${:.2f}",usage.cost_today,client.budget_usd_per_day)
                                            :unlimited)+" · "+
                                        text("Token reservation")+": "+std::to_string(client.token_reservation)+" · "+text("Reserved tokens")+": "+literouter::humanCount(usage.reserved_tokens))
                                    .fontSize(11.5f).color(p.textMuted).build();
                                content.text(base+".tokens").position(18.0f,noteY+25.0f).size(cardWidth-36.0f,22.0f)
                                    .text(text("Prompt / completion tokens")+": "+literouter::humanCount(usage.tokens_prompt)+" / "+literouter::humanCount(usage.tokens_completion))
                                    .fontSize(11.5f).color(p.textMuted).build();
                                content.text(base+".day").position(18.0f,noteY+51.0f).size(cardWidth-36.0f,36.0f)
                                    .text(tr("Daily quotas and budgets reset at 00:00 UTC. A budget is checked when a request arrives, so one already in flight when the ceiling is reached still completes. Token totals include reservations."))
                                    .fontSize(11.0f).lineHeight(16.0f).wrap(true).maxWidth(cardWidth-36.0f).color(p.textFaint).build();
                            }).build();
                    }
                }).build();
        }).build();
}
} // namespace lr_gui
