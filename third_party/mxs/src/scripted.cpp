// Scripted definitions: macroScript, plugin and tool.
//
// The engine owns the definitions and the per-object script state; realising
// them (menu actions, real scene classes, mouse tools) is the host's job, done
// through the Engine API in mxs.h (macroscripts()/run_macroscript(),
// on_plugin_defined + plugin_instance() + HandlerScope, start_tool_hook).
//
// Scoping follows the rest of this interpreter: function bodies bind to the
// global scope, so a handler's state (parameters, the definition's locals and
// functions, host extras such as 'mesh') is STAGED into the globals for the
// duration of the call and written back afterwards (copy-in/copy-out), the
// same mechanism rollout handlers and struct methods use.
#include "mxs_internal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>

namespace mxs {

[[noreturn]] static void serr(const std::string& m, int line = 0) { throw MxsError(m, line); }

std::string ScriptedInfo::str(const std::string& key) const {
    auto it = kw.find(lower(key));
    if (it == kw.end()) return std::string();
    return it->second.to_string();
}
bool ScriptedInfo::has(const std::string& event) const {
    const std::string e = lower(event);
    for (auto& x : events) if (x == e) return true;
    return false;
}

std::string Engine::lower_name(const std::string& s) { return lower(s); }

// ------------------------------------------------------------ staging ----
Value with_staged_slots(Engine& eng, std::map<std::string, Value>& slots, const std::function<Value()>& fn) {
    Env* g = eng.global_env_ptr();
    std::map<std::string, Value> saved;
    std::vector<std::string> added;
    for (auto& kv : slots) {
        auto ex = g->vars.find(kv.first);
        if (ex != g->vars.end()) saved[kv.first] = ex->second;
        else added.push_back(kv.first);
        g->vars[kv.first] = kv.second;
    }
    auto restore = [&]() {
        for (auto& kv : slots) {
            auto cur = g->vars.find(kv.first);
            if (cur != g->vars.end()) kv.second = cur->second;
        }
        for (auto& kv : saved) g->vars[kv.first] = kv.second;
        for (auto& nm : added) g->vars.erase(nm);
    };
    Value r;
    try { r = fn(); } catch (...) { restore(); throw; }
    restore();
    return r;
}

// Header / parameter keyword value. Identifiers that name nothing (ui:spn,
// extends:Standard, icon names) come back as Names so the host can read them.
static Value kw_value(Node* k, const std::string& kw, Env& env, Engine& eng) {
    if (!k) return Value::undef();
    if (k->k == NK::Ident && (kw == "extends" || kw == "ui" || kw == "rollout" || kw == "type"))
        return Value::N(k->name);
    if (k->k == NK::ArrayLit && kw == "ui") {
        Array_t out;
        for (auto& e : k->kids) out.push_back(e->k == NK::Ident ? Value::N(e->name) : eval_node(e.get(), env, eng));
        return Value::A(std::move(out));
    }
    Value v = eval_node(k, env, eng);
    if (v.t == Type::Undefined && k->k == NK::Ident) return Value::N(k->name);
    return v;
}

static std::string kw_name_str(const Value& v) {
    if (v.t == Type::Name || v.t == Type::Class) return lower(*v.name);
    if (v.t == Type::String) return lower(*v.s);
    return std::string();
}

static std::shared_ptr<ScriptedDef> build_def(Node* n, Env& env, Engine& eng) {
    auto def = std::make_shared<ScriptedDef>();
    def->deniedCaps = eng.denied_caps;   // [F08] code defined under a restriction stays restricted
    ScriptedInfo& info = def->info;
    info.kind = n->str;
    info.name = n->name;
    info.deniedCaps = def->deniedCaps;
    if (n->str == "plugin" && n->lit.t == Type::String) info.superclass = *n->lit.s;
    for (int i = 0; i < n->pos_argc && i < (int)n->kw_names.size(); ++i)
        info.kw[n->kw_names[i]] = kw_value(n->kids[i].get(), n->kw_names[i], env, eng);
    if (n->str == "macroscript" && n->lit.t == Type::String && !info.kw.count("buttontext"))
        info.kw["buttontext"] = n->lit;
    info.source = n->src;

    std::vector<Node*> paramBlocks;
    for (size_t i = (size_t)n->pos_argc; i < n->kids.size(); ++i) {
        Node* k = n->kids[i].get();
        if (k->k == NK::OnHandler) {
            ScriptedHandler h;
            h.body = k->kids.empty() ? nullptr : k->kids[0].get();
            h.params = k->fn_params;
            def->handlers[k->name] = h;
            info.events.push_back(k->name);
        } else if (k->k == NK::Rollout && k->flag) {
            paramBlocks.push_back(k);
        } else if (k->k == NK::Rollout) {
            def->rollouts.push_back(k);
        } else if (k->k == NK::Scripted && k->str == "tool") {
            auto t = build_def(k, env, eng);
            def->tools[lower(k->name)] = t;
            info.tools.push_back(lower(k->name));
        } else {
            def->stmts.push_back(k);
        }
    }
    // parameters blocks
    for (Node* pb : paramBlocks) {
        std::string rolloutName;
        const size_t nh = pb->kw_names.size();
        for (size_t i = 0; i < nh && i < pb->kids.size(); ++i)
            if (pb->kw_names[i] == "rollout") rolloutName = kw_name_str(kw_value(pb->kids[i].get(), "rollout", env, eng));
        for (size_t i = nh; i < pb->kids.size(); ++i) {
            Node* it = pb->kids[i].get();
            if (it->k == NK::OnHandler) {
                ScriptedHandler h;
                h.body = it->kids.empty() ? nullptr : it->kids[0].get();
                h.params = it->fn_params;
                def->handlers[it->name + "." + it->str] = h;
                info.events.push_back(it->name + "." + it->str);
                continue;
            }
            if (it->k != NK::UiItem || it->str != "parameter") continue;
            PluginParam p;
            p.name = it->name;
            p.block = pb->name;
            p.rollout = rolloutName;
            for (size_t j = 0; j < it->kw_names.size() && j < it->kids.size(); ++j)
                p.kw[it->kw_names[j]] = kw_value(it->kids[j].get(), it->kw_names[j], env, eng);
            p.type = p.kw.count("type") ? kw_name_str(p.kw["type"]) : std::string("float");
            if (p.kw.count("default")) p.def = p.kw["default"];
            if (p.kw.count("animatable")) p.animatable = p.kw["animatable"].truthy();
            if (p.kw.count("ui")) {
                const Value& u = p.kw["ui"];
                if (u.t == Type::Array && u.arr && !u.arr->empty()) p.ui = kw_name_str((*u.arr)[0]);
                else p.ui = kw_name_str(u);
            }
            info.params.push_back(std::move(p));
        }
    }
    // join each parameter with its rollout control
    for (PluginParam& p : info.params) {
        if (p.ui.empty()) continue;
        for (Node* ro : def->rollouts) {
            if (!p.rollout.empty() && ro->name != p.rollout) continue;
            const size_t nh = ro->kw_names.size();
            for (size_t i = nh; i < ro->kids.size(); ++i) {
                Node* c = ro->kids[i].get();
                if (c->k != NK::UiItem || c->name != p.ui) continue;
                p.uiType = c->str;
                p.rolloutTitle = ro->str.empty() ? ro->name : ro->str;
                if (c->lit.t == Type::String) p.caption = *c->lit.s;
                for (size_t j = 0; j < c->kw_names.size() && j < c->kids.size(); ++j) {
                    const std::string& kn = c->kw_names[j];
                    if (kn != "range" && kn != "type" && kn != "labels" && kn != "items") continue;
                    Value v = kw_value(c->kids[j].get(), kn == "type" ? "type" : kn, env, eng);
                    if (kn == "range" && v.vec) {
                        p.hasRange = true;
                        p.rmin = v.vec->x; p.rmax = v.vec->y; p.rdef = v.vec->z;
                    } else if (kn == "type") {
                        p.uiInteger = kw_name_str(v) == "integer";
                    } else if (v.t == Type::Array && v.arr) {
                        for (auto& e : *v.arr) p.items.push_back(e.to_string());
                    }
                }
            }
        }
    }
    info.generation = eng.next_generation();
    return def;
}

static Value make_state(Engine& eng, const std::shared_ptr<ScriptedDef>& def, const std::string& key) {
    Value obj = Engine::make_object(def->info.name);
    auto& slots = obj.sinst->slots;
    if (!key.empty()) slots["__scripted"] = Value::N(key);
    for (auto& p : def->info.params) slots[p.name] = p.def;
    if (def->stmts.empty()) return obj;
    Engine::CapabilityScope capScope(eng, def->deniedCaps);   // [auditA] instance bodies stay restricted too
    std::map<std::string, Value> moved;
    Env body;
    with_staged_slots(eng, slots, [&]() -> Value {
        Env* g = eng.global_env_ptr();
        body.parent = g;
        std::set<std::string> before;
        for (auto& kv : g->vars) before.insert(kv.first);
        for (Node* st : def->stmts) eval_node(st, body, eng);
        std::vector<std::string> fresh;
        for (auto& kv : g->vars) if (!before.count(kv.first)) fresh.push_back(kv.first);
        for (auto& nm : fresh) { moved[nm] = g->vars[nm]; g->vars.erase(nm); }
        return Value::ok();
    });
    for (auto& kv : moved) slots[kv.first] = kv.second;
    for (auto& kv : body.vars) slots[kv.first] = kv.second;
    return obj;
}

Value eval_scripted(Node* n, Env& env, Engine& eng) {
    auto def = build_def(n, env, eng);
    const ScriptedInfo& info = def->info;
    if (info.kind == "macroscript") {
        const std::string key = "macroscript:" + lower(info.str("category")) + "|" + lower(info.name);
        eng.scripted_defs()[key] = def;
        if (eng.on_macroscript_defined) eng.on_macroscript_defined(info);
        return Value::I(info.generation);
    }
    if (info.kind == "plugin") {
        const std::string key = "plugin:" + lower(info.name);
        eng.scripted_defs()[key] = def;
        if (eng.on_plugin_defined) eng.on_plugin_defined(info);
        else {
            Engine* pe = &eng;
            const std::string nm = info.name;
            eng.def(nm, [pe, nm](std::vector<Value>&, std::map<std::string, Value>& kw, Env&) -> Value {
                Value inst = pe->plugin_instance(nm);
                for (auto& kv : kw) inst.sinst->slots[kv.first] = kv.second;
                return inst;
            });
        }
        return Value::C(info.name);
    }
    // tool
    const std::string key = "tool:" + lower(info.name);
    eng.scripted_defs()[key] = def;
    Value tv = Engine::make_object("Tool");
    tv.sinst->slots["__scripted"] = Value::N(key);
    tv.sinst->slots["name"] = Value::S(info.name);
    env.global()->set_local(lower(info.name), tv);
    return tv;
}

// ----------------------------------------------------------- Engine API ----
static std::vector<const ScriptedInfo*> list_kind(const std::map<std::string, std::shared_ptr<ScriptedDef>>& m,
                                                  const std::string& prefix) {
    std::vector<const ScriptedInfo*> out;
    for (auto& kv : m) if (kv.first.rfind(prefix, 0) == 0) out.push_back(&kv.second->info);
    return out;
}
std::vector<const ScriptedInfo*> Engine::macroscripts() const { return list_kind(scripted_, "macroscript:"); }
std::vector<const ScriptedInfo*> Engine::plugins() const { return list_kind(scripted_, "plugin:"); }

static std::shared_ptr<ScriptedDef> find_macro_def(const std::map<std::string, std::shared_ptr<ScriptedDef>>& m,
                                                   const std::string& category, const std::string& name,
                                                   std::string* keyOut = nullptr) {
    const std::string c = lower(category), n = lower(name);
    for (auto& kv : m) {
        if (kv.first.rfind("macroscript:", 0) != 0) continue;
        const std::string rest = kv.first.substr(12);
        const size_t bar = rest.find('|');
        if (rest.substr(bar + 1) != n) continue;
        if (!c.empty() && rest.substr(0, bar) != c) continue;
        if (keyOut) *keyOut = kv.first;
        return kv.second;
    }
    return nullptr;
}
const ScriptedInfo* Engine::find_macroscript(const std::string& category, const std::string& name) const {
    auto d = find_macro_def(scripted_, category, name);
    return d ? &d->info : nullptr;
}
const ScriptedInfo* Engine::find_plugin(const std::string& name) const {
    auto it = scripted_.find("plugin:" + lower(name));
    return it == scripted_.end() ? nullptr : &it->second->info;
}
const ScriptedInfo* Engine::find_tool(const std::string& name) const {
    auto it = scripted_.find("tool:" + lower(name));
    return it == scripted_.end() ? nullptr : &it->second->info;
}
Value Engine::plugin_instance(const std::string& name) {
    const std::string key = "plugin:" + lower(name);
    auto it = scripted_.find(key);
    if (it == scripted_.end()) serr("no scripted plugin '" + name + "'");
    return make_state(*this, it->second, key);
}
Value Engine::tool_instance(const std::string& name) {
    const std::string key = "tool:" + lower(name);
    auto it = scripted_.find(key);
    if (it == scripted_.end()) serr("no tool '" + name + "'");
    return make_state(*this, it->second, key);
}

std::shared_ptr<ScriptedDef> Engine::scripted_def_of(const Value& state) const {
    if (state.t != Type::Struct || !state.sinst) return nullptr;
    auto it = state.sinst->slots.find("__scripted");
    if (it == state.sinst->slots.end() || it->second.t != Type::Name) return nullptr;
    auto d = scripted_.find(*it->second.name);
    return d == scripted_.end() ? nullptr : d->second;
}

Value Engine::run_macroscript(const std::string& category, const std::string& name) {
    std::string key;
    auto def = find_macro_def(scripted_, category, name, &key);
    if (!def) serr("no macroScript '" + (category.empty() ? std::string() : category + ".") + name + "'");
    if (!def->handlers.count("execute")) {
        // No handlers: the body itself is the action and runs on every invocation.
        Engine* prev = g_active_engine;
        g_active_engine = this;
        Value last = Value::ok();
        CapabilityScope capScope(*this, def->deniedCaps);   // [auditA] handler-less macroScript bodies were run unrestricted
        try {
            Env local; local.parent = global_env.get();
            for (Node* st : def->stmts) last = eval_node(st, local, *this);
        } catch (ReturnFn& r) { last = r.v; } catch (...) { g_active_engine = prev; throw; }
        g_active_engine = prev;
        return last;
    }
    if (!def->macroStateReady) { def->macroState = make_state(*this, def, key); def->macroStateReady = true; }
    HandlerScope sc(*this, def->macroState);
    return sc.call("execute");
}
Value Engine::query_macroscript(const std::string& category, const std::string& name, const std::string& event) {
    std::string key;
    auto def = find_macro_def(scripted_, category, name, &key);
    if (!def || !def->handlers.count(lower(event))) return Value::unsupplied();
    if (!def->macroStateReady) { def->macroState = make_state(*this, def, key); def->macroStateReady = true; }
    HandlerScope sc(*this, def->macroState);
    return sc.call(event);
}

bool Engine::has_handler(const Value& state, const std::string& event, const std::string& tool) const {
    auto d = scripted_def_of(state);
    if (!d) return false;
    if (!tool.empty()) {
        auto t = d->tools.find(lower(tool));
        if (t == d->tools.end()) return false;
        d = t->second;
    }
    return d->handlers.count(lower(event)) != 0;
}
Value Engine::call_handler(Value& state, const std::string& event, std::vector<Value> args,
                           std::map<std::string, Value>* extras, const std::string& tool) {
    if (!has_handler(state, event, tool)) return Value::unsupplied();
    HandlerScope sc(*this, state, extras, tool);
    return sc.call(event, std::move(args));
}

// --------------------------------------------------------- HandlerScope ----
Engine::HandlerScope::HandlerScope(Engine& e, Value& state, std::map<std::string, Value>* extras,
                                   const std::string& tool)
    : eng_(e), state_(state), extras_(extras) {
    def_ = e.scripted_def_of(state);
    if (!def_) serr("not a scripted plugin / tool / macro state");
    if (!tool.empty()) {
        auto t = def_->tools.find(lower(tool));
        if (t == def_->tools.end()) serr("'" + def_->info.name + "' has no tool '" + tool + "'");
        const std::string slot = "__tool:" + lower(tool);
        auto& sl = state.sinst->slots;
        if (!sl.count(slot) || sl[slot].t != Type::Struct) sl[slot] = make_state(e, t->second, std::string());
        toolState_ = sl[slot];
        def_ = t->second;
    }
    nested_ = g_active_engine == &e;
    prevActive_ = g_active_engine;
    prevDeadline_ = e.run_deadline;
    prevDenied_ = e.denied_caps;
    e.denied_caps |= def_->deniedCaps;   // [F08]
    g_active_engine = &e;
    if (!nested_) {
        e.abort_flag = false;
        e.run_deadline = e.timeout_ms ? (std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch()).count() +
                                         (long long)e.timeout_ms)
                                      : 0;
    }
    Env* g = e.global_env_ptr();
    auto stage = [&](const std::map<std::string, Value>& m) {
        for (auto& kv : m) {
            if (!saved_.count(kv.first) && std::find(added_.begin(), added_.end(), kv.first) == added_.end()) {
                auto ex = g->vars.find(kv.first);
                if (ex != g->vars.end()) saved_[kv.first] = ex->second;
                else added_.push_back(kv.first);
            }
            g->vars[kv.first] = kv.second;
        }
    };
    stage(state.sinst->slots);
    if (toolState_.sinst) stage(toolState_.sinst->slots);
    if (extras_) stage(*extras_);
}
void Engine::HandlerScope::restore() {
    Env* g = eng_.global_env_ptr();
    auto back = [&](std::map<std::string, Value>& m) {
        for (auto& kv : m) {
            if (kv.first == "__scripted") continue;          // identity of the state, never rebound
            auto cur = g->vars.find(kv.first);
            if (cur != g->vars.end()) kv.second = cur->second;
        }
    };
    back(state_.sinst->slots);
    if (toolState_.sinst) back(toolState_.sinst->slots);
    if (extras_) back(*extras_);
    for (auto& kv : saved_) g->vars[kv.first] = kv.second;
    for (auto& nm : added_) g->vars.erase(nm);
    g_active_engine = prevActive_;
    eng_.denied_caps = prevDenied_;
    if (!nested_) eng_.run_deadline = prevDeadline_;
}
Engine::HandlerScope::~HandlerScope() { restore(); }
bool Engine::HandlerScope::has(const std::string& event) const { return def_->handlers.count(lower(event)) != 0; }
Value Engine::HandlerScope::call(const std::string& event, std::vector<Value> args) {
    auto it = def_->handlers.find(lower(event));
    if (it == def_->handlers.end() || !it->second.body) return Value::unsupplied();
    Env local;
    local.parent = eng_.global_env_ptr();
    for (size_t k = 0; k < it->second.params.size(); ++k)
        local.set_local(it->second.params[k], k < args.size() ? args[k] : Value::undef());
    try { return eval_node(it->second.body, local, eng_); }
    catch (ReturnFn& r) { return r.v; }
}
Value Engine::HandlerScope::get(const std::string& var) const {
    Value* v = eng_.global_env_ptr()->find(lower(var));
    return v ? *v : Value::undef();
}
void Engine::HandlerScope::set(const std::string& var, const Value& v) { eng_.global_env_ptr()->vars[lower(var)] = v; }

