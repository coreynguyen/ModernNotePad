#pragma once
#include "util_mxs/mxs.h"
#include <algorithm>
#include <cstdint>
#include <vector>
#include <variant>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mxs {

// [auditA] recursion guard: true when this thread's native stack is nearly exhausted (stackguard.cpp)
uintptr_t stack_limit_for_thread();
inline bool stack_exhausted() {
    static thread_local const uintptr_t lim = stack_limit_for_thread();
#if defined(_MSC_VER)
    const uintptr_t here = (uintptr_t)_AddressOfReturnAddress();
#else
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
#endif
    return lim != 0 && here < lim;
}
// [auditA] Recursion guard for printing / serialising containers: `cyclic` when this container is already
// being printed further up (a = #(); append a a) or nesting is absurdly deep.
struct CycleGuard {
    static std::vector<const void*>& active() { static thread_local std::vector<const void*> a; return a; }
    bool cyclic = false, pushed = false;
    explicit CycleGuard(const void* p) {
        auto& a = active();
        cyclic = a.size() > 200 || std::find(a.begin(), a.end(), p) != a.end();
        if (!cyclic) { a.push_back(p); pushed = true; }
    }
    ~CycleGuard() { if (pushed) active().pop_back(); }
    CycleGuard(const CycleGuard&) = delete;
    CycleGuard& operator=(const CycleGuard&) = delete;
};


// ------------------------------------------------------------------ AST -----
enum class NK : uint8_t {
    Lit, Ident, ArrayLit, BitArrayLit,
    PointLit,          // [x,y] / [x,y,z] / [x,y,z,w] -- operand position only
    ScenePath,         // $foo, $*, $parent/child -- needs GX2 scene bindings
    WithCtx,           // with redraw off ( body ) -- context ignored, body runs
    Rollout,           // rollout name "title" ( items )
    UiItem,            // button b "caption" width:80 align:#left
    OnHandler,         // on <ctl> <event> do <expr>
    Prop, Index, Call,
    Unary, Binary, And, Or, Assign, CompoundAssign, AsCast,
    Block, If, While, DoWhile, ForNum, ForIn, Case,
    FnDef, StructDef, LocalDecl, GlobalDecl,
    Return, Exit, Continue, TryCatch, Throw,
    Scripted,          // macroScript / plugin / tool definition (str = kind)
    When,              // [mxs25 F] when <attr> <objs> change[s] ... do <expr> (name = attr / "deleted")
    MaxCmd,            // [mxsfix 1c] max <command words> (name = words joined by one space)
};

struct Node {
    Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node();   // [auditA] iterative: a very deep tree (long operator chains) must not recurse on destruction
    // rollout / UI (see Parser::rollout_expr)
    std::string str;          // rollout title, control type, or event name
    int on_count = 0, item_count = 0, stmt_count = 0;
    int group_depth = 0;   // open 'group "x" (' blocks inside a rollout body
    NK k;
    int line = 0;
    Value lit;                                   // Lit
    std::string name;                            // Ident / Prop name / op ("+","-","==",...) / fn name / loop var
    std::vector<std::unique_ptr<Node>> kids;     // operands / args / body
    std::vector<std::string> kw_names;           // Call keyword arg names (parallel tail of kids)
    int pos_argc = 0;                            // Call: number of positional args in kids after callee
    // FnDef: kids = [defaults..., body]; params in fn_params, kw params in fn_kwparams
    std::vector<std::string> fn_params;
    std::vector<std::string> fn_kwparams;
    bool flag = false;                           // ForNum: has 'by'; If: has else; Case: has test expr; Exit: has value;
                                                 // FnDef: mapped; GlobalDecl: persistent; Rollout: is a 'parameters' block;
                                                 // WithCtx: has a qualifier
    std::string src;                             // Scripted: exact source text; Scripted plugin: superclass in lit
};
using NodeP = std::unique_ptr<Node>;

// ---------------------------------------------------------------- lexer -----
enum class Tok : uint8_t {
    End, Number, String, NameLit, Ident,
    LParen, RParen, LBracket, RBracket, ArrayOpen /*#(*/, BitArrayOpen /*#{*/, RBrace,
    Comma, Semi, Colon, Dot, DotDot,
    Plus, Minus, Star, Slash, Caret,
    Assign, PlusEq, MinusEq, StarEq, SlashEq,
    Eq, Ne, Lt, Gt, Le, Ge,
    Amp, Query, Dollar,
    // keywords
    KwAnd, KwOr, KwNot, KwIf, KwThen, KwElse, KwDo, KwWhile, KwFor, KwIn, KwTo,
    KwBy, KwOf, KwWhere, KwCollect, KwCase, KwFn, KwLocal, KwGlobal, KwReturn,
    KwExit, KwContinue, KwStruct, KwTry, KwCatch, KwThrow, KwAs, KwWith, KwRollout, KwUtility, KwParameters, KwRcMenu, KwAt, KwAbout, KwAttributes, KwDefault,
    KwTrue, KwFalse, KwOn, KwOff, KwUndefined, KwOk, KwUnsupplied,
};

