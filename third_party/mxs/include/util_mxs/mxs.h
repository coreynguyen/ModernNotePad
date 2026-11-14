// util_mxs — MAXScript interpreter.
// Independent implementation of the MAXScript language core (John Wainwright,
// 1996 design), following the public reference documentation.
// No 3ds Max dependencies. C++17, OS-agnostic.
#pragma once
#include <cstdint>
#include <functional>
#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

namespace mxs {

// ---------------------------------------------------------------- values ----
enum class Type : uint8_t {
    Undefined, Ok, Unsupplied,     // singletons
    Bool, Int, Float,              // immediates (Int is int64; MXS prints as 32-bit int semantics)
    Name, String,                  // Name = interned, case-insensitive
    Array, BitArray,
    Point2, Point3, Point4, Quat, Matrix3, Color,
    FileStream, StringStream,
    Function, NativeFn, Struct, StructDef,
    // The result of classOf(): a bare class reference. Prints WITHOUT a '#'
    // (e.g. classOf 5 -> Integer, not #Integer) -- confirmed against the
    // official docs (Structure-Definition topic: "classOf bill --returns
    // person"). Common class names (Integer, Float, String, Array, ...) are
    // also bound as globals of this type, so `classOf x == Integer` -- the
    // idiom every real MAXScript uses -- works without quoting or '#'.
    Class,
    DontCollect,   // the 'dontCollect' singleton: a collect loop drops it
    // ---- appended (keep the order above stable: classOf tables index it) ----
    Time,          // f = frames (MAX 30 fps / 160 ticks per frame); prints 10f
    Interval,      // vec->x start, vec->y end (frames); prints (interval 0f 100f)
    EulerAngles,   // vec->x/y/z degrees
    AngleAxis,     // vec->x/y/z axis, vec->w angle (degrees)
    Dictionary,    // dict (Max 2017+ value; harmless addition)
    Reference,     // ref: by-reference box created by '&var' arguments
};

struct Value;
using Array_t = std::vector<Value>;
struct Env;
struct FnDef;         // scripted function
struct StructDef_t;
struct StructInst;
struct FileStream_t;
struct StringStream_t;
struct ScriptedDef;   // macroScript / plugin / tool definition (internal)
struct Dict_t;        // Dictionary storage
struct RefBox_t;      // '&var' by-reference box

// Native function: (args, keyword-args, env) -> Value
using NativeFn_t = std::function<Value(std::vector<Value>&,
                                       std::map<std::string, Value>&, Env&)>;

struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Mat3 { float m[4][3] = {{1,0,0},{0,1,0},{0,0,1},{0,0,0}}; }; // MXS Matrix3: row1..row3, row4=translation

struct Value {
    Type t = Type::Undefined;
    union { bool b; int64_t i; double f; };
    const std::string* name = nullptr;                 // Type::Name (interned, original spelling)
    std::shared_ptr<std::string>    s;                 // String
    std::shared_ptr<Array_t>        arr;               // Array
    std::shared_ptr<std::vector<bool>> bits;           // BitArray
    std::shared_ptr<Vec4>           vec;               // Point2/3/4, Quat(x,y,z,w), Color(r,g,b,a)
    std::shared_ptr<Mat3>           mat;
    std::shared_ptr<FnDef>          fn;
    std::shared_ptr<NativeFn_t>     nat;
    const char*                     nat_name = nullptr;
    std::shared_ptr<StructDef_t>    sdef;
    std::shared_ptr<StructInst>     sinst;
    std::shared_ptr<FileStream_t>   fs;
    std::shared_ptr<StringStream_t> ss;
    std::shared_ptr<Dict_t>         dict;
    std::shared_ptr<RefBox_t>       ref;

    Value() : i(0) {}
    static Value undef()            { return Value{}; }
    static Value ok()               { Value v; v.t = Type::Ok; return v; }
    static Value unsupplied()       { Value v; v.t = Type::Unsupplied; return v; }
    static Value B(bool x)          { Value v; v.t = Type::Bool; v.b = x; return v; }
    static Value I(int64_t x)       { Value v; v.t = Type::Int;  v.i = x; return v; }
    static Value F(double x)        { Value v; v.t = Type::Float; v.f = x; return v; }
    static Value S(std::string x);
    static Value N(const std::string& x);              // interns
    static Value M3();                                  // identity matrix3
    static Value C(const std::string& x);               // classOf() result; interns, prints bare (no '#')
    static Value A(Array_t x = {});
    static Value P2(float x, float y);
    static Value P3(float x, float y, float z);
    static Value P4(float x, float y, float z, float w);
    static Value QuatV(float x, float y, float z, float w);
    static Value ColorV(float r, float g, float b, float a = 255.f);
    static Value T(double frames);                       // Time value (rounded to whole ticks)
    static Value Ival(double startFrames, double endFrames);
    static Value Euler(float x, float y, float z);
    static Value AngAx(float angleDeg, float ax, float ay, float az);