// ---------------------------------------------------- persistent globals ----
void Engine::clear_persistents(bool alsoValues) {
    if (alsoValues) for (auto& n : persistents_) global_env->vars.erase(n);
    persistents_.clear();
}

static std::string num_lit(double d) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.9g", d);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}
std::string Engine::serialize_value(const Value& v) const {
    switch (v.t) {
        case Type::Undefined: return "undefined";
        case Type::Ok: return "ok";
        case Type::Unsupplied: return "unsupplied";
        case Type::Bool: return v.b ? "true" : "false";
        case Type::Int: return std::to_string(v.i);
        case Type::Float: return num_lit(v.f);
        case Type::String: {
            std::string o = "\"";
            for (char c : *v.s) {
                switch (c) {
                    case '"': o += "\\\""; break;
                    case '\\': o += "\\\\"; break;
                    case '\n': o += "\\n"; break;
                    case '\r': o += "\\r"; break;
                    case '\t': o += "\\t"; break;
                    default: o += c;
                }
            }
            return o + "\"";
        }
        case Type::Name: {
            bool plain = !v.name->empty() && !isdigit((unsigned char)(*v.name)[0]);
            for (char c : *v.name) if (!isalnum((unsigned char)c) && c != '_') plain = false;
            if (plain) return "#" + *v.name;
            if (v.name->find('\'') != std::string::npos) return std::string();
            return "#'" + *v.name + "'";
        }
        case Type::Point2: return "[" + num_lit(v.vec->x) + "," + num_lit(v.vec->y) + "]";
        case Type::Point3: return "[" + num_lit(v.vec->x) + "," + num_lit(v.vec->y) + "," + num_lit(v.vec->z) + "]";
        case Type::Point4: return "[" + num_lit(v.vec->x) + "," + num_lit(v.vec->y) + "," + num_lit(v.vec->z) + "," + num_lit(v.vec->w) + "]";
        case Type::Color: return "(color " + num_lit(v.vec->x) + " " + num_lit(v.vec->y) + " " + num_lit(v.vec->z) + " " + num_lit(v.vec->w) + ")";
        case Type::Quat: return "(quat " + num_lit(v.vec->x) + " " + num_lit(v.vec->y) + " " + num_lit(v.vec->z) + " " + num_lit(v.vec->w) + ")";
        case Type::Matrix3: {
            std::string o = "(matrix3";
            for (int r = 0; r < 4; ++r)
                o += " [" + num_lit(v.mat->m[r][0]) + "," + num_lit(v.mat->m[r][1]) + "," + num_lit(v.mat->m[r][2]) + "]";
            return o + ")";
        }
        case Type::Array: {
            CycleGuard cg(v.arr.get());   // [auditA] a cyclic array cannot be written as a literal
            if (cg.cyclic) return std::string();
            std::string o = "#(";
            for (size_t k = 0; k < v.arr->size(); ++k) {
                std::string e = serialize_value((*v.arr)[k]);
                if (e.empty()) return std::string();
                if (k) o += ", ";
                o += e;
            }
            return o + ")";
        }
        case Type::BitArray: {
            std::string o = "#{";
            bool first = true;
            for (size_t k = 0; k < v.bits->size(); ++k)
                if ((*v.bits)[k]) { if (!first) o += ","; o += std::to_string(k + 1); first = false; }
            return o + "}";
        }
        case Type::Time: return num_lit(v.f) + "f";
        case Type::Interval: return "(interval " + num_lit(v.vec->x) + "f " + num_lit(v.vec->y) + "f)";
        case Type::EulerAngles: return "(eulerAngles " + num_lit(v.vec->x) + " " + num_lit(v.vec->y) + " " + num_lit(v.vec->z) + ")";
        case Type::AngleAxis: return "(angleAxis " + num_lit(v.vec->w) + " [" + num_lit(v.vec->x) + "," + num_lit(v.vec->y) + "," + num_lit(v.vec->z) + "])";
        case Type::Dictionary: {
            std::string o = "(Dictionary #" + v.dict->keyType;
            for (auto& kv : v.dict->items) {
                std::string k = serialize_value(kv.first), x = serialize_value(kv.second);
                if (k.empty() || x.empty()) return std::string();
                o += " #(" + k + ", " + x + ")";
            }
            return o + ")";
        }
        case Type::Reference: return v.ref ? serialize_value(v.ref->v) : std::string();
        default:
            return serialize_hook ? serialize_hook(v) : std::string();
    }
}
Value Engine::deserialize_value(const std::string& text) {
    // [auditA] persistent-global literals come from scene files: never let them touch files / processes
    CapabilityScope capScope(*this, CapFile | CapProcess);
    Parser p(text, "<persistent>");
    NodeP root = p.parse_program();
    Node* r = root.get();
    ast_roots.push_back(std::move(root));
    Engine* prev = g_active_engine;
    g_active_engine = this;
    Value v;
    try { v = eval_node(r, *global_env, *this); } catch (...) { g_active_engine = prev; throw; }
    g_active_engine = prev;
    return v;
}