struct Token {
    Tok t = Tok::End;
    Value v;                 // Number/String/NameLit payload
    std::string text;        // Ident original spelling
    int line = 1;
    bool nl_before = false;  // a newline occurred before this token
    bool sp_before = false;  // whitespace immediately before
    bool sp_after  = false;  // whitespace immediately after (needed for '-' rule)
    size_t off = 0, end_off = 0;   // byte range in the source
};

struct Lexer {
    const char* p; const char* end; int line = 1;
    std::string src_name;
    explicit Lexer(const std::string& s, std::string name)
        : p(s.data()), end(s.data() + s.size()), src_name(std::move(name)), base(s.data()) {}
    const char* base;
    Token next();
private:
    [[noreturn]] void err(const std::string& m) { throw MxsError("Syntax error: " + m + " (" + src_name + ")", line); }
};

struct Parser {
    std::vector<Token> toks; size_t pos = 0;
    std::string src_name;
    std::string source;                  // BOM-stripped text the token offsets refer to
    int stop_by = 0;                     // [mxs25 F] >0 while parsing 'for ... to <expr>': identifier 'by' ends it
    Parser(const std::string& src, std::string name);

    NodeP parse_program();               // top-level expr sequence -> Block
private:
    const Token& cur() const { return toks[pos]; }
    const Token& peek(int n = 1) const { size_t i = pos + n; return toks[i < toks.size() ? i : toks.size() - 1]; }
    bool at(Tok t) const { return cur().t == t; }
    bool accept(Tok t) { if (at(t)) { ++pos; return true; } return false; }
    void expect(Tok t, const char* what);
    [[noreturn]] void err(const std::string& m) { throw MxsError("Syntax error: " + m + " (" + src_name + ")", cur().line); }

    void skip_separators();              // ; and expression-sequence EOLs are implicit via nl_before

    NodeP expr();
    NodeP rollout_expr();
    NodeP condition_expr();
    bool  peek_is_numeric_range() const;
    bool  is_ui_control(const std::string& lw) const;                        // full expression (assignment level)
    NodeP expr_no_assign();
    NodeP or_expr();  NodeP and_expr();  NodeP not_expr();  NodeP cmp_expr();
    NodeP add_expr(); NodeP mul_expr();  NodeP pow_expr();  NodeP unary_expr();
    NodeP as_expr();
    NodeP postfix_expr(bool allow_call); // primary + .prop [idx] + juxtaposition call
    NodeP primary(bool allow_call);
    NodeP operand();                     // call argument: postfix w/o further juxtaposition
    NodeP block_paren();                 // '(' seq ')'
    NodeP if_expr(); NodeP for_expr(); NodeP while_expr(); NodeP do_expr();
    NodeP case_expr(); NodeP fn_expr(); NodeP struct_expr();
    NodeP local_expr(); NodeP global_expr(); NodeP try_expr();
    NodeP context_expr();                // with/at/in/about/animate/undo/coordsys/set contexts
    bool  at_context_start() const;
    NodeP ctx_clause(const std::string& word, int line);   // one clause, body filled by caller
    NodeP scripted_expr(const std::string& kind);
    bool  at_when_start() const;                            // [mxs25 F] change-handler construct
    NodeP when_expr();           // macroScript / plugin / tool
    bool  at_scripted_start(std::string& kind) const;
    bool starts_operand(const Token& tk) const;
    bool arg_minus() const;              // '-' that begins a new negated argument
    static NodeP mk(NK k, int line);
};

// ----------------------------------------------------------------- eval -----
Value eval_node(Node* n, Env& env, Engine& eng);
Value call_value(Value& callee, std::vector<Value>& args,
                 std::map<std::string, Value>& kwargs, Env& env, Engine& eng, int line);