    // Time is number-like: num() gives frames, so hosts reading 'at time 10f'
    // or 'sliderTime' as a plain number keep working.
    bool is_num()   const { return t == Type::Int || t == Type::Float || t == Type::Time; }
    double num()    const { return t == Type::Int ? double(i) : f; }
    // [auditA] float -> int saturates (NaN -> 0): a direct cast of an out-of-range double is UB
    int64_t inum()  const { return t == Type::Int ? i : f != f ? 0 : f >= 9.2e18 ? INT64_MAX : f <= -9.2e18 ? INT64_MIN : int64_t(f); }
    bool truthy()   const;                             // MXS: only true is true; on/off map to bool
    bool eq(const Value& o) const;                     // '==' semantics
    std::string to_display() const;                    // listener representation (strings quoted)
    std::string to_string()  const;                    // 'as string' (strings unquoted)
};

// Dictionary: insertion-ordered key/value list. keyType is #name, #string or
// #integer (Max 2017 semantics: keys of one kind per dictionary).
struct Dict_t {
    std::string keyType = "name";
    std::vector<std::pair<Value, Value>> items;
    int find(const Value& k) const;                     // index or -1
};
// '&var' argument: the callee sees (and may replace) the box's value; the
// caller copies it back into the variable after the call when 'dirty'.
struct RefBox_t { Value v; bool dirty = false; };

// Out-parameters for NATIVE functions: inside a native call, an argument that
// was passed as '&var' can be written back with these (index is 0-based over
// the positional arguments). Return false when that argument was not '&'.
bool set_out_arg(size_t index, const Value& v);
bool set_out_kwarg(const std::string& lname, const Value& v);
bool is_out_arg(size_t index);

// Interned, case-insensitive name table (#names and identifiers share it)
const std::string* intern(const std::string& s);       // returns canonical ptr, keeps first spelling
std::string lower(std::string s);

// ---------------------------------------------------------------- errors ----
struct MxsError : std::runtime_error {
    Value thrown;      // value carried by throw() / runtime error message string
    int line;
    MxsError(std::string msg, int line_ = 0)
        : std::runtime_error(std::move(msg)), line(line_) { thrown = Value::S(what()); }
};
struct ExitLoop     { Value v; };                      // 'exit [with expr]'
struct ContinueLoop {};
struct ReturnFn     { Value v; };

// ------------------------------------------------------------------ env -----
struct Env {
    std::map<std::string, Value> vars;                 // keys are lowercased
    Env* parent = nullptr;
    Env* global();
    Value* find(const std::string& lname);             // scope-chain lookup
    void   set_local(const std::string& lname, Value v) { vars[lname] = std::move(v); }
};

// ---------------------------------------------------------------- engine ----
struct Node;                                           // AST (internal)

// ---- rollout description (for hosts that realise the UI) ------------------
// The engine parses rollouts and evaluates their control declarations, but has
// no idea what a window is. A host (the GX2 IDE) asks for this description,
// builds real controls from it, and calls invoke_handler() on events.
struct UiControl {
    std::string type;          // "button", "checkbox", ...
    std::string name;          // variable name inside the rollout
    std::string caption;       // display text, may be empty
    std::map<std::string, Value> params;   // width:, align:, checked:, ...
};
struct UiHandler {
    std::string control;       // control name, or the rollout name for #open/#close
    std::string event;         // "pressed", "changed", "open", ...
};
// ---- scripted definitions (macroScript / plugin / tool) ---------------------
// A parameter declared in a scripted plugin's 'parameters' block, joined with
// the rollout control its ui: names (so a host can build a native panel).
struct PluginParam {
    std::string name;          // lowercase
    std::string type;          // lowercase type name: float, integer, boolean, color, point3,
                               // string, angle, worldunits, percent, node, material, texturemap ...
    std::string block;         // parameters block name
    std::string rollout;       // rollout: of the block (lowercase), may be empty
    std::string ui;            // ui: control name (lowercase), may be empty
    Value def;                 // default: (Undefined if none)
    bool animatable = true;
    std::map<std::string, Value> kw;   // all keywords as evaluated
    // From the ui control, when found in the plugin body:
    std::string uiType;        // spinner, checkbox, colorpicker, radiobuttons, edittext, ...
    std::string caption;       // control caption
    std::string rolloutTitle;  // the rollout's title
    bool hasRange = false;
    double rmin = 0, rmax = 0, rdef = 0;
    std::vector<std::string> items;    // radiobuttons labels: / dropdownlist items:
    bool uiInteger = false;            // spinner type:#integer
};
struct ScriptedInfo {
    std::string kind;          // "macroscript", "plugin", "tool"
    std::string name;          // as written
    std::string superclass;    // plugins: lowercase superclass (simpleobject, simplemod, material, helper ...)
    std::map<std::string, Value> kw;   // header keywords; identifiers (extends:, icon names) come back as Names
    std::vector<std::string> events;   // handler names, lowercase ("execute", "buildmesh", "map", ...)
    std::vector<std::string> tools;    // nested tool names (plugins), lowercase
    std::vector<PluginParam> params;
    std::string source;        // exact source text of the definition (for scene persistence)
    int generation = 0;        // bumps on every (re)definition
    unsigned deniedCaps = 0;   // [auditA] Engine::denied_caps at definition time (persisted with the source)
    std::string str(const std::string& key) const;   // kw as string ("" if absent)
    bool has(const std::string& event) const;
};

struct RolloutDef {
    std::string name, title;
    std::vector<UiControl> controls;
    std::vector<UiHandler> handlers;
    std::map<std::string, Value> params;   // rollout-level width:, height:
    unsigned deniedCaps = 0;               // [F08] Engine::denied_caps at definition time
};

class Engine {
public:
    Engine();
    ~Engine();