// ------------------------------------------------------------- builtins ----
void install_scripted(Engine& e, Env& g) {
    Value dc; dc.t = Type::DontCollect;
    g.set_local("dontcollect", dc);
    Engine* pe = &e;

    // macros.run <category> <name>  |  macros.run <name>
    Value macros = Engine::make_namespace("macros");
    Engine::ns_def(macros, "run", [pe](std::vector<Value>& a, std::map<std::string, Value>&, Env&) -> Value {
        if (a.empty()) serr("macros.run <category> <name>");
        if (a.size() == 1) return pe->run_macroscript(std::string(), a[0].to_string());
        return pe->run_macroscript(a[0].to_string(), a[1].to_string());
    });
    Engine::ns_def(macros, "list", [pe](std::vector<Value>&, std::map<std::string, Value>&, Env&) -> Value {
        Array_t out;
        for (auto* m : pe->macroscripts()) out.push_back(Value::A({Value::S(m->str("category")), Value::S(m->name)}));
        return Value::A(std::move(out));
    });
    Engine::ns_def(macros, "ischecked", [pe](std::vector<Value>& a, std::map<std::string, Value>&, Env&) -> Value {
        if (a.size() < 2) serr("macros.isChecked <category> <name>");
        Value v = pe->query_macroscript(a[0].to_string(), a[1].to_string(), "ischecked");
        return v.t == Type::Unsupplied ? Value::B(false) : v;
    });
    Engine::ns_def(macros, "isenabled", [pe](std::vector<Value>& a, std::map<std::string, Value>&, Env&) -> Value {
        if (a.size() < 2) serr("macros.isEnabled <category> <name>");
        Value v = pe->query_macroscript(a[0].to_string(), a[1].to_string(), "isenabled");
        return v.t == Type::Unsupplied ? Value::B(true) : v;
    });
    g.set_local("macros", macros);

    // startTool <tool> [prompt:] [snap:] -- the host drives it; stopTool is a no-op here.
    e.def("startTool", [pe](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) -> Value {
        auto d = a.empty() ? nullptr : pe->scripted_def_of(a[0]);
        if (!d || d->info.kind != "tool") serr("startTool: tool expected");
        if (pe->start_tool_hook) return pe->start_tool_hook(d->info.name, kw);
        if (pe->on_output) pe->on_output("-- startTool " + d->info.name + ": no interactive host\n");
        return Value::undef();
    });
    e.def("stopTool", [](std::vector<Value>&, std::map<std::string, Value>&, Env&) -> Value { return Value::ok(); });

    // persistents.isPersistent #name / persistents.remove #name / persistents.removeAll()
    Value pers = Engine::make_namespace("persistents");
    Engine::ns_def(pers, "ispersistent", [pe](std::vector<Value>& a, std::map<std::string, Value>&, Env&) -> Value {
        return Value::B(!a.empty() && pe->persistent_names().count(lower(a[0].to_string())) != 0);
    });
    Engine::ns_def(pers, "make", [pe](std::vector<Value>& a, std::map<std::string, Value>&, Env&) -> Value {
        if (!a.empty()) pe->add_persistent(a[0].to_string());
        return Value::ok();
    });
    Engine::ns_def(pers, "remove", [pe](std::vector<Value>& a, std::map<std::string, Value>&, Env&) -> Value {
        if (a.empty()) return Value::ok();
        std::set<std::string> keep = pe->persistent_names();
        keep.erase(lower(a[0].to_string()));
        pe->clear_persistents(false);
        for (auto& n : keep) pe->add_persistent(n);
        return Value::ok();
    });
    Engine::ns_def(pers, "removeall", [pe](std::vector<Value>&, std::map<std::string, Value>&, Env&) -> Value {
        pe->clear_persistents(false);
        return Value::ok();
    });
    Engine::ns_def(pers, "gather", [pe](std::vector<Value>&, std::map<std::string, Value>&, Env&) -> Value {
        Array_t out;
        for (auto& n : pe->persistent_names()) out.push_back(Value::N(n));
        return Value::A(std::move(out));
    });
    g.set_local("persistents", pers);
}

}  // namespace mxs