struct FnDef {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> kwparams;
    std::vector<Value> kwdefaults;       // literal defaults (evaluated once)
    std::vector<Node*> kwdefault_exprs;  // non-literal defaults: evaluated per call, positionals in scope (nullptr = literal)
    std::shared_ptr<Node> body;
    Env* closure = nullptr;              // defining scope (globals for top-level fns)
    bool mapped = false;                 // 'mapped fn': auto-maps over a collection first argument
    unsigned deniedCaps = 0;             // [F08] Engine::denied_caps when defined; re-applied on every call
};

struct StructDef_t {
    std::string name;
    std::vector<std::string> members;    // ordered
    std::vector<Value> defaults;         // Undefined if none
    Node* on_create = nullptr;           // 'on create do ...' handler body
    Node* rollout_node = nullptr;        // rollout: its definition (createDialog re-initialises from it)
    bool is_rollout = false;             // prints as Rollout:<name>
};
struct StructInst {
    std::shared_ptr<StructDef_t> def;
    std::map<std::string, Value> slots;
};

struct FileStream_t {
    FILE* f = nullptr;
    std::string path, mode;
    ~FileStream_t() { if (f) fclose(f); }
};
struct StringStream_t { std::string buf; size_t rpos = 0; };

// ---- scripted definitions (scripted.cpp)
struct ScriptedHandler { Node* body = nullptr; std::vector<std::string> params; };
struct ScriptedDef {
    ScriptedInfo info;
    std::map<std::string, ScriptedHandler> handlers;      // event -> body; parameter handlers as "<param>.<event>"
    std::vector<Node*> stmts;                             // body statements (locals, fns, ...) run per instance
    std::vector<Node*> rollouts;                          // rollout definitions (UI description only)
    std::map<std::string, std::shared_ptr<ScriptedDef>> tools;   // nested tools
    Value macroState;                                     // macroScript: persistent locals across runs
    bool macroStateReady = false;
    unsigned deniedCaps = 0;   // [F08] Engine::denied_caps at definition time, re-applied to every handler run
};
Value eval_scripted(Node* n, Env& env, Engine& eng);
extern Engine* g_active_engine;
// Run fn with 'slots' staged as globals; writes back assignments to slots.
Value with_staged_slots(Engine& eng, std::map<std::string, Value>& slots, const std::function<Value()>& fn);

void install_core(Engine& e, Env& g);
void install_file(Engine& e, Env& g);
void install_math3d(Engine& e, Env& g);
void install_ext(Engine& e, Env& g);
void install_scripted(Engine& e, Env& g); // scripted.cpp      // builtins_ext.cpp
FILE* mxs_script_log();                   // openLog() target, or nullptr

void install_lang2(Engine& e, Env& g);   // builtins_lang2.cpp: Max 8 language backlog

// ---- values2.cpp: rotation conventions -------------------------------------
// MXS quats are MAX's LEFT-handed ones: (eulerAngles 0 0 90) as quat -->
// (quat 0 0 -0.707107 0.707107). Rotation matrices are row-vector (p * M) and
// eulerAngles x y z == rotateXMatrix x * rotateYMatrix y * rotateZMatrix z.
void  rot_from_euler(double x, double y, double z, Mat3& out);         // degrees
void  rot_to_euler(const Mat3& m, double& x, double& y, double& z);    // degrees
void  rot_from_quat(const Vec4& q, Mat3& out);                         // MXS quat
Vec4  rot_to_quat(const Mat3& m);                                      // MXS quat
Vec4  quat_from_angleaxis(double deg, double ax, double ay, double az);
void  quat_to_angleaxis(const Vec4& q, double& deg, double& ax, double& ay, double& az);
// Value -> rotation matrix for Quat / EulerAngles / AngleAxis / Matrix3; false otherwise
bool  rotation_of(const Value& v, Mat3& out);
std::string ext_to_string(const Value& v);      // Time/Interval/EulerAngles/AngleAxis/Dictionary/Reference
std::string time_str(double frames);            // 10f / 2.5f
Value deref(const Value& v);                    // Reference -> its value
// Property access with the evaluator's full rules (host hooks, value types).
Value value_get_prop(const Value& v, const std::string& lname);
void  value_set_prop(Value& v, const std::string& lname, const Value& nv);

// Native out-parameter plumbing (set_out_arg): one frame per native call that
// received '&' arguments.
struct OutArgFrame {
    std::vector<std::shared_ptr<RefBox_t>> pos;          // nullptr where not '&'
    std::map<std::string, std::shared_ptr<RefBox_t>> kw;
};
extern thread_local OutArgFrame* g_out_frame;

// format "%..." helper shared by print/format
std::string mxs_format(const std::string& fmt, const std::vector<Value>& args);

} // namespace mxs