    // Compile+run a whole source buffer (file semantics: top-level expr sequence).
    // Returns value of last expression. Throws MxsError on lex/parse/runtime error.
    Value execute(const std::string& source, const std::string& source_name = "<input>");

    // Listener output hook (print/format/... route here). Default: stdout.
    std::function<void(const std::string&)> on_output;

    // Register a native global:  eng.def("doesFileExist", fn)
    void def(const std::string& name, NativeFn_t fn);
    // Register/overwrite a global variable
    void set_global(const std::string& name, Value v);
    Value get_global(const std::string& name);

    Env& globals() { return *global_env; }

    // Namespace objects for embedders:  gx2.showScene() style APIs.
    //   Value ns = Engine::make_namespace("gx2");
    //   Engine::ns_def(ns, "showScene", fn);
    //   eng.set_global("gx2", ns);
    static Value make_namespace(const std::string& name);
    static void  ns_def(Value& ns, const std::string& name, NativeFn_t fn);

    // Generic script-visible objects for hosts (scene nodes, materials, ...).
    // Same underlying representation as a struct instance, but hosts get at it
    // through these instead of the internal StructInst type.
    // Build a callable value WITHOUT registering it globally, so a host can
    // store per-object methods in an object's slots (e.g. a modifier handle
    // whose SetNormal is bound to the mesh it was added to).
    // Call a callable value from host code (a native, a script function, or a
    // struct method), without exposing the internal AST types.
    Value call_value_public(Value& callee, std::vector<Value>& args,
                            std::map<std::string, Value>& kwargs, Env& env);

    static Value make_native(const std::string& name, NativeFn_t fn);
    static Value make_object(const std::string& className);
    static void  obj_set(Value& obj, const std::string& prop, const Value& v);
    static Value obj_get(const Value& obj, const std::string& prop);
    static bool  obj_is(const Value& obj, const std::string& className);
    static bool  obj_same(const Value& a, const Value& b);   // identity

    // Hosts can intercept property get/set on their own object classes, so
    // 'node.transform = m' reaches the real scene instead of just writing a
    // slot. Return false to fall through to normal slot behaviour.
    using PropGetHook = std::function<bool(const Value&, const std::string&, Value&)>;
    using PropSetHook = std::function<bool(Value&, const std::string&, const Value&)>;
    void set_object_property_hooks(PropGetHook g, PropSetHook s) {
        prop_get_hook = std::move(g); prop_set_hook = std::move(s);
    }
    PropGetHook prop_get_hook;
    PropSetHook prop_set_hook;
    // Host collections indexed by #name / "string" (node.modifiers[#Bend]),
    // and host objects (Struct values) indexed by a number (multiMat[3]).
    // Return false to fall through to the engine's own indexing / errors.
    std::function<bool(const Value& coll, const Value& index, Value& out)> index_get_hook;

    // Cooperative abort. A host (GUI toolbar "Stop", a watchdog thread) sets the
    // flag; loop heads and call entry poll it and unwind with MxsError("aborted").
    // Cleared automatically at the start of every execute().
    // Parse without executing: lets a real script be validated for syntax
    // coverage without needing its data files or a scene.
    void parse_only(const std::string& source, const std::string& source_name = "<mxs>");
    // ---- rollouts -------------------------------------------------------
    // Definitions collected as rollout expressions are evaluated.
    const RolloutDef* find_rollout(const std::string& name) const;
    std::vector<std::string> rollout_names() const;
    // Run 'on <control> <event> do ...'. Returns false if there is no such
    // handler; script errors propagate as MxsError like any other call.
    bool invoke_handler(const std::string& rollout, const std::string& control,
                        const std::string& event);
    // Control properties live in the rollout's value, so a handler that does
    // 'btn.enabled = false' and the host window stay in sync.
    Value get_control_prop(const std::string& rollout, const std::string& control,
                           const std::string& prop) const;
    void  set_control_prop(const std::string& rollout, const std::string& control,
                           const std::string& prop, const Value& v);

    // ---- global hygiene --------------------------------------------------
    // MAXScript globals persist until 3ds Max restarts, which is why scripts
    // get wrapped in rollouts and why a tool can work one day and crash the
    // next on a stale or colliding name. These make that state inspectable and
    // resettable without restarting the host.
    void reset_globals();                        // drop user globals, keep builtins
    std::vector<std::string> user_global_names() const;
    std::vector<std::string> all_global_names() const;   // including builtins
    void mark_builtins_complete();               // call once after install_*()

    Env* global_env_ptr() { return global_env.get(); }

    // ---- context hook with full qualifier values -----------------------
    // Preferred over ctx_hook when set. 'what' is the context word
    // (animate, time, coordsys, about, undo, redraw, quiet, level, in);
    // qual its evaluated qualifier (#local / a node / 10 / true ...), label
    // the optional undo label. mode: 0 enter, 1 exit, 2 'set <ctx> <q>'
    // (persistent, no exit).
    enum CtxMode { CtxEnter = 0, CtxExit = 1, CtxSet = 2 };
    using CtxHookV = std::function<void*(const std::string& what, const Value& qual,
                                         const Value& label, int mode, void* token)>;
    CtxHookV ctx_hook_v;

    // ---- watched globals ---------------------------------------------------
    // Called after a script assigns the GLOBAL 'name' (lowercase key), e.g.
    // 'sliderTime = 10' or 'animButtonState = true' reaching the host.
    std::map<std::string, std::function<void(const Value&)>> global_watch;
    // Live globals: read through the host instead of the stored value
    // (lowercase key), e.g. 'subObjectLevel' reflecting the Modify panel.
    std::map<std::string, std::function<Value()>> global_read;

    // ---- persistent globals ---------------------------------------------
    // 'persistent global x = ...' names. A host saves their values with the
    // scene (serialize_value) and restores them on load (deserialize_value).
    const std::set<std::string>& persistent_names() const { return persistents_; }
    void add_persistent(const std::string& name) { persistents_.insert(lower_name(name)); }
    void clear_persistents(bool alsoValues);
    // Literal text that re-creates v when executed, or "" when v cannot be
    // written as a literal (functions, structs, files...). Host objects are
    // offered to serialize_hook first.
    std::string serialize_value(const Value& v) const;
    std::function<std::string(const Value&)> serialize_hook;
    // Listener representation of host objects (nodes print as $Box:Box001 @ [0,0,0],
    // controllers as Controller:Bezier_Float ...). Return true to take over.
    static std::function<bool(const Value&, std::string&)>& display_hook();
    // [mxs25 F] Host identity for wrapper objects: two distinct wrapper structs
    // that refer to the same scene node / animatable compare equal (==, !=,
    // findItem, appendIfUnique, obj_same). Return 1 same, 0 different, -1 n/a.
    static std::function<int(const Value&, const Value&)>& identity_hook();
    Value deserialize_value(const std::string& text);

    // ---- macroScripts ----------------------------------------------------
    std::vector<const ScriptedInfo*> macroscripts() const;
    // Empty category matches any category. Names are case-insensitive.
    const ScriptedInfo* find_macroscript(const std::string& category, const std::string& name) const;
    // Run 'on execute' (or the whole body when there is no execute handler).
    // Throws MxsError when not found or on a script error.
    Value run_macroscript(const std::string& category, const std::string& name);
    // isChecked / isEnabled / isVisible / isIndeterminate: the handler's value,
    // or Value::unsupplied() when the macro has no such handler.
    Value query_macroscript(const std::string& category, const std::string& name, const std::string& event);
    std::function<void(const ScriptedInfo&)> on_macroscript_defined;

    // ---- scripted plugins --------------------------------------------------
    std::vector<const ScriptedInfo*> plugins() const;
    const ScriptedInfo* find_plugin(const std::string& name) const;
    // Called after every plugin definition (and redefinition). A host registers
    // a real class here. Without a hook the plugin name is bound to a script
    // constructor that returns a bare instance (for tests / standalone use).
    std::function<void(const ScriptedInfo&)> on_plugin_defined;
    // New per-object script state: parameter defaults + the body's locals and
    // functions. Its handlers run with these as variables (copy-in/copy-out).
    Value plugin_instance(const std::string& name);
    // ---- tools -------------------------------------------------------------
    const ScriptedInfo* find_tool(const std::string& name) const;
    Value tool_instance(const std::string& name);   // state for a top-level tool
    // startTool <tool> [prompt:...]: the host drives the tool from mouse input
    // (via HandlerScope on tool_instance). Without a hook startTool reports
    // #nohost and returns undefined.
    std::function<Value(const std::string& toolName, std::map<std::string, Value>& kw)> start_tool_hook;

    // ---- handler invocation (plugin / tool / macro state objects) ---------
    // Stages the state's variables (plus 'extras', e.g. mesh/this/delegate/
    // worldPoint) into scope for one or many handler calls; on destruction the
    // values handlers assigned are written back to the state and to extras.
    // 'tool' selects a nested tool of a plugin (e.g. "create").
    class HandlerScope {
    public:
        HandlerScope(Engine& e, Value& state, std::map<std::string, Value>* extras = nullptr,
                     const std::string& tool = std::string());
        ~HandlerScope();
        HandlerScope(const HandlerScope&) = delete;
        HandlerScope& operator=(const HandlerScope&) = delete;
        bool has(const std::string& event) const;
        Value call(const std::string& event, std::vector<Value> args = {});
        // Read/write a staged variable during the scope.
        Value get(const std::string& var) const;
        void  set(const std::string& var, const Value& v);
    private:
        void restore();
        Engine& eng_;
        Value& state_;
        Value toolState_;
        std::map<std::string, Value>* extras_;
        std::shared_ptr<ScriptedDef> def_;
        std::map<std::string, Value> saved_;
        std::vector<std::string> added_;
        bool nested_ = false;
        Engine* prevActive_ = nullptr;
        long long prevDeadline_ = 0;
        unsigned prevDenied_ = 0;
    };
    // Convenience: one handler call. Returns unsupplied when there is no handler.
    Value call_handler(Value& state, const std::string& event, std::vector<Value> args = {},
                       std::map<std::string, Value>* extras = nullptr, const std::string& tool = std::string());
    bool has_handler(const Value& state, const std::string& event, const std::string& tool = std::string()) const;

    // internal (scripted.cpp)
    std::map<std::string, std::shared_ptr<ScriptedDef>>& scripted_defs() { return scripted_; }
    std::shared_ptr<ScriptedDef> scripted_def_of(const Value& state) const;
    int next_generation() { return ++generation_; }

    // Context hook: called on entry to and exit from 'at time t (...)',
    // 'animate on (...)' etc. A host that records animation uses it to move the
    // keyframe cursor. Return an opaque token on entry; it is handed back on
    // exit so the previous state can be restored (contexts nest).
    using CtxHook = std::function<void*(const std::string& what, double value,
                                        bool entering, void* token)>;
    CtxHook ctx_hook;

    // Scene path resolver: '$foo', '$*', '$' (current selection). A host with
    // a node registry registers this; without one, '$' raises a clear error
    // rather than pretending to work.
    using ScenePathHook = std::function<Value(const std::string& path)>;
    ScenePathHook scene_path_hook;
    // [mxsfix 1c] 'max <command words>' (2025 "3ds MAX Commands"). Gets the lower-cased words
    // joined by single spaces ("zoomext sel all"); returns true when it ran the command.
    // Hosts chain it (keep the previous hook and call it for commands they don't own).
    using MaxCommandHook = std::function<bool(const std::string& cmd)>;
    MaxCommandHook max_command_hook;
    // [mxs25 F] 'when <attr> <objs> change[s] ... do <expr>': attr is lowercase ("deleted" for the
    // deletion form), kw holds id:/handleat:, fn takes the optional object parameter. Returns the
    // ChangeHandler value.
    std::function<Value(const std::string& attr, const Value& objs, std::map<std::string, Value>& kw, const Value& fn)> when_hook;

    // ---- capabilities (MAX2Modern [F08]) ------------------------------------
    // Natives can be tagged with capability bits. While a bit is set in
    // denied_caps, every call to a native tagged with it throws an MxsError
    // "permission denied" (catchable by script try/catch, but the native never
    // runs). The gate is in the single native dispatch point, so aliases
    // (f = deleteFile; f "x") and namespace/object-slot natives with the same
    // name are covered. Tags are by lowercase native name. Hosts with natives
    // that only sometimes touch files (render outputfile:) call
    // require_capability() inside the native.
    enum Capability : unsigned { CapFile = 1u, CapProcess = 2u };
    unsigned denied_caps = 0;
    void tag_capability(const std::string& nativeName, unsigned caps);   // OR-ed in
    unsigned capability_of(const char* natName) const;
    // Throws MxsError("permission denied: <what> ...") if any bit of cap is denied.
    void require_capability(unsigned cap, const std::string& what) const;
    // RAII: deny additional capabilities for a scope (nests; restores on exit).
    struct CapabilityScope {
        Engine& e; unsigned prev;
        CapabilityScope(Engine& eng, unsigned deny) : e(eng), prev(eng.denied_caps) { e.denied_caps |= deny; }
        ~CapabilityScope() { e.denied_caps = prev; }
        CapabilityScope(const CapabilityScope&) = delete;
        CapabilityScope& operator=(const CapabilityScope&) = delete;
    };

    void request_abort() { abort_flag = true; }
    // Throws MxsError if abort was requested or the run deadline elapsed.
    void poll_abort(int line);
    bool aborting() const { return abort_flag; }
    // Optional watchdog: throw if one execute() exceeds this many milliseconds
    // (0 = unlimited). Guards runaway importer loops in the GUI.
    unsigned long timeout_ms = 0;
    // Reading a variable that was never declared or assigned raises
    // "Undefined variable" instead of yielding undefined. MAXScript itself is
    // lenient; the conformance harness turns this on so a documented example
    // whose setup was not run is reported as skipped, not as a wrong value.
    bool strict_undefined_reads = false;

    // Polled by the evaluator; public so builtins in other TUs can check it.
    // Rollout definitions + handler bodies (opaque Node*), keyed lowercase.
    std::map<std::string, RolloutDef>& rollouts() { return rollouts_; }
    std::map<std::string, const void*>& handler_bodies() { return handler_bodies_; }

    std::atomic<bool> abort_flag{false};
    long long run_deadline = 0;      // steady_clock ms; 0 = none

private:
    static std::string lower_name(const std::string& s);
    std::set<std::string> persistents_;
    // key: "macroscript:<category>|<name>", "plugin:<name>", "tool:<name>" (lowercase)
    std::map<std::string, std::shared_ptr<ScriptedDef>> scripted_;
    int generation_ = 0;
    std::unique_ptr<Env> global_env;
    std::map<std::string, RolloutDef> rollouts_;
    std::map<std::string, const void*> handler_bodies_;
    std::set<std::string> builtin_names;   // snapshot taken by mark_builtins_complete()
    std::map<std::string, unsigned> cap_tags_;   // lowercase native name -> Capability bits
    // AST storage for the lifetime of THIS engine. Function bodies close over
    // Node* pointers into these trees, so they must outlive every call, but
    // they must NOT outlive the Engine via a separate static side-table --
    // see the comment on the old states()/EngineState design that this
    // replaced (util_mxs commit fixing GH crash: static destruction order
    // fiasco when a process-lifetime Engine, e.g. a function-local static
    // singleton, outlived a *different* function-local static side-table).
    // Node is forward-declared above; this is fine as a direct member because
    // ~Engine() is declared here but defined out-of-line in eval.cpp, where
    // Node is complete -- same pattern as global_env's unique_ptr<Env>.
    std::vector<std::unique_ptr<Node>> ast_roots;
    std::unordered_map<std::string, Node*> ast_cache_;   // [auditA] (source name, source) -> parsed root in ast_roots
    void install_builtins();
public:
    size_t ast_root_count() const { return ast_roots.size(); }   // [auditA] diagnostics / tests
};

} // namespace mxs
