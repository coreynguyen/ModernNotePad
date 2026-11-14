#include "mxs_internal.h"
#include <chrono>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace mxs {

// ================================================================ values ====
std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::unordered_map<std::string, std::string>& name_table() {
    static std::unordered_map<std::string, std::string> t; return t;
}
const std::string* intern(const std::string& s) {
    auto& t = name_table();
    auto it = t.find(lower(s));
    if (it == t.end()) it = t.emplace(lower(s), s).first;    // keep first spelling
    return &it->second;
}

Value Value::S(std::string x)  { Value v; v.t = Type::String; v.s = std::make_shared<std::string>(std::move(x)); return v; }
Value Value::N(const std::string& x) { Value v; v.t = Type::Name; v.name = intern(x); return v; }
Value Value::A(Array_t x)      { Value v; v.t = Type::Array; v.arr = std::make_shared<Array_t>(std::move(x)); return v; }
static Value vecval(Type t, float x, float y, float z, float w) {
    Value v; v.t = t; v.vec = std::make_shared<Vec4>(); v.vec->x = x; v.vec->y = y; v.vec->z = z; v.vec->w = w; return v;
}

Value Value::M3() { Value v; v.t = Type::Matrix3; v.mat = std::make_shared<Mat3>(); return v; }
// Class references get their own table: a class prints with its documented
// spelling (EulerAngles) even when a lowercase builtin or #name of the same
// word was interned first. A spelling with capitals replaces an all-lowercase
// one; the pointer stays stable, so class equality is still a pointer compare.
static const std::string* intern_class(const std::string& x) {
    static std::unordered_map<std::string, std::unique_ptr<std::string>> t;
    auto& slot = t[lower(x)];
    if (!slot) slot = std::make_unique<std::string>(x);
    else if (*slot == lower(*slot) && x != lower(x)) *slot = x;
    return slot.get();
}
Value Value::C(const std::string& x) { Value v; v.t = Type::Class; v.name = intern_class(x); return v; }
Value Value::P2(float x, float y)                    { return vecval(Type::Point2, x, y, 0, 0); }
Value Value::P3(float x, float y, float z)           { return vecval(Type::Point3, x, y, z, 0); }
Value Value::P4(float x, float y, float z, float w)  { return vecval(Type::Point4, x, y, z, w); }
Value Value::QuatV(float x, float y, float z, float w){ return vecval(Type::Quat, x, y, z, w); }
Value Value::ColorV(float r, float g, float b, float a){ return vecval(Type::Color, r, g, b, a); }

bool Value::truthy() const {
    switch (t) {
        case Type::Bool: return b;
        case Type::Undefined: return false;                      // MXS: 'if undefined then' errors; be permissive
        default:
            throw MxsError("Type error: boolean expected, got " + to_display());
    }
}

// Component formatting inside point2/3/4, quat, colour and matrix3 rows.
// Max 8 drops the ".0" there even though a bare Float keeps it:
//     5.0 as string        --> "5.0"
//     point3 1 2 3         --> [1,2,3]
//     matrix3 1            --> (matrix3 [1,0,0] [0,1,0] [0,0,1] [0,0,0])
static std::string vnum(double d) {
    if (std::abs(d) < 1e15 && d == (int64_t)d) {   // [auditA] range first: the cast is UB out of range
        char buf[40]; snprintf(buf, sizeof buf, "%lld", (long long)d); return buf;
    }
    char buf[40]; snprintf(buf, sizeof buf, "%g", d); return buf;
}

static std::string fnum(double d) {
    // MXS float printing: up to 6 significant-ish, trailing ".0" for whole floats
    if (std::abs(d) < 1e15 && d == (int64_t)d) {   // [auditA] range first: the cast is UB out of range
        char buf[40]; snprintf(buf, sizeof buf, "%.1f", d); return buf;
    }
    char buf[40]; snprintf(buf, sizeof buf, "%g", d); return buf;
}

std::string Value::to_string() const {
    switch (t) {
        case Type::Undefined: return "undefined";
        case Type::Ok:        return "OK";
        case Type::Unsupplied:return "unsupplied";
        case Type::Bool:      return b ? "true" : "false";
        case Type::Int:       return std::to_string(i);
        case Type::Float:     return fnum(f);
        case Type::String:    return *s;
        case Type::Name:      return *name;
        case Type::Array: {
            CycleGuard cg(arr.get());   // [auditA] an array holding itself recursed until the stack overflowed
            if (cg.cyclic) return "#(...)";
            std::string out = "#(";
            for (size_t k = 0; k < arr->size(); ++k) { if (k) out += ", "; out += (*arr)[k].to_display(); }
            return out + ")"; }
        case Type::BitArray: {
            std::string out = "#{"; bool first = true;
            size_t k = 0, n = bits->size();
            while (k < n) {
                if (!(*bits)[k]) { ++k; continue; }
                size_t j = k; while (j + 1 < n && (*bits)[j + 1]) ++j;
                if (!first) out += ", ";
                first = false;
                out += std::to_string(k + 1);
                if (j > k) out += ".." + std::to_string(j + 1);
                k = j + 1;
            }
            return out + "}"; }
        case Type::Point2: return "[" + vnum(vec->x) + "," + vnum(vec->y) + "]";
        case Type::Point3: return "[" + vnum(vec->x) + "," + vnum(vec->y) + "," + vnum(vec->z) + "]";
        case Type::Point4: return "[" + vnum(vec->x) + "," + vnum(vec->y) + "," + vnum(vec->z) + "," + vnum(vec->w) + "]";
        case Type::Quat:   return "(quat " + vnum(vec->x) + " " + vnum(vec->y) + " " + vnum(vec->z) + " " + vnum(vec->w) + ")";
        case Type::Color:  return "(color " + vnum(vec->x) + " " + vnum(vec->y) + " " + vnum(vec->z) + ")";
        case Type::Matrix3: {
            auto& m = *mat;
            auto row = [&](int r) { return "[" + vnum(m.m[r][0]) + "," + vnum(m.m[r][1]) + "," + vnum(m.m[r][2]) + "]"; };
            return "(matrix3 " + row(0) + " " + row(1) + " " + row(2) + " " + row(3) + ")"; }
        case Type::FileStream:   return "<File:" + fs->path + ">";
        case Type::StringStream: return ss->buf;
        case Type::Function:     return fn->name.empty() ? "<fn>" : (fn->name + "()");
        case Type::NativeFn:     return std::string(nat_name ? nat_name : "<native>") + "()";
        case Type::StructDef:    return "#Struct:" + sdef->name;
        case Type::Struct: {
            if (auto& h = Engine::display_hook()) { std::string o; if (h(*this, o)) return o; }
            if (sinst->def->is_rollout) return "Rollout:" + sinst->def->name;
            // Listener form of a struct instance: (person name:"Bill" age:42).
            // Data members in declaration order; member functions are left out.
            CycleGuard cg(sinst.get());   // [auditA] self-referencing instances
            if (cg.cyclic) return "(" + sinst->def->name + " ...)";
            std::string o = "(" + sinst->def->name;
            if (!sinst->def->members.empty()) {
                for (auto& m : sinst->def->members) {
                    auto it = sinst->slots.find(m);
                    if (it == sinst->slots.end()) continue;
                    if (it->second.t == Type::Function || it->second.t == Type::NativeFn) continue;
                    o += " " + m + ":" + it->second.to_display();
                }
                return o + ")";
            }
            return o + " ...)"; }
        case Type::Class:        return *name;
        case Type::DontCollect:  return "dontCollect";
        case Type::Time: case Type::Interval: case Type::EulerAngles:
        case Type::AngleAxis: case Type::Dictionary: case Type::Reference:
            return ext_to_string(*this);
    }
    return "?";
}
std::string Value::to_display() const {
    if (t == Type::String) { return "\"" + *s + "\""; }
    if (t == Type::Name)   { return "#" + *name; }
    // Listener form is StringStream:"contents" (confirmed by the documented
    // example 'ss = stringstream "ABCDE" --> StringStream:"ABCDE"').
    // to_string() stays the raw contents, which is what 'as string' must give.
    if (t == Type::StringStream) { return "StringStream:\"" + ss->buf + "\""; }
    // Type::Class deliberately falls through to to_string(), which returns
    // the bare name with no '#' -- that IS the display form for a class ref.
    return to_string();
}

bool Value::eq(const Value& o) const {
    if (is_num() && o.is_num()) return num() == o.num();
    // 'classOf bill == person' -- a struct instance's class is a Class value
    // carrying the struct's name, while the definition is a StructDef. They
    // are different Types but must compare equal, or every
    //     if classOf x == MyStruct
    // test silently fails.
    if (t == Type::Class && o.t == Type::StructDef && o.sdef)
        return lower(*name) == lower(o.sdef->name);
    if (t == Type::StructDef && o.t == Type::Class && sdef)
        return lower(sdef->name) == lower(*o.name);
    if (t == Type::Reference || o.t == Type::Reference) return deref(*this).eq(deref(o));
    // 'classOf obj == Box': a class reference equals the constructor of that class
    auto ctorName = [](const Value& f) { return std::string(f.nat_name ? f.nat_name : ""); };
    if (t == Type::Class && o.t == Type::NativeFn) return lower(*name) == lower(ctorName(o));
    if (t == Type::NativeFn && o.t == Type::Class) return lower(ctorName(*this)) == lower(*o.name);
    if (t != o.t) return false;
    switch (t) {
        case Type::Undefined: case Type::Ok: case Type::Unsupplied: return true;
        case Type::Bool:   return b == o.b;
        // Case SENSITIVE -- verified against Max 8: "ABC" == "abc" --> false.
        // Do not confuse with findString, which IS case-insensitive; MAXScript
        // uses different rules for comparison and for search.
        case Type::String: return *s == *o.s;
        case Type::Name:   return name == o.name;
        case Type::Class:  return name == o.name;   // same interned-pointer compare as Name
        case Type::Array:  return arr == o.arr;                   // identity
        case Type::BitArray: {                                    // by content (set bits)
            const auto& A = *bits; const auto& B = *o.bits;
            for (size_t k = 0, n = std::max(A.size(), B.size()); k < n; ++k)
                if ((k < A.size() && A[k]) != (k < B.size() && B[k])) return false;
            return true;
        }
        case Type::Point2: case Type::Point3: case Type::Point4: case Type::Quat: case Type::Color:
        case Type::Interval: case Type::EulerAngles: case Type::AngleAxis:
            return vec->x == o.vec->x && vec->y == o.vec->y && vec->z == o.vec->z && vec->w == o.vec->w;
        case Type::Dictionary: return dict == o.dict;          // identity, as in Max
        case Type::Matrix3: {
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) if (mat->m[r][c] != o.mat->m[r][c]) return false;
            return true; }
        case Type::StructDef: return sdef == o.sdef;
        case Type::Function: return fn == o.fn;
        case Type::NativeFn: return nat == o.nat;
        case Type::Struct:
            if (sinst != o.sinst && sinst && o.sinst)
                if (auto& h = Engine::identity_hook()) { int r = h(*this, o); if (r >= 0) return r == 1; }
            return sinst == o.sinst;
        default: return this == &o || (arr == o.arr && s == o.s && fs == o.fs && sinst == o.sinst);
    }
}

// ================================================================== env =====
Env* Env::global() { Env* e = this; while (e->parent) e = e->parent; return e; }
Value* Env::find(const std::string& lname) {
    for (Env* e = this; e; e = e->parent) {
        auto it = e->vars.find(lname);
        if (it != e->vars.end()) return &it->second;
    }
    return nullptr;
}

// ================================================================= eval =====
[[noreturn]] static void rterr(const std::string& m, int line) { throw MxsError(m, line); }

static Value arith(const std::string& op, const Value& a0, const Value& b0, int line) {
    const Value& a = a0.t == Type::Reference ? a0.ref->v : a0;
    const Value& b = b0.t == Type::Reference ? b0.ref->v : b0;
    // Time arithmetic: a number mixed with a Time counts as frames and the
    // result stays a Time ('sliderTime + 1' --> 11f); Time / Time is a ratio.
    if ((a.t == Type::Time || b.t == Type::Time) && a.is_num() && b.is_num()) {
        const double x = a.num(), y = b.num();
        if (op == "+") return Value::T(x + y);
        if (op == "-") return Value::T(x - y);
        if (op == "*") return Value::T(x * y);
        if (op == "/") {
            if (y == 0) rterr("Division by zero", line);
            if (a.t == Type::Time && b.t == Type::Time) return Value::F(x / y);
            return Value::T(x / y);
        }
        if (op == "^") return Value::F(std::pow(x, y));
    }
    // Interval +/- number shifts both ends
    if (a.t == Type::Interval && b.is_num() && (op == "+" || op == "-")) {
        const double d = op == "+" ? b.num() : -b.num();
        return Value::Ival(a.vec->x + d, a.vec->y + d);
    }
    // numbers
    if (a.is_num() && b.is_num()) {
        // '^' on two Integers yields an Integer in MAXScript (Max 8: 2^10 --> 1024,
        // not 1024.0). Only '/' forces the float path here; int/int division is
        // handled specially below.
        bool as_int = a.t == Type::Int && b.t == Type::Int && op != "/";
        if (as_int && (op == "+" || op == "-" || op == "*")) {
            // [auditA] exact 32-bit wrap in unsigned arithmetic: the double path lost low bits past 2^53
            // (2147483647 * 2147483647) and converting an out-of-range double to int64 is UB.
            const uint32_t ua = (uint32_t)(uint64_t)a.i, ub = (uint32_t)(uint64_t)b.i;
            const uint32_t ur = op == "+" ? ua + ub : op == "-" ? ua - ub : ua * ub;
            return Value::I((int64_t)(int32_t)ur);
        }
        double x = a.num(), y = b.num(), r = 0;
        if      (op == "+") r = x + y;
        else if (op == "-") r = x - y;
        else if (op == "*") r = x * y;
        else if (op == "/") {
            if (a.t == Type::Int && b.t == Type::Int) {            // MXS: int/int is integer division
                if (b.i == 0) rterr("Division by zero", line);
                if (b.i == -1) return Value::I((int64_t)(int32_t)(0u - (uint32_t)(uint64_t)a.i));   // [auditA] INT_MIN / -1 wraps (no UB)
                return Value::I((int64_t)(int32_t)(uint32_t)(uint64_t)(a.i / b.i));
            }
            if (y == 0) rterr("Division by zero", line);
            r = x / y;
        }
        else if (op == "^") r = std::pow(x, y);
        if (!as_int) return Value::F(r);
        // MAXScript Integers are 32-bit and WRAP; verified against Max 8:
        //     2147483647 + 1 --> -2147483648
        //     2147483647 * 2 --> -2
        // We hold int64 internally (it makes unsigned binary reads easier), so
        // truncate to 32 bits here to reproduce the documented arithmetic.
        // [auditA] (only '^' reaches here) out-of-range / non-finite doubles must not be cast directly (UB)
        if (!std::isfinite(r)) return Value::I(0);
        r = std::fmod(std::trunc(r), 4294967296.0);
        if (r < 0) r += 4294967296.0;
        return Value::I((int64_t)(int32_t)(uint32_t)r);
    }
    // strings
    if (a.t == Type::String && op == "+") {
        if (b.t != Type::String) rterr("Cannot '+' String with " + b.to_display(), line);
        return Value::S(*a.s + *b.s);
    }
    // bitarrays: + union, - difference, * intersection (documented MXS BitArray
    // operators). Result count: union/intersection take the larger count,
    // difference keeps the left operand's count.
    if (a.t == Type::BitArray && b.t == Type::BitArray && (op == "+" || op == "-" || op == "*")) {
        const auto& A = *a.bits; const auto& B = *b.bits;
        size_t n = op == "-" ? A.size() : std::max(A.size(), B.size());
        Value out; out.t = Type::BitArray; out.bits = std::make_shared<std::vector<bool>>(n, false);
        for (size_t k = 0; k < n; ++k) {
            bool x = k < A.size() && A[k], y = k < B.size() && B[k];
            (*out.bits)[k] = op == "+" ? (x || y) : op == "-" ? (x && !y) : (x && y);
        }
        return out;
    }
    // arrays
    if (a.t == Type::Array && b.t == Type::Array && op == "+") {
        Array_t out = *a.arr; out.insert(out.end(), b.arr->begin(), b.arr->end());
        return Value::A(std::move(out));
    }
    // vectors (Point2/3/4, Color): elementwise with vector, scale with number
    auto isvec = [](const Value& v) { return v.t == Type::Point2 || v.t == Type::Point3 || v.t == Type::Point4 || v.t == Type::Color; };
    if (isvec(a) && isvec(b) && a.t == b.t) {
        Value out = a; out.vec = std::make_shared<Vec4>(*a.vec);
        float* o = &out.vec->x; const float* q = &b.vec->x;
        for (int k = 0; k < 4; ++k) {
            if      (op == "+") o[k] += q[k];
            else if (op == "-") o[k] -= q[k];
            else if (op == "*") o[k] *= q[k];
            else if (op == "/") o[k] = q[k] ? o[k] / q[k] : 0;
            else rterr("bad vector op '" + op + "'", line);
        }
        return out;
    }
    if (isvec(a) && b.is_num()) {
        Value out = a; out.vec = std::make_shared<Vec4>(*a.vec);
        float s = (float)b.num(); float* o = &out.vec->x;
        for (int k = 0; k < 4; ++k) {
            if      (op == "*") o[k] *= s;
            else if (op == "/") o[k] /= s;
            else if (op == "+") o[k] += s;
            else if (op == "-") o[k] -= s;
            else rterr("bad vector op", line);
        }
        return out;
    }
    // number * vector (scale), number + vector
    if (a.is_num() && isvec(b) && (op == "*" || op == "+")) return arith(op, b, a, line);
    // point3 * matrix3  (transform)
    if (a.t == Type::Point3 && b.t == Type::Matrix3 && op == "*") {
        auto& m = b.mat->m; float x = a.vec->x, y = a.vec->y, z = a.vec->z;
        return Value::P3(x*m[0][0] + y*m[1][0] + z*m[2][0] + m[3][0],
                         x*m[0][1] + y*m[1][1] + z*m[2][1] + m[3][1],
                         x*m[0][2] + y*m[1][2] + z*m[2][2] + m[3][2]);
    }
    // matrix3 * matrix3
    if (a.t == Type::Matrix3 && b.t == Type::Matrix3 && op == "*") {
        Value out; out.t = Type::Matrix3; out.mat = std::make_shared<Mat3>();
        auto& A = a.mat->m; auto& B = b.mat->m; auto& O = out.mat->m;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) {
                O[r][c] = A[r][0]*B[0][c] + A[r][1]*B[1][c] + A[r][2]*B[2][c] + (r == 3 ? B[3][c] : 0.f);
            }
        return out;
    }
    // point3 * quat / eulerAngles / angleAxis: rotate the point
    if (a.t == Type::Point3 && op == "*" && (b.t == Type::Quat || b.t == Type::EulerAngles || b.t == Type::AngleAxis)) {
        Mat3 m; rotation_of(b, m);
        float x = a.vec->x, y = a.vec->y, z = a.vec->z;
        return Value::P3(x*m.m[0][0] + y*m.m[1][0] + z*m.m[2][0],
                         x*m.m[0][1] + y*m.m[1][1] + z*m.m[2][1],
                         x*m.m[0][2] + y*m.m[1][2] + z*m.m[2][2]);
    }
    // quat +/- quat (component-wise), quat * number
    if (a.t == Type::Quat && b.t == Type::Quat && (op == "+" || op == "-")) {
        float s = op == "+" ? 1.f : -1.f;
        return Value::QuatV(a.vec->x + s*b.vec->x, a.vec->y + s*b.vec->y, a.vec->z + s*b.vec->z, a.vec->w + s*b.vec->w);
    }
    if (a.t == Type::Quat && b.is_num() && (op == "*" || op == "/")) {
        float k = (float)(op == "*" ? b.num() : 1.0 / b.num());
        return Value::QuatV(a.vec->x*k, a.vec->y*k, a.vec->z*k, a.vec->w*k);
    }
    // eulerAngles +/- eulerAngles (component-wise degrees)
    if (a.t == Type::EulerAngles && b.t == Type::EulerAngles && (op == "+" || op == "-")) {
        float s = op == "+" ? 1.f : -1.f;
        return Value::Euler(a.vec->x + s*b.vec->x, a.vec->y + s*b.vec->y, a.vec->z + s*b.vec->z);
    }
    // quat * quat (composition, MXS order: q1*q2 applies q1 then q2)
    if (a.t == Type::Quat && b.t == Type::Quat && op == "*") {
        float ax = a.vec->x, ay = a.vec->y, az = a.vec->z, aw = a.vec->w;
        float bx = b.vec->x, by = b.vec->y, bz = b.vec->z, bw = b.vec->w;
        return Value::QuatV(bw*ax + bx*aw + by*az - bz*ay,
                            bw*ay - bx*az + by*aw + bz*ax,
                            bw*az + bx*ay - by*ax + bz*aw,
                            bw*aw - bx*ax - by*ay - bz*az);
    }
    rterr("Cannot apply '" + op + "' to " + a.to_display() + " and " + b.to_display(), line);
}

static int cmp_vals(const Value& a0, const Value& b0, int line) {
    const Value& a = a0.t == Type::Reference ? a0.ref->v : a0;
    const Value& b = b0.t == Type::Reference ? b0.ref->v : b0;
    if (a.is_num() && b.is_num()) { double x = a.num(), y = b.num(); return x < y ? -1 : x > y ? 1 : 0; }
    if (a.t == Type::String && b.t == Type::String) {
        std::string x = lower(*a.s), y = lower(*b.s);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (a.t == Type::Name && b.t == Type::Name) {
        std::string x = lower(*a.name), y = lower(*b.name);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    rterr("Cannot compare " + a.to_display() + " with " + b.to_display(), line);
}

// -------- property access -------------------------------------------------
// The active engine, so property hooks can be reached from the free-standing
// get_prop/set_prop helpers without threading it through every call site.
Engine* g_active_engine = nullptr;

static Value get_prop(const Value& v, const std::string& p, int line) {
    // 'objects.count' -- MAXScript collections exposed here as functions must
    // answer property access too, not just iteration. Calling with no args is
    // safe: these are the zero-arg collection accessors (objects, selection,
    // geometry). Anything else falls through unchanged.
    if ((v.t == Type::NativeFn || v.t == Type::Function) && g_active_engine) {
        static const char* collProps[] = { "count", nullptr };
        for (const char** cp = collProps; *cp; ++cp)
            if (p == *cp) {
                std::vector<Value> noargs;
                std::map<std::string, Value> nokw;
                Value self = v;
                Env tmp; tmp.parent = g_active_engine->global_env_ptr();
                Value got = call_value(self, noargs, nokw, tmp, *g_active_engine, line);
                if (got.t != Type::NativeFn && got.t != Type::Function)
                    return get_prop(got, p, line);
            }
    }
    // Host objects (scene nodes, materials) get first refusal, so
    // 'node.transform' reads the real scene rather than a stale slot.
    if (g_active_engine && g_active_engine->prop_get_hook) {
        Value out;
        if (g_active_engine->prop_get_hook(v, p, out)) return out;
    }

    switch (v.t) {
        case Type::Reference: return get_prop(v.ref->v, p, line);
        case Type::Time:
            if (p == "frame") return Value::F(v.f);
            if (p == "ticks") return Value::I((int64_t)std::llround(v.f * 160.0));
            if (p == "normalized") return Value::F(v.f / 100.0);   // SPEC-GAP: against the default 0..100 range
            break;
        case Type::Interval:
            if (p == "start") return Value::T(v.vec->x);
            if (p == "end")   return Value::T(v.vec->y);
            break;
        case Type::EulerAngles:
            if (p == "x" || p == "x_rotation") return Value::F(v.vec->x);
            if (p == "y" || p == "y_rotation") return Value::F(v.vec->y);
            if (p == "z" || p == "z_rotation") return Value::F(v.vec->z);
            break;
        case Type::AngleAxis:
            if (p == "angle") return Value::F(v.vec->w);
            if (p == "axis")  return Value::P3(v.vec->x, v.vec->y, v.vec->z);
            if (p == "numrevs") return Value::I((int64_t)(v.vec->w / 360.0));
            // compatibility: components of the axis
            if (p == "x") return Value::F(v.vec->x);
            if (p == "y") return Value::F(v.vec->y);
            if (p == "z") return Value::F(v.vec->z);
            break;
        case Type::Dictionary:
            if (p == "count") return Value::I((int64_t)v.dict->items.size());
            if (p == "keys") { Array_t o; for (auto& kv : v.dict->items) o.push_back(kv.first); return Value::A(std::move(o)); }
            if (p == "type") return Value::N(v.dict->keyType);
            break;
        case Type::StringStream:
            if (p == "count") return Value::I((int64_t)v.ss->buf.size());
            if (p == "position") return Value::I((int64_t)v.ss->rpos);
            break;
        case Type::StructDef:
            if (p == "name") return Value::S(v.sdef->name);
            break;
        case Type::Array:    if (p == "count") return Value::I((int64_t)v.arr->size()); break;
        case Type::String:   if (p == "count") return Value::I((int64_t)v.s->size()); break;
        case Type::BitArray:
            if (p == "count") return Value::I((int64_t)v.bits->size());
            if (p == "numberset") { int64_t n = 0; for (bool b : *v.bits) n += b; return Value::I(n); }
            if (p == "isempty") { for (bool b : *v.bits) if (b) return Value::B(false); return Value::B(true); }
            break;
        case Type::Point2: case Type::Point3: case Type::Point4: case Type::Quat:
            if (p == "x") return Value::F(v.vec->x);
            if (p == "y") return Value::F(v.vec->y);
            if (p == "z" && v.t != Type::Point2) return Value::F(v.vec->z);
            if (p == "w" && (v.t == Type::Point4 || v.t == Type::Quat)) return Value::F(v.vec->w);
            if (v.t == Type::Quat && (p == "angle" || p == "axis")) {
                double d, ax, ay, az; quat_to_angleaxis(*v.vec, d, ax, ay, az);
                return p == "angle" ? Value::F(d) : Value::P3((float)ax, (float)ay, (float)az);
            }
            break;
        case Type::Color:
            if (p == "r" || p == "red")   return Value::F(v.vec->x);
            if (p == "g" || p == "green") return Value::F(v.vec->y);
            if (p == "b" || p == "blue")  return Value::F(v.vec->z);
            if (p == "a" || p == "alpha") return Value::F(v.vec->w);
            if (p == "h" || p == "hue" || p == "s" || p == "saturation" || p == "v" || p == "value") {
                // MAX's HSV is 0..255 on every channel
                double r = v.vec->x / 255.0, g = v.vec->y / 255.0, b = v.vec->z / 255.0;
                double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
                if (p == "v" || p == "value") return Value::F(mx * 255.0);
                if (p == "s" || p == "saturation") return Value::F(mx > 0 ? d / mx * 255.0 : 0.0);
                double h = 0;
                if (d > 0) {
                    if (mx == r) h = std::fmod((g - b) / d, 6.0);
                    else if (mx == g) h = (b - r) / d + 2;
                    else h = (r - g) / d + 4;
                    h /= 6.0; if (h < 0) h += 1;
                }
                return Value::F(h * 255.0);
            }
            break;
        case Type::Matrix3: {
            auto row = [&](int r) { return Value::P3(v.mat->m[r][0], v.mat->m[r][1], v.mat->m[r][2]); };
            if (p == "row1") return row(0);
            if (p == "row2") return row(1);
            if (p == "row3") return row(2);
            if (p == "row4" || p == "translation" || p == "pos" || p == "translationpart") return row(3);
            if (p == "rotationpart") {
                Mat3 m = *v.mat;
                for (int r = 0; r < 3; ++r) {
                    double l = std::sqrt((double)m.m[r][0]*m.m[r][0] + (double)m.m[r][1]*m.m[r][1] + (double)m.m[r][2]*m.m[r][2]);
                    if (l > 1e-12) for (int c = 0; c < 3; ++c) m.m[r][c] = (float)(m.m[r][c] / l);
                }
                Vec4 q = rot_to_quat(m);
                return Value::QuatV(q.x, q.y, q.z, q.w);
            }
            if (p == "scalepart") {
                auto len = [&](int r) { return (float)std::sqrt((double)v.mat->m[r][0]*v.mat->m[r][0] + (double)v.mat->m[r][1]*v.mat->m[r][1] + (double)v.mat->m[r][2]*v.mat->m[r][2]); };
                return Value::P3(len(0), len(1), len(2));
            }
            if (p == "determinantsign") {
                const auto& M = v.mat->m;
                double d = (double)M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - (double)M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + (double)M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
                return Value::I(d < 0 ? -1 : 1);
            }
            if (p == "isidentity") {
                Mat3 id; for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) if (std::fabs(v.mat->m[r][c] - id.m[r][c]) > 1e-6f) return Value::B(false);
                return Value::B(true);
            }
            break; }
        case Type::Struct: {
            auto it = v.sinst->slots.find(p);
            if (it != v.sinst->slots.end()) return it->second;
            rterr("Unknown property '" + p + "' in struct " + v.sinst->def->name, line); }
        default: break;
    }
    rterr("Unknown property: '" + p + "' in " + v.to_display(), line);
}

static void set_prop(Value& v, const std::string& p, const Value& nv, int line) {
    if (g_active_engine && g_active_engine->prop_set_hook &&
        g_active_engine->prop_set_hook(v, p, nv)) return;
    auto f = [&]() -> float { if (!nv.is_num()) rterr("number required", line); return (float)nv.num(); };
    switch (v.t) {
        case Type::Reference: set_prop(v.ref->v, p, nv, line); v.ref->dirty = true; return;
        case Type::Interval:
            if (p == "start") { v.vec->x = f(); return; }
            if (p == "end")   { v.vec->y = f(); return; }
            break;
        case Type::EulerAngles:
            if (p == "x" || p == "x_rotation") { v.vec->x = f(); return; }
            if (p == "y" || p == "y_rotation") { v.vec->y = f(); return; }
            if (p == "z" || p == "z_rotation") { v.vec->z = f(); return; }
            break;
        case Type::AngleAxis:
            if (p == "angle") { v.vec->w = f(); return; }
            if (p == "axis" && nv.vec) { v.vec->x = nv.vec->x; v.vec->y = nv.vec->y; v.vec->z = nv.vec->z; return; }
            break;
        case Type::Point2: case Type::Point3: case Type::Point4: case Type::Quat:
            if (p == "x") { v.vec->x = f(); return; }
            if (p == "y") { v.vec->y = f(); return; }
            if (p == "z" && v.t != Type::Point2) { v.vec->z = f(); return; }
            if (p == "w" && (v.t == Type::Point4 || v.t == Type::Quat)) { v.vec->w = f(); return; }
            break;
        case Type::Color:
            if (p == "r" || p == "red")   { v.vec->x = f(); return; }
            if (p == "g" || p == "green") { v.vec->y = f(); return; }
            if (p == "b" || p == "blue")  { v.vec->z = f(); return; }
            if (p == "a" || p == "alpha") { v.vec->w = f(); return; }
            break;
        case Type::Matrix3: {
            if (nv.t != Type::Point3) rterr("point3 required", line);
            int r = p == "row1" ? 0 : p == "row2" ? 1 : p == "row3" ? 2 :
                    (p == "row4" || p == "translation" || p == "pos") ? 3 : -1;
            if (r >= 0) { v.mat->m[r][0] = nv.vec->x; v.mat->m[r][1] = nv.vec->y; v.mat->m[r][2] = nv.vec->z; return; }
            break; }
        case Type::Struct: v.sinst->slots[p] = nv; return;
        case Type::Array:
            if (p == "count" && nv.is_num() && nv.inum() >= 0) {
                if (nv.inum() > (int64_t)1 << 26) rterr("array count too large", line);   // [auditA]
                v.arr->resize((size_t)nv.inum()); return; }
            break;
        case Type::BitArray:
            if (p == "count" && nv.is_num() && nv.inum() >= 0) {
                if (nv.inum() > (int64_t)1 << 30) rterr("BitArray count too large", line);   // [auditA]
                v.bits->resize((size_t)nv.inum(), false); return; }
            break;
        default: break;
    }
    rterr("Cannot set property '" + p + "' on " + v.to_display(), line);
}

Value value_get_prop(const Value& v, const std::string& lname) { return get_prop(v, lname, 0); }
void  value_set_prop(Value& v, const std::string& lname, const Value& nv) { set_prop(v, lname, nv, 0); }

static Value get_index(const Value& v0, const Value& iv0, int line) {
    const Value& v = v0.t == Type::Reference ? v0.ref->v : v0;
    const Value& iv = iv0.t == Type::Reference ? iv0.ref->v : iv0;
    if (v.t == Type::Dictionary) {
        int k = v.dict->find(iv);
        return k < 0 ? Value::undef() : v.dict->items[(size_t)k].second;
    }
    // host collections by name, and host objects (Struct) by number: Multi/Sub-Object 'mat[id]' ...
    if ((iv.t == Type::Name || iv.t == Type::String || (v.t == Type::Struct && iv.is_num())) && g_active_engine &&
        g_active_engine->index_get_hook) {
        Value out;
        if (g_active_engine->index_get_hook(v, iv, out)) return out;
    }
    if (!iv.is_num()) rterr("index must be a number", line);
    int64_t i = iv.inum();
    switch (v.t) {
        case Type::Array:
            // Observed MAXScript behaviour (stricter than a plain bounds check):
            //     a[0]  -> Runtime error: array index must be +ve number
            //     a[-1] -> Runtime error: same
            //     a[99] -> undefined
            // The bounds check in the indexing operator runs BEFORE Array::get,
            // so a non-positive index errors and only past-the-end reads give
            // undefined.
            if (i < 1) rterr("array index must be +ve number, got: " + std::to_string(i), line);
            if ((size_t)i > v.arr->size()) return Value::undef();
            return (*v.arr)[(size_t)(i - 1)];
        case Type::String:
            if (i < 1 || (size_t)i > v.s->size()) rterr("String index out of range: " + std::to_string(i), line);
            return Value::S(std::string(1, (*v.s)[(size_t)(i - 1)]));
        case Type::Struct: {
            // 'mat[i]' on a MultiMaterial addresses its SUB-MATERIALS. Host
            // objects expose the backing array in a conventional slot, so the
            // engine can index them without knowing what a material is.
            if (!v.sinst) break;
            // host collections (key arrays ...) keep their items in '__items'
            auto hi = v.sinst->slots.find("__items");
            if (hi != v.sinst->slots.end() && hi->second.t == Type::Array) {
                if (i < 1) rterr("index must be +ve number, got: " + std::to_string(i), line);
                if ((size_t)i > hi->second.arr->size()) return Value::undef();
                return (*hi->second.arr)[(size_t)(i - 1)];
            }
            auto it = v.sinst->slots.find("materiallist");
            if (it == v.sinst->slots.end() || it->second.t != Type::Array) break;
            if (i < 1) rterr("index must be +ve number, got: " + std::to_string(i), line);
            if ((size_t)i > it->second.arr->size()) return Value::undef();
            return (*it->second.arr)[(size_t)(i - 1)];
        }
        case Type::BitArray:
            if (i < 1) rterr("BitArray index out of range", line);
            return Value::B((size_t)i <= v.bits->size() && (*v.bits)[(size_t)(i - 1)]);
        default: break;
    }
    rterr("Cannot index " + v.to_display(), line);   // [[noreturn]]; also reached from Struct's break
}
static void set_index(Value& v, const Value& iv, const Value& nv, int line) {
    if (v.t == Type::Reference) { set_index(v.ref->v, iv, nv, line); v.ref->dirty = true; return; }
    if (v.t == Type::Dictionary) {
        int k = v.dict->find(iv);
        if (k < 0) v.dict->items.emplace_back(iv, nv);
        else v.dict->items[(size_t)k].second = nv;
        return;
    }
    if (!iv.is_num()) rterr("index must be a number", line);
    int64_t i = iv.inum();
    if (v.t == Type::Array) {
        if (i < 1) rterr("array index must be +ve number, got: " + std::to_string(i), line);
        if (i > (int64_t)1 << 26 && (size_t)i > v.arr->size()) rterr("array index too large: " + std::to_string(i), line);   // [auditA] a[2^31] = x tried to allocate ~100 GB
        if ((size_t)i > v.arr->size()) v.arr->resize((size_t)i);   // MXS grows array on write past end
        (*v.arr)[(size_t)(i - 1)] = nv;
        return;
    }
    if (v.t == Type::BitArray) {
        if (i < 1) rterr("BitArray index out of range", line);
        if (i > (int64_t)1 << 30 && (size_t)i > v.bits->size()) rterr("BitArray index too large", line);   // [auditA]
        if ((size_t)i > v.bits->size()) v.bits->resize((size_t)i, false);
        (*v.bits)[(size_t)(i - 1)] = nv.t == Type::Bool ? nv.b : nv.truthy();
        return;
    }
    if (v.t == Type::String) {
        if (nv.t != Type::String || nv.s->size() != 1) rterr("single-char string required", line);
        if (i < 1 || (size_t)i > v.s->size()) rterr("String index out of range", line);
        (*v.s)[(size_t)(i - 1)] = (*nv.s)[0];
        return;
    }
    if (v.t == Type::Struct && v.sinst) {
        // 'mat[i] = subMaterial' -- assigning a MultiMaterial slot. Grows the
        // list like an array does, because scripts fill slots out of order:
        //     mat[matid] = StandardMaterial()
        auto it = v.sinst->slots.find("materiallist");
        if (it != v.sinst->slots.end() && it->second.t == Type::Array) {
            if (i < 1) rterr("index must be +ve number, got: " + std::to_string(i), line);
            if ((size_t)i > it->second.arr->size()) it->second.arr->resize((size_t)i);
            (*it->second.arr)[(size_t)(i - 1)] = nv;
            // keep numsubs in step so 'mat.numsubs' stays truthful
            v.sinst->slots["numsubs"] = Value::I((int64_t)it->second.arr->size());
            return;
        }
    }
    rterr("Cannot index-assign " + v.to_display(), line);
}

// -------- 'as' conversions -------------------------------------------------
// Parse "[1,2,3]" (surrounding whitespace allowed) into n numbers; false when
// the text is not exactly a point literal of that arity.
static bool parse_point(const std::string& s, int n, double* out) {
    size_t i = 0;
    auto ws = [&]() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; };
    ws();
    if (i >= s.size() || s[i] != '[') return false;
    ++i;
    for (int k = 0; k < n; ++k) {
        ws();
        const char* b = s.c_str() + i; char* e = nullptr;
        out[k] = strtod(b, &e);
        if (e == b) return false;
        i += (size_t)(e - b);
        ws();
        if (k + 1 < n) { if (i >= s.size() || s[i] != ',') return false; ++i; }
    }
    if (i >= s.size() || s[i] != ']') return false;
    ++i; ws();
    return i == s.size();
}

static Value as_cast(const Value& v0, const std::string& ty, int line) {
    const Value& v = v0.t == Type::Reference ? v0.ref->v : v0;
    if (ty == "string") return Value::S(v.to_string());
    // Time converts to TICKS as a number ('currentTime as integer' / ticksPerFrame)
    if (v.t == Type::Time && (ty == "integer" || ty == "float" || ty == "double" || ty == "integer64"))
        return ty == "integer" || ty == "integer64" ? Value::I((int64_t)std::llround(v.f * 160.0)) : Value::F(std::round(v.f * 160.0));
    if (ty == "time") {
        if (v.is_num()) return Value::T(v.num());
        if (v.t == Type::String) {
            const char* p = v.s->c_str(); char* e = nullptr;
            double d = strtod(p, &e);
            if (e == p) return Value::undef();
            if (*e == 't') d /= 160.0; else if (*e == 's') d *= 30.0; else if (*e == 'm') d *= 1800.0;
            return Value::T(d);
        }
        rterr("cannot convert to time: " + v.to_display(), line);
    }
    if (ty == "point2" || ty == "point3" || ty == "point4") {
        const int n = ty[5] - '0';
        if (v.t == Type::String) {
            double c[4] = {0, 0, 0, 0};
            if (!parse_point(*v.s, n, c)) return Value::undef();
            return n == 2 ? Value::P2((float)c[0], (float)c[1]) : n == 3 ? Value::P3((float)c[0], (float)c[1], (float)c[2])
                          : Value::P4((float)c[0], (float)c[1], (float)c[2], (float)c[3]);
        }
        if (v.vec && v.t != Type::Interval) {
            if (n == 2) return Value::P2(v.vec->x, v.vec->y);
            if (n == 3) return Value::P3(v.vec->x, v.vec->y, v.vec->z);
            return Value::P4(v.vec->x, v.vec->y, v.vec->z, v.t == Type::Color ? v.vec->w : v.vec->w);
        }
        if (v.is_num()) { float f = (float)v.num(); return n == 2 ? Value::P2(f, f) : n == 3 ? Value::P3(f, f, f) : Value::P4(f, f, f, f); }
        rterr("cannot convert to " + ty + ": " + v.to_display(), line);
    }
    if (ty == "color") {
        if (v.t == Type::Color) return v;
        if (v.t == Type::Point3) return Value::ColorV(v.vec->x, v.vec->y, v.vec->z);
        if (v.t == Type::Point4) return Value::ColorV(v.vec->x * 255.f, v.vec->y * 255.f, v.vec->z * 255.f, v.vec->w * 255.f);
        rterr("cannot convert to color: " + v.to_display(), line);
    }
    if (ty == "quat" || ty == "eulerangles" || ty == "angleaxis" || (ty == "matrix3" && v.t != Type::Quat)) {
        if (ty == "quat" && v.t == Type::Quat) return v;
        if (ty == "eulerangles" && v.t == Type::EulerAngles) return v;
        if (ty == "angleaxis" && v.t == Type::AngleAxis) return v;
        if (ty == "matrix3" && v.t == Type::Matrix3) return v;
        if (ty == "angleaxis" && v.t == Type::Quat) {
            double d, x, y, z; quat_to_angleaxis(*v.vec, d, x, y, z);
            return Value::AngAx((float)d, (float)x, (float)y, (float)z);
        }
        if (ty == "quat" && v.t == Type::AngleAxis) {
            Vec4 q = quat_from_angleaxis(v.vec->w, v.vec->x, v.vec->y, v.vec->z);
            return Value::QuatV(q.x, q.y, q.z, q.w);
        }
        Mat3 m;
        if (!rotation_of(v, m)) rterr("cannot convert to " + ty + ": " + v.to_display(), line);
        if (ty == "matrix3") return [&]{ Value o = Value::M3(); *o.mat = m; return o; }();
        if (ty == "eulerangles") { double x, y, z; rot_to_euler(m, x, y, z); return Value::Euler((float)x, (float)y, (float)z); }
        Vec4 q = rot_to_quat(m);
        if (ty == "quat") return Value::QuatV(q.x, q.y, q.z, q.w);
        double d, x, y, z; quat_to_angleaxis(q, d, x, y, z);
        return Value::AngAx((float)d, (float)x, (float)y, (float)z);
    }
    if (ty == "interval" && v.t == Type::Interval) return v;
    if (ty == "dictionary" && v.t == Type::Dictionary) return v;
    if (ty == "boolean" || ty == "booleanclass") {
        if (v.t == Type::Bool) return v;
        if (v.t == Type::String) { std::string l = lower(*v.s); if (l == "true") return Value::B(true); if (l == "false") return Value::B(false); return Value::undef(); }
        if (v.is_num()) return Value::B(v.num() != 0);
        rterr("cannot convert to boolean: " + v.to_display(), line);
    }
    if (ty == "stringstream") { Value o; o.t = Type::StringStream; o.ss = std::make_shared<StringStream_t>(); o.ss->buf = v.to_string(); return o; }
    if (ty == "integer" || ty == "integer64" || ty == "integerptr") {
        if (v.is_num()) return Value::I(v.inum());
        if (v.t == Type::String) {
            // Max 8: "42" -> 42, "4.5" -> 4 (truncates), "abc" -> undefined.
            // An unparseable string is NOT an error, it yields undefined.
            const char* p = v.s->c_str();
            char* end = nullptr;
            const double d = strtod(p, &end);
            if (end == p) return Value::undef();
            return Value::I(Value::F(d).inum());   // [auditA] saturating (huge / nan strings)
        }
        rterr("cannot convert to integer: " + v.to_display(), line);
    }
    if (ty == "float" || ty == "double") {
        if (v.is_num()) return Value::F(v.num());
        if (v.t == Type::String) {
            const char* p = v.s->c_str();
            char* end = nullptr;
            const double d = strtod(p, &end);
            if (end == p) return Value::undef();      // same rule as 'as integer'
            return Value::F(d);
        }
        rterr("cannot convert to float: " + v.to_display(), line);
    }
    if (ty == "name") {
        if (v.t == Type::String) return Value::N(*v.s);
        if (v.t == Type::Name) return v;
        if (v.t == Type::Class) return Value::N(*v.name);
        rterr("cannot convert to name: " + v.to_display(), line);
    }
    if (ty == "bitarray") {
        if (v.t == Type::BitArray) return v;
        if (v.t == Type::Array) {
            Value o; o.t = Type::BitArray; o.bits = std::make_shared<std::vector<bool>>();
            for (auto& e : *v.arr) {
                if (!e.is_num() || e.inum() < 1) rterr("cannot convert to bitarray: " + v.to_display(), line);
                size_t i = (size_t)e.inum();
                if (i > o.bits->size()) o.bits->resize(i, false);
                (*o.bits)[i - 1] = true;
            }
            return o;
        }
        rterr("cannot convert to bitarray: " + v.to_display(), line);
    }
    if (ty == "array") {
        if (v.t == Type::Array) return v;
        if (v.t == Type::BitArray) {
            Array_t out;
            for (size_t k = 0; k < v.bits->size(); ++k) if ((*v.bits)[k]) out.push_back(Value::I((int64_t)k + 1));
            return Value::A(std::move(out));
        }
        if (v.t == Type::String) { Array_t out; for (char c : *v.s) out.push_back(Value::S(std::string(1, c))); return Value::A(std::move(out)); }
        if (v.t == Type::Dictionary) { Array_t out; for (auto& kv : v.dict->items) out.push_back(Value::A(Array_t{kv.first, kv.second})); return Value::A(std::move(out)); }
        rterr("cannot convert to array: " + v.to_display(), line);
    }
    if (ty == "matrix3" && v.t == Type::Quat) {
        Value out = Value::M3();
        rot_from_quat(*v.vec, *out.mat);
        return out;
    }
    rterr("unsupported 'as " + ty + "'", line);
}

// -------- calls ------------------------------------------------------------
Value call_value(Value& callee, std::vector<Value>& args,
                 std::map<std::string, Value>& kwargs, Env& env, Engine& eng, int line) {
    if (callee.t == Type::NativeFn) {
        if (eng.denied_caps) {   // [F08] capability gate (single native dispatch point)
            unsigned c = eng.capability_of(callee.nat_name);
            if (c & eng.denied_caps)
                throw MxsError(std::string("permission denied: ") + (callee.nat_name ? callee.nat_name : "native") +
                               "() needs " + ((c & eng.denied_caps & Engine::CapFile) ? "file access" : "process access") +
                               ", which the host has disabled", line);
        }
        return (*callee.nat)(args, kwargs, env);
    }
    if (callee.t == Type::Function) {
        FnDef& fd = *callee.fn;
        Engine::CapabilityScope capScope(eng, fd.deniedCaps);   // [F08] restricted code stays restricted
        // 'mapped fn': a collection first argument calls the body once per
        // element and the call returns OK.
        if (fd.mapped && !args.empty() &&
            (args[0].t == Type::Array || args[0].t == Type::BitArray)) {
            Value coll = args[0];
            std::vector<Value> items;
            if (coll.t == Type::Array) items = *coll.arr;
            else for (size_t k = 0; k < coll.bits->size(); ++k) if ((*coll.bits)[k]) items.push_back(Value::I((int64_t)k + 1));
            for (auto& it : items) {
                std::vector<Value> a2 = args;
                a2[0] = it;
                call_value(callee, a2, kwargs, env, eng, line);
            }
            return Value::ok();
        }
        Env local;
        local.parent = fd.closure ? fd.closure : env.global();
        for (size_t k = 0; k < fd.params.size(); ++k)
            local.set_local(fd.params[k], k < args.size() ? args[k] : Value::unsupplied());
        for (size_t k = 0; k < fd.kwparams.size(); ++k) {
            auto it = kwargs.find(fd.kwparams[k]);
            if (it != kwargs.end()) local.set_local(fd.kwparams[k], it->second);
            else if (k < fd.kwdefault_exprs.size() && fd.kwdefault_exprs[k])
                local.set_local(fd.kwparams[k], eval_node(fd.kwdefault_exprs[k], local, eng));
            else local.set_local(fd.kwparams[k], fd.kwdefaults[k]);
        }
        try { return eval_node(fd.body.get(), local, eng); }
        catch (ReturnFn& r) { return r.v; }
    }
    if (callee.t == Type::StructDef) {                             // instantiate
        Value inst; inst.t = Type::Struct;
        inst.sinst = std::make_shared<StructInst>();
        inst.sinst->def = callee.sdef;
        auto& d = *callee.sdef;
        for (size_t k = 0; k < d.members.size(); ++k) {
            Value v = d.defaults[k];
            if (k < args.size()) v = args[k];
            auto it = kwargs.find(d.members[k]);
            if (it != kwargs.end()) v = it->second;
            // bind member fns to instance: leave closure as-is; 'this' unsupported (documented)
            inst.sinst->slots[d.members[k]] = std::move(v);
        }
        if (d.on_create) {
            Node* body = d.on_create;
            with_staged_slots(eng, inst.sinst->slots, [&]() -> Value {
                Env local; local.parent = eng.global_env_ptr();
                try { return eval_node(body, local, eng); } catch (ReturnFn& r) { return r.v; }
            });
        }
        return inst;
    }
    rterr("Not callable: " + callee.to_display(), line);
}

// Write through a variable slot: a by-reference parameter ('fn f &x') holds a
// Reference box, and assigning the parameter assigns the caller's variable.
static void store_slot(Value* slot, const Value& v) {
    if (slot->t == Type::Reference && slot->ref) { slot->ref->v = deref(v); slot->ref->dirty = true; }
    else *slot = v;
}
static void assign_ident(Env& env, Engine& eng, const std::string& name, const Value& v) {
    Env* g = env.global();
    Value* slot = env.find(name);
    if (slot) store_slot(slot, v);
    else g->set_local(name, v);                                    // implicit global (MXS semantics)
    if (!eng.global_watch.empty()) {
        auto gi = g->vars.find(name);
        if (gi != g->vars.end() && (!slot || slot == &gi->second)) {
            auto w = eng.global_watch.find(name);
            if (w != eng.global_watch.end() && w->second) w->second(v);
        }
    }
}

static void assign_lvalue(Node* dst, const Value& v, Env& env, Engine& eng, int line) {
    if (dst->k == NK::ScenePath) rterr("cannot assign to scene path $" + dst->name, line);
        if (dst->k == NK::Ident) {
            assign_ident(env, eng, dst->name, v);
        } else if (dst->k == NK::Prop) {
            if (dst->kids[0]->k == NK::Prop && g_active_engine && g_active_engine->prop_set_hook) {
                // '$box.pos.controller = c': the host gets the dotted path first (see NK::Prop)
                Node* inner = dst->kids[0].get();
                Value gb = eval_node(inner->kids[0].get(), env, eng);
                if (gb.t == Type::Struct && gb.sinst && g_active_engine->prop_set_hook(gb, inner->name + "." + dst->name, v)) return;
            }
            Value base = eval_node(dst->kids[0].get(), env, eng);
            set_prop(base, dst->name, v, line);
            // write back for value-semantics lvalue chains rooted at an identifier
            if (dst->kids[0]->k == NK::Ident)
                if (Value* slot = env.find(dst->kids[0]->name)) store_slot(slot, base);
            // '$box.pos.x = 5' / 'm.row4.z = 1': the modified value goes back
            // through the owning property (value types only; objects are shared)
            if ((dst->kids[0]->k == NK::Prop || dst->kids[0]->k == NK::Index) && base.vec)
                assign_lvalue(dst->kids[0].get(), base, env, eng, line);
        } else {
            Value base = eval_node(dst->kids[0].get(), env, eng);
            Value idx  = eval_node(dst->kids[1].get(), env, eng);
            set_index(base, idx, v, line);
            if (dst->kids[0]->k == NK::Ident)
                if (Value* slot = env.find(dst->kids[0]->name)) store_slot(slot, base);
            // 'mat.materialIDList[10] = 20': an array read from a host property is
            // a copy, so the edited array is assigned back to the property
            if (dst->kids[0]->k == NK::Prop && (base.t == Type::Array || base.t == Type::BitArray)) {
                Value owner = eval_node(dst->kids[0]->kids[0].get(), env, eng);
                if (owner.t == Type::Struct && owner.sinst && (owner.sinst->slots.count("__h") || owner.sinst->slots.count("__a")))
                    try { set_prop(owner, dst->kids[0]->name, base, line); } catch (MxsError&) {}
            }
        }
        
}

Value eval_node(Node* n, Env& env, Engine& eng) {
    if (stack_exhausted()) throw MxsError("Stack overflow: recursion too deep", n->line);   // [auditA] instead of crashing the host
    switch (n->k) {
    case NK::Rollout: {
        // Collect a host-facing description alongside the value, so the IDE
        // can build real controls without knowing anything about the AST.
        RolloutDef def;
        def.deniedCaps = eng.denied_caps;   // [F08]
        def.name = n->name;
        def.title = n->str.empty() ? n->name : n->str;
        for (size_t i = 0; i < n->kw_names.size() && i < n->kids.size(); ++i)
            def.params[n->kw_names[i]] = eval_node(n->kids[i].get(), env, eng);

        // The rollout DEFINITION evaluates to a value holding its controls and
        // handlers. Realising it as a window is the host's job (createDialog /
        // the GX2 IDE); until that lands the definition is inert but valid, so
        // a script that defines rollouts still loads and its non-UI functions
        // still run.
        auto sd = std::make_shared<StructDef_t>();
        sd->name = n->name;
        sd->is_rollout = !n->flag;
        sd->rollout_node = n;
        Value v; v.t = Type::Struct;
        v.sinst = std::make_shared<StructInst>();
        v.sinst->def = sd;
        for (auto& kid : n->kids) {
            if (kid->k != NK::UiItem && kid->k != NK::OnHandler) continue;  // handled below
            if (kid->k == NK::OnHandler) {
                UiHandler uh;
                uh.control = kid->name;
                uh.event = kid->str;
                def.handlers.push_back(uh);
                eng.handler_bodies()[n->name + "." + kid->name + "." + kid->str] =
                    kid->kids.empty() ? nullptr : kid->kids[0].get();
                continue;
            }
            if (kid->k == NK::UiItem) {
                UiControl uc;
                uc.type = kid->str;
                uc.name = kid->name;
                if (kid->lit.t == Type::String) uc.caption = *kid->lit.s;

                // Each control becomes a slot named after its variable, so
                // 'btn.enabled = false' resolves even before there is a window.
                Value ctl; ctl.t = Type::Struct;
                ctl.sinst = std::make_shared<StructInst>();
                auto cd = std::make_shared<StructDef_t>();
                cd->name = kid->str;
                ctl.sinst->def = cd;
                // Give every control the default properties MAXScript controls
                // have. Handlers read cb.checked / lb.selection / et.text
                // without the rollout ever declaring them, and an undeclared
                // read was raising "Unknown property" and killing the handler.
                {
                    const std::string& ty = kid->str;
                    ctl.sinst->slots["enabled"] = Value::B(true);
                    ctl.sinst->slots["visible"] = Value::B(true);
                    ctl.sinst->slots["caption"] = Value::S("");
                    ctl.sinst->slots["text"]    = Value::S("");
                    ctl.sinst->slots["tooltip"] = Value::S("");
                    if (ty == "checkbox" || ty == "checkbutton" || ty == "menuitem") {
                        ctl.sinst->slots["checked"] = Value::B(false);
                        ctl.sinst->slots["state"]   = Value::B(false);
                    } else if (ty == "listbox" || ty == "multilistbox" ||
                               ty == "combobox" || ty == "dropdownlist" ||
                               ty == "radiobuttons") {
                        ctl.sinst->slots["selection"] = Value::I(0);
                        ctl.sinst->slots["items"]     = Value::A(Array_t{});
                        ctl.sinst->slots["selected"]  = Value::undef();
                    } else if (ty == "spinner" || ty == "slider" ||
                               ty == "progressbar" || ty == "angle") {
                        ctl.sinst->slots["value"] = Value::F(0.0);
                        ctl.sinst->slots["range"] = Value::P3(0, 100, 0);
                    } else if (ty == "colorpicker") {
                        ctl.sinst->slots["color"] = Value::P3(255, 255, 255);
                    } else if (ty == "bitmap" || ty == "imgtag") {
                        ctl.sinst->slots["bitmap"]   = Value::undef();
                        ctl.sinst->slots["filename"] = Value::S("");
                    }
                }
                if (kid->lit.t == Type::String) {
                    ctl.sinst->slots["caption"] = kid->lit;
                    ctl.sinst->slots["text"]    = kid->lit;
                }
                for (size_t i = 0; i < kid->kw_names.size() && i < kid->kids.size(); ++i) {
                    Value pv = eval_node(kid->kids[i].get(), env, eng);
                    ctl.sinst->slots[kid->kw_names[i]] = pv;
                    uc.params[kid->kw_names[i]] = pv;
                }
                // spinner/slider: the third range component is the initial value
                // (range:[0,100,50] --> .value 50.0; type:#integer --> 50)
                if (kid->str == "spinner" || kid->str == "slider") {
                    auto r = ctl.sinst->slots.find("range");
                    auto ty = ctl.sinst->slots.find("type");
                    const bool isInt = ty != ctl.sinst->slots.end() && ty->second.t == Type::Name && lower(*ty->second.name) == "integer";
                    if (r != ctl.sinst->slots.end() && r->second.t == Type::Point3)
                        ctl.sinst->slots["value"] = isInt ? Value::I((int64_t)r->second.vec->z) : Value::F(r->second.vec->z);
                    else if (isInt) ctl.sinst->slots["value"] = Value::I(0);
                }
                if (kid->str == "checkbox" || kid->str == "checkbutton") {
                    auto c = ctl.sinst->slots.find("checked");
                    if (c != ctl.sinst->slots.end()) ctl.sinst->slots["state"] = c->second;
                }
                def.controls.push_back(std::move(uc));
                v.sinst->slots[kid->name] = ctl;
            }
        }
        // Statements in the body (fn definitions, locals) are MEMBERS of the
        // rollout in MAXScript: 'rollout r ( fn read x = ... )' is called as
        // 'r.read 1'. Evaluate them in a child scope and fold the results into
        // the rollout's slots.
        {
            // 'fn' definitions register globally (see NK::FnDef), so a child
            // Env alone would come back empty. Snapshot the global names first
            // and fold in whatever the body added, plus anything it declared
            // locally.
            Env* g = env.global();
            std::set<std::string> before;
            for (auto& kv : g->vars) before.insert(kv.first);

            Env body;
            body.parent = &env;
            for (auto& kid : n->kids) {
                if (kid->k == NK::UiItem || kid->k == NK::OnHandler) continue;
                eval_node(kid.get(), body, eng);
            }
            for (auto& kv : body.vars)
                if (!v.sinst->slots.count(kv.first)) v.sinst->slots[kv.first] = kv.second;
            // Anything the body newly defined at global scope belongs to the
            // ROLLOUT, not the globals. Move it, don't copy it.
            //
            // This matters: a rollout is the idiomatic way to scope a MaxScript
            // tool precisely BECAUSE its contents do not leak. Copying would
            // leave a global behind, so a second run would see the previous
            // run's definitions -- the "worked yesterday, crashes today"
            // failure mode that using a rollout is meant to prevent.
            std::vector<std::string> moved;
            for (auto& kv : g->vars)
                if (!before.count(kv.first)) {
                    if (!v.sinst->slots.count(kv.first)) v.sinst->slots[kv.first] = kv.second;
                    moved.push_back(kv.first);
                }
            for (const std::string& nm : moved) g->vars.erase(nm);
        }
        env.global()->set_local(n->name, v);
        eng.rollouts()[n->name] = std::move(def);
        return v;
    }

    case NK::UiItem:
    case NK::OnHandler:
        return Value::ok();          // consumed by NK::Rollout above

    case NK::WithCtx: {
        // kids: [label?] qualifier [body]; 'set <ctx>' has no body (name == "set").
        const bool isSet = n->name == "set";
        const size_t qi = (size_t)n->pos_argc;
        Value label = n->pos_argc ? eval_node(n->kids[0].get(), env, eng) : Value::undef();
        Value qual = n->flag && n->kids.size() > qi ? eval_node(n->kids[qi].get(), env, eng) : Value::undef();
        Node* body = isSet ? nullptr : n->kids.back().get();
        const std::string what = n->str.empty() ? std::string("with") : n->str;
        if (eng.ctx_hook_v) {
            if (isSet) { eng.ctx_hook_v(what, qual, label, Engine::CtxSet, nullptr); return Value::ok(); }
            void* saved = eng.ctx_hook_v(what, qual, label, Engine::CtxEnter, nullptr);
            try {
                Value r = eval_node(body, env, eng);
                eng.ctx_hook_v(what, qual, label, Engine::CtxExit, saved);
                return r;
            } catch (...) {
                eng.ctx_hook_v(what, qual, label, Engine::CtxExit, saved);
                throw;
            }
        }
        if (isSet) return Value::ok();
        // Legacy numeric hook: 'at time 5' gives a number; 'animate on' gives
        // a BOOLEAN -- read it as 1/0 so animate-on does not arrive as 0.
        double t = qual.is_num() ? qual.num() : (qual.t == Type::Bool && qual.b ? 1.0 : 0.0);
        if (!eng.ctx_hook) return eval_node(body, env, eng);
        void* saved = eng.ctx_hook(what, t, true, nullptr);
        try {
            Value r = eval_node(body, env, eng);
            eng.ctx_hook(what, t, false, saved);
            return r;
        } catch (...) {
            eng.ctx_hook(what, t, false, saved);
            throw;
        }
    }

    case NK::Scripted:
        return eval_scripted(n, env, eng);

    case NK::MaxCmd: {   // [mxsfix 1c] returns ok, like Max; an unknown command is a runtime error
        if (eng.max_command_hook && eng.max_command_hook(n->name)) return Value::ok();
        rterr("Unknown max command: max " + n->name, n->line);
    }
    case NK::When: {   // [mxs25 F] change handler: the host registers it against its scene objects
        Value objs = eval_node(n->kids[0].get(), env, eng);
        std::map<std::string, Value> kw;
        for (size_t k = 0; k < n->kw_names.size(); ++k) kw[n->kw_names[k]] = eval_node(n->kids[1 + k].get(), env, eng);
        auto fd = std::make_shared<FnDef>();
        fd->deniedCaps = eng.denied_caps;
        fd->name = "when";
        fd->params = n->fn_params;
        fd->body = std::shared_ptr<Node>(n->kids.back().get(), [](Node*) {});   // AST owned by the Engine
        fd->closure = env.global();   // docs: the handler runs in its own context, not the creating one's locals
        Value fv; fv.t = Type::Function; fv.fn = fd;
        if (!eng.when_hook) rterr("when: change handlers need a host with a scene (none registered)", n->line);
        return eng.when_hook(n->name, objs, kw, fv);
    }

    case NK::ScenePath: {
        if (eng.scene_path_hook) return eng.scene_path_hook(n->name);
        rterr("scene path '$" + n->name + "' needs a host with a scene "
              "(none registered)", n->line);
        return Value::undef();
    }

    case NK::PointLit: {
        double comp[4] = { 0, 0, 0, 0 };
        for (size_t k = 0; k < n->kids.size(); ++k) {
            Value cv = eval_node(n->kids[k].get(), env, eng);
            if (!cv.is_num()) rterr("point literal components must be numbers", n->line);
            comp[k] = cv.num();
        }
        if (n->kids.size() == 2) return Value::P2((float)comp[0], (float)comp[1]);
        if (n->kids.size() == 3) return Value::P3((float)comp[0], (float)comp[1], (float)comp[2]);
        return Value::P4((float)comp[0], (float)comp[1], (float)comp[2], (float)comp[3]);
    }
    case NK::Lit:
        // [auditA] strings are mutable in place (s[1] = "x"): a literal must not hand out the AST's own
        // buffer, or the next evaluation of the same code (a loop, a cached AST) sees the edit
        if (n->lit.t == Type::String && n->lit.s) return Value::S(*n->lit.s);
        return n->lit;
    case NK::Ident: {
        Value* v = env.find(n->name);
        if (!eng.global_read.empty()) {
            // live host globals (subObjectLevel): only when the name resolves to
            // the global itself, so a same-named local still shadows it
            Env* g = env.global();
            auto gi = g->vars.find(n->name);
            if (!v || (gi != g->vars.end() && v == &gi->second)) {
                auto r = eng.global_read.find(n->name);
                if (r != eng.global_read.end() && r->second) return r->second();
            }
        }
        if (v) return v->t == Type::Reference && v->ref ? v->ref->v : *v;
        if (eng.strict_undefined_reads) rterr("Undefined variable: " + n->name, n->line);
        return Value::undef();                                     // MXS: unknown global reads as undefined
    }
    case NK::Block: {
        Value last = Value::ok();
        for (auto& k : n->kids) last = eval_node(k.get(), env, eng);
        return last;
    }
    case NK::ArrayLit: {
        Array_t out; out.reserve(n->kids.size());
        for (auto& k : n->kids) out.push_back(eval_node(k.get(), env, eng));
        return Value::A(std::move(out));
    }
    case NK::BitArrayLit: {
        Value v; v.t = Type::BitArray; v.bits = std::make_shared<std::vector<bool>>();
        auto setbit = [&](int64_t i) {
            if (i < 1) rterr("bad bitarray index", n->line);
            if (i > (int64_t)1 << 30 && (size_t)i > v.bits->size()) rterr("BitArray index too large", n->line);   // [auditA] #{1..2^40}
            if ((size_t)i > v.bits->size()) v.bits->resize((size_t)i, false);
            (*v.bits)[(size_t)(i - 1)] = true;
        };
        for (auto& k : n->kids) {
            if (k->k == NK::Binary && k->name == "..") {
                int64_t a = eval_node(k->kids[0].get(), env, eng).inum();
                int64_t b = eval_node(k->kids[1].get(), env, eng).inum();
                for (int64_t i = a; i <= b; ++i) setbit(i);
            } else setbit(eval_node(k.get(), env, eng).inum());
        }
        return v;
    }
    case NK::Prop: {
        // 'obj.track.prop' on host objects: offer the host the dotted path first, so
        // '$box.pos.controller' reaches the track instead of a property of the Point3 value.
        if (n->kids[0]->k == NK::Prop && n->kids[0]->kids[0]->k != NK::Prop && g_active_engine && g_active_engine->prop_get_hook) {
            Node* inner = n->kids[0].get();
            Value gb = eval_node(inner->kids[0].get(), env, eng);
            if (gb.t == Type::Struct && gb.sinst) {
                Value out;
                if (g_active_engine->prop_get_hook(gb, inner->name + "." + n->name, out)) return out;
            }
            Value mid = get_prop(gb, inner->name, inner->line);
            return get_prop(mid, n->name, n->line);
        }
        Value base = eval_node(n->kids[0].get(), env, eng);
        return get_prop(base, n->name, n->line);
    }
    case NK::Index: {
        Value base = eval_node(n->kids[0].get(), env, eng);
        Value idx  = eval_node(n->kids[1].get(), env, eng);
        return get_index(base, idx, n->line);
    }
    case NK::Unary: {
        Value v = eval_node(n->kids[0].get(), env, eng);
        if (n->name == "&") return v;             // '&x' outside an argument list: the value
        if (n->name == "-") {
            if (v.t == Type::Int)   return Value::I(-v.i);
            if (v.t == Type::Float) return Value::F(-v.f);
            if (v.t == Type::Time)  return Value::T(-v.num()); // [Delta 007] negative time literals, e.g. -5f
            if (v.t == Type::BitArray) {                           // -<bitarray>: complement within .count
                Value o; o.t = Type::BitArray; o.bits = std::make_shared<std::vector<bool>>(*v.bits);
                o.bits->flip();
                return o;
            }
            if (v.vec) { Value o = v; o.vec = std::make_shared<Vec4>(*v.vec);
                o.vec->x = -o.vec->x; o.vec->y = -o.vec->y; o.vec->z = -o.vec->z; o.vec->w = -o.vec->w; return o; }
            rterr("cannot negate " + v.to_display(), n->line);
        }
        return Value::B(!v.truthy());                              // not
    }
    case NK::Binary: {
        Value a = eval_node(n->kids[0].get(), env, eng);
        Value b = eval_node(n->kids[1].get(), env, eng);
        const std::string& op = n->name;
        if (op == "==") return Value::B(a.eq(b));
        if (op == "!=") return Value::B(!a.eq(b));
        if (op == "<")  return Value::B(cmp_vals(a, b, n->line) < 0);
        if (op == ">")  return Value::B(cmp_vals(a, b, n->line) > 0);
        if (op == "<=") return Value::B(cmp_vals(a, b, n->line) <= 0);
        if (op == ">=") return Value::B(cmp_vals(a, b, n->line) >= 0);
        return arith(op, a, b, n->line);
    }
    case NK::And: {
        Value a = eval_node(n->kids[0].get(), env, eng);
        if (!a.truthy()) return Value::B(false);
        return Value::B(eval_node(n->kids[1].get(), env, eng).truthy());
    }
    case NK::Or: {
        Value a = eval_node(n->kids[0].get(), env, eng);
        if (a.truthy()) return Value::B(true);
        return Value::B(eval_node(n->kids[1].get(), env, eng).truthy());
    }
    case NK::AsCast: {
        Value v = eval_node(n->kids[0].get(), env, eng);
        return as_cast(v, n->name, n->line);
    }
    case NK::Assign: {
        Value v = eval_node(n->kids[1].get(), env, eng);
        assign_lvalue(n->kids[0].get(), v, env, eng, n->line);
        return v;
    }
    case NK::CompoundAssign: {
        // desugar: dst = dst <op> rhs
        Node* dst = n->kids[0].get();
        // [auditA] 's += "x"' on a plain string variable nobody else references appends in place:
        // the desugared form copied the whole string every time (a loop building a string was O(n^2)).
        // Aliases (t = s), watched / live globals and by-reference slots take the general path.
        if (n->name == "+" && dst->k == NK::Ident && !eng.global_watch.count(dst->name) && !eng.global_read.count(dst->name)) {
            Value* slot = env.find(dst->name);
            if (slot && slot->t == Type::String && slot->s) {
                Value cur = *slot;                                   // evaluation order as the desugared form
                Value rhs = eval_node(n->kids[1].get(), env, eng);
                slot = env.find(dst->name);                          // the rhs may have (re)assigned it
                if (slot && slot->t == Type::String && slot->s == cur.s && slot->s.use_count() == 2 && rhs.t == Type::String && rhs.s) {
                    cur = Value();                                   // only the variable holds the buffer now
                    slot->s->append(*rhs.s);
                    return *slot;
                }
                Value v = arith(n->name, cur, rhs, n->line);
                assign_lvalue(dst, v, env, eng, n->line);
                return v;
            }
        }
        Value cur = eval_node(dst, env, eng);
        Value rhs = eval_node(n->kids[1].get(), env, eng);
        Value v = arith(n->name, cur, rhs, n->line);
        assign_lvalue(dst, v, env, eng, n->line);
        return v;
    }
    case NK::Call: {
        // 'obj.method args' -- keep the owning object so a member function can
        // see its SIBLING members. This is what makes a rollout usable as a
        // class container with no UI at all:
        //     rollout r ( local pos = 0   fn setPos p = (pos = p) )
        //     r.setPos 12   -->  r.pos == 12
        // Without it, 'pos = p' created a global and r.pos never changed.
        Value owner;
        bool haveOwner = false;
        if (n->kids[0]->k == NK::Prop) {
            owner = eval_node(n->kids[0]->kids[0].get(), env, eng);
            haveOwner = (owner.t == Type::Struct && owner.sinst != nullptr);
        }
        Value callee = haveOwner ? get_prop(owner, n->kids[0]->name, n->line)
                                 : eval_node(n->kids[0].get(), env, eng);

        std::vector<Value> args;
        std::map<std::string, Value> kwargs;
        // '&var' arguments: the callee gets a Reference box; after the call a
        // box the callee wrote ('dirty') is copied back into the variable.
        // Natives get the plain value and write through set_out_arg().
        struct RefArg { Node* target; std::shared_ptr<RefBox_t> box; bool own; };
        std::vector<RefArg> refs;
        const bool nativeCallee = callee.t == Type::NativeFn;
        OutArgFrame frame;
        auto refTarget = [](Node* a) -> Node* {
            while (a && a->k == NK::Block && a->kids.size() == 1) a = a->kids[0].get();
            return a && a->k == NK::Unary && a->name == "&" ? a->kids[0].get() : nullptr;
        };
        auto evalArg = [&](Node* a, std::shared_ptr<RefBox_t>* outBox) -> Value {
            Node* t = refTarget(a);
            if (!t) return eval_node(a, env, eng);
            std::shared_ptr<RefBox_t> box;
            bool own = true;
            if (t->k == NK::Ident) {
                Value* slot = env.find(t->name);
                if (slot && slot->t == Type::Reference && slot->ref) { box = slot->ref; own = false; }
            }
            if (!box) { box = std::make_shared<RefBox_t>(); box->v = eval_node(t, env, eng); }
            refs.push_back(RefArg{t, box, own});
            *outBox = box;
            if (nativeCallee) return box->v;
            Value r; r.t = Type::Reference; r.ref = box; return r;
        };
        for (int k = 0; k < n->pos_argc; ++k) {
            std::shared_ptr<RefBox_t> box;
            args.push_back(evalArg(n->kids[1 + k].get(), &box));
            frame.pos.push_back(box);
        }
        for (size_t k = 0; k < n->kw_names.size(); ++k) {
            std::shared_ptr<RefBox_t> box;
            kwargs[n->kw_names[k]] = evalArg(n->kids[1 + n->pos_argc + k].get(), &box);
            if (box) frame.kw[n->kw_names[k]] = box;
        }
        if (callee.t == Type::Undefined && n->kids[0]->k == NK::Ident)
            rterr("Call to undefined function: " + n->kids[0]->name, n->line);
        auto doCall = [&]() -> Value {
        if (haveOwner && callee.t == Type::Function) {
            // A function body binds to GLOBAL scope (see call_value), not to
            // the caller's, so a child Env here would never be consulted. The
            // members are therefore staged into the global scope for the
            // duration of the call and restored afterwards -- which is
            // effectively where MAXScript puts a rollout's own scope: above
            // global for its members, invisible everywhere else.
            Env* g = eng.global_env_ptr();
            std::map<std::string, Value> saved;
            std::vector<std::string> added;
            for (auto& slot : owner.sinst->slots) {
                if (Value* prev = g->vars.count(slot.first) ? &g->vars[slot.first] : nullptr)
                    saved[slot.first] = *prev;
                else
                    added.push_back(slot.first);
                g->vars[slot.first] = slot.second;
            }
            // 'this' inside a struct member function (Max 9+, harmless earlier)
            if (!owner.sinst->slots.count("this")) {
                if (g->vars.count("this")) saved["this"] = g->vars["this"]; else added.push_back("this");
                g->vars["this"] = owner;
            }
            Value r;
            try {
                r = call_value(callee, args, kwargs, env, eng, n->line);
            } catch (...) {
                for (auto& slot : owner.sinst->slots)
                    if (g->vars.count(slot.first)) slot.second = g->vars[slot.first];
                for (auto& kv : saved) g->vars[kv.first] = kv.second;
                for (const std::string& nm : added) g->vars.erase(nm);
                throw;
            }
            // Copy mutations back into the object, then restore the globals.
            for (auto& slot : owner.sinst->slots)
                if (g->vars.count(slot.first)) slot.second = g->vars[slot.first];
            for (auto& kv : saved) g->vars[kv.first] = kv.second;
            for (const std::string& nm : added) g->vars.erase(nm);
            return r;
        }
        return call_value(callee, args, kwargs, env, eng, n->line);
        };
        struct FrameGuard {
            OutArgFrame* prev;
            explicit FrameGuard(OutArgFrame* f) : prev(g_out_frame) { g_out_frame = f; }
            ~FrameGuard() { g_out_frame = prev; }
        };
        if (refs.empty()) { FrameGuard fg(nullptr); return doCall(); }
        // Run the call with the out-arg frame active, then copy back.
        Value r;
        {
            FrameGuard fg(nativeCallee ? &frame : nullptr);
            r = doCall();
        }
        for (auto& ra : refs)
            if (ra.own && ra.box->dirty) assign_lvalue(ra.target, ra.box->v, env, eng, n->line);
        return r;
    }
    case NK::If: {
        Value c = eval_node(n->kids[0].get(), env, eng);
        if (c.truthy()) return eval_node(n->kids[1].get(), env, eng);
        if (n->flag)    return eval_node(n->kids[2].get(), env, eng);
        return Value::undef();
    }
    case NK::While: {
        eng.poll_abort(n->line);
        Value r = Value::ok();
        try {
            while (eval_node(n->kids[0].get(), env, eng).truthy()) {
                eng.poll_abort(n->line);
                try { eval_node(n->kids[1].get(), env, eng); }
                catch (ContinueLoop&) {}
            }
        } catch (ExitLoop& e) { return e.v.t == Type::Undefined ? Value::ok() : e.v; }
        return r;
    }
    case NK::DoWhile: {
        eng.poll_abort(n->line);
        try {
            do {
                eng.poll_abort(n->line);
                try { eval_node(n->kids[0].get(), env, eng); }
                catch (ContinueLoop&) {}
            } while (eval_node(n->kids[1].get(), env, eng).truthy());
        } catch (ExitLoop& e) { return e.v.t == Type::Undefined ? Value::ok() : e.v; }
        return Value::ok();
    }
    case NK::ForNum: case NK::ForIn: {
        bool collect = n->kw_names[0] == "collect";
        const size_t nextra = n->kw_names.size() - 1;
        Node* where = nullptr;
        Node* whileC = nullptr;
        for (size_t k = 0; k < nextra; ++k) {
            Node* x = n->kids[n->kids.size() - 1 - nextra + k].get();
            if (n->kw_names[1 + k] == "where") where = x; else whileC = x;
        }
        Node* body  = n->kids.back().get();
        Env local; local.parent = &env;
        Array_t collected;
        struct StopLoop {};
        auto iterate = [&](const Value& item) {
            eng.poll_abort(n->line);
            local.set_local(n->name, item);
            if (whileC && !eval_node(whileC, local, eng).truthy()) throw StopLoop{};
            if (where && !eval_node(where, local, eng).truthy()) return;
            try {
                Value r = eval_node(body, local, eng);
                if (collect && r.t != Type::DontCollect) collected.push_back(std::move(r));
            } catch (ContinueLoop&) {}
        };
        try {
            if (n->k == NK::ForNum) {
                Value a = eval_node(n->kids[0].get(), env, eng);
                Value b = eval_node(n->kids[1].get(), env, eng);
                Value st = n->flag ? eval_node(n->kids[2].get(), env, eng) : Value::I(1);
                if (a.t == Type::Int && b.t == Type::Int && st.t == Type::Int) {
                    int64_t s = st.i; if (s == 0) rterr("zero step", n->line);
                    if (s > 0) for (int64_t i = a.i; i <= b.i; i += s) iterate(Value::I(i));
                    else       for (int64_t i = a.i; i >= b.i; i += s) iterate(Value::I(i));
                } else {
                    double s = st.num(); if (s == 0) rterr("zero step", n->line);
                    const bool tm = a.t == Type::Time || b.t == Type::Time;   // for t = 0f to 100f
                    auto mk = [tm](double i) { return tm ? Value::T(i) : Value::F(i); };
                    if (s > 0) for (double i = a.num(); i <= b.num() + 1e-9; i += s) iterate(mk(i));
                    else       for (double i = a.num(); i >= b.num() - 1e-9; i += s) iterate(mk(i));
                }
            } else {
                Value coll = eval_node(n->kids[0].get(), env, eng);
                if (coll.t == Type::Array) {
                    // Index rather than range-for, and COPY the element before
                    // running the body. The body may append to (or otherwise
                    // resize) the very array being iterated -- which real
                    // importers do constantly:
                    //     for f in faces do append faces (split f)
                    // A range-for holds a reference into the vector's buffer,
                    // so the first reallocation leaves it dangling and the next
                    // copy touches freed memory (0xDDDDDDDD).
                    //
                    // The bound is re-read each step, so items appended during
                    // the loop are visited -- matching MAXScript, where the
                    // array is live.
                    for (size_t k = 0; k < coll.arr->size(); ++k) {
                        Value item = (*coll.arr)[k];     // copy: body may resize
                        iterate(item);
                    }
                }
                else if (coll.t == Type::BitArray) {
                    for (size_t k = 0; k < coll.bits->size(); ++k)
                        if ((*coll.bits)[k]) iterate(Value::I((int64_t)k + 1));
                }
                else if (coll.t == Type::NativeFn || coll.t == Type::Function) {
                    // MAXScript exposes 'objects', 'selection', 'geometry' &c
                    // as COLLECTIONS, not calls: 'for o in objects do ...'.
                    // They are functions here, so iterating one invokes it and
                    // walks the result. Without this the loop reported
                    // "cannot iterate objects()".
                    std::vector<Value> noargs;
                    std::map<std::string, Value> nokw;
                    Value got = call_value(coll, noargs, nokw, env, eng, n->line);
                    if (got.t == Type::Array)
                        for (size_t k = 0; k < got.arr->size(); ++k) {
                            Value item = (*got.arr)[k];
                            iterate(item);
                        }
                    else if (got.t != Type::Undefined) iterate(got);
                }
                else if (coll.t == Type::String) {
                    for (char c : *coll.s) iterate(Value::S(std::string(1, c)));
                }
                else if (coll.t == Type::Struct && coll.sinst && coll.sinst->slots.count("__keyarray")) {
                    // MAXKeyArray must enumerate the current controller keys,
                    // not the snapshot cached when the wrapper was created.
                    Value count;
                    if (!eng.prop_get_hook || !eng.prop_get_hook(coll, "count", count) || !count.is_num())
                        rterr("invalid MAXKeyArray", n->line);
                    const int64_t nkeys = count.inum();
                    for (int64_t k = 1; k <= nkeys; ++k) {
                        Value item;
                        if (!eng.index_get_hook || !eng.index_get_hook(coll, Value::I(k), item))
                            rterr("invalid MAXKeyArray index", n->line);
                        iterate(item);
                    }
                }
                else if (coll.t == Type::Struct && coll.sinst && coll.sinst->slots.count("__items") &&
                         coll.sinst->slots["__items"].t == Type::Array) {
                    Value items = coll.sinst->slots["__items"];
                    for (size_t k = 0; k < items.arr->size(); ++k) { Value item = (*items.arr)[k]; iterate(item); }
                }
                else if (coll.t == Type::Dictionary) {
                    // Max 2017: iterating a Dictionary yields its key/value pairs
                    for (size_t k = 0; k < coll.dict->items.size(); ++k) {
                        auto& kv = coll.dict->items[k];
                        iterate(Value::A(Array_t{kv.first, kv.second}));
                    }
                }
                else rterr("cannot iterate " + coll.to_display(), n->line);
            }
        } catch (ExitLoop& e) {
            if (collect) return Value::A(std::move(collected));
            return e.v.t == Type::Undefined ? Value::ok() : e.v;
        } catch (StopLoop&) {}
        return collect ? Value::A(std::move(collected)) : Value::ok();
    }
    case NK::Case: {
        bool has_test = n->flag;
        size_t base = has_test ? 1 : 0;
        Value test;
        if (has_test) test = eval_node(n->kids[0].get(), env, eng);
        size_t nlabels = n->kw_names.size();
        // pairs: kids[base + 2k] = label (or null), kids[base + 2k + 1] = body
        Node* default_body = nullptr;
        for (size_t k = 0; k < nlabels; ++k) {
            Node* label = n->kids[base + 2*k].get();
            Node* body  = n->kids[base + 2*k + 1].get();
            if (!label) { default_body = body; continue; }
            Value lv = eval_node(label, env, eng);
            bool hit = has_test ? test.eq(lv) : lv.truthy();
            if (hit) return eval_node(body, env, eng);
        }
        if (default_body) return eval_node(default_body, env, eng);
        return Value::undef();
    }
    case NK::FnDef: {
        auto fd = std::make_shared<FnDef>();
        fd->deniedCaps = eng.denied_caps;   // [F08]
        fd->name = n->name;
        fd->params = n->fn_params;
        fd->kwparams = n->fn_kwparams;
        for (size_t k = 0; k < n->fn_kwparams.size(); ++k) {
            Node* d = n->kids[k].get();
            const bool lit = d->k == NK::Lit;
            fd->kwdefaults.push_back(lit ? d->lit : Value::undef());
            fd->kwdefault_exprs.push_back(lit ? nullptr : d);
        }
        // body is the last kid; share ownership via aliasing shared_ptr on the AST root the Engine holds
        fd->body = std::shared_ptr<Node>(n->kids.back().get(), [](Node*) {});   // non-owning; AST outlives (Engine keeps roots)
        fd->closure = env.global();                                             // MXS fns see globals, not enclosing locals
        fd->mapped = n->flag;
        Value v; v.t = Type::Function; v.fn = fd;
        env.global()->set_local(n->name, v);                                    // fn defs are global unless 'local fn' (unsupported)
        return v;
    }
    case NK::StructDef: {
        auto sd = std::make_shared<StructDef_t>();
        sd->name = n->name;
        for (size_t k = 0; k < n->kw_names.size(); ++k) {
            const std::string& mn = n->kw_names[k];
            if (mn == "on:create") { sd->on_create = n->kids[k].get(); continue; }
            if (mn.rfind("on:", 0) == 0) continue;               // other struct events: accepted, not raised
            if (mn.rfind("fn:", 0) == 0) {
                // member function: define it, store the Function value as default
                Value fv = eval_node(n->kids[k].get(), env, eng);
                env.global()->vars.erase(n->kids[k]->name);        // don't leak member fn into globals
                sd->members.push_back(mn.substr(3));
                sd->defaults.push_back(fv);
            } else {
                sd->members.push_back(mn);
                sd->defaults.push_back(n->kids[k] ? eval_node(n->kids[k].get(), env, eng) : Value::undef());
            }
        }
        Value v; v.t = Type::StructDef; v.sdef = sd;
        env.global()->set_local(n->name, v);
        return v;
    }
    case NK::LocalDecl: case NK::GlobalDecl: {
        Value last = Value::undef();
        Env* target = n->k == NK::GlobalDecl ? env.global() : &env;
        if (n->k == NK::GlobalDecl && n->flag)
            for (auto& nm : n->kw_names) eng.add_persistent(nm);
        for (size_t k = 0; k < n->kw_names.size(); ++k) {
            // A persistent global restored from a scene keeps its value when the
            // declaring script runs again without an initialiser.
            if (n->k == NK::GlobalDecl && !n->kids[k]) {
                // Documented listener results: 'global x' --> OK, while
                // 'persistent global x' --> the variable's (undefined) value.
                auto ex = target->vars.find(n->kw_names[k]);
                if (ex == target->vars.end()) target->set_local(n->kw_names[k], Value::undef());
                last = n->flag ? target->vars[n->kw_names[k]] : Value::ok();
                continue;
            }
            Value v = n->kids[k] ? eval_node(n->kids[k].get(), env, eng) : Value::undef();
            target->set_local(n->kw_names[k], v);
            last = v;
        }
        return last;
    }
    case NK::Return:   throw ReturnFn{ n->kids.empty() ? Value::ok() : eval_node(n->kids[0].get(), env, eng) };
    case NK::Exit:     throw ExitLoop{ n->flag ? eval_node(n->kids[0].get(), env, eng) : Value::undef() };
    case NK::Continue: throw ContinueLoop{};
    case NK::Throw: {
        if (n->kids.empty()) {
            // bare 'throw' inside a catch re-raises the current exception
            Value* cur = env.global()->find("__current_exception");
            if (cur && cur->t == Type::String && !cur->s->empty()) throw MxsError(*cur->s, n->line);
            throw MxsError("-- Runtime error: throw", n->line);
        }
        Value v = eval_node(n->kids[0].get(), env, eng);
        MxsError e(v.t == Type::String ? *v.s : v.to_string(), n->line);
        e.thrown = v;
        throw e;
    }
    case NK::TryCatch: {
        try { return eval_node(n->kids[0].get(), env, eng); }
        catch (MxsError& e) {
            env.global()->set_local("__current_exception", Value::S(e.what()));
            return eval_node(n->kids[1].get(), env, eng);
        }
    }
    }
    rterr("internal: bad node", n->line);
}

// ================================================================ engine ====
// AST lifetime is owned directly by Engine::ast_roots (see mxs.h). Previously
// this was a static std::unordered_map<Engine*, EngineState> side-table; that
// design crashed at process exit whenever an Engine outlived the map (e.g. a
// function-local-static Engine constructed before the map's own first use --
// C++ tears down function-local statics in reverse construction order across
// TUs with no relative guarantee between unrelated statics). Fixed by giving
// each Engine its own storage: no cross-static ordering to get wrong.

Engine::Engine() : global_env(std::make_unique<Env>()) {
    on_output = [](const std::string& s) { fputs(s.c_str(), stdout); };
    install_builtins();
}
Engine::~Engine() = default;

Value Engine::call_value_public(Value& callee, std::vector<Value>& args,
                                std::map<std::string, Value>& kwargs, Env& env) {
    return call_value(callee, args, kwargs, env, *this, 0);
}

Value Engine::make_native(const std::string& name, NativeFn_t fn) {
    Value v; v.t = Type::NativeFn;
    v.nat = std::make_shared<NativeFn_t>(std::move(fn));
    v.nat_name = intern(name)->c_str();
    return v;
}

void Engine::def(const std::string& name, NativeFn_t fn) {
    Value v; v.t = Type::NativeFn;
    v.nat = std::make_shared<NativeFn_t>(std::move(fn));
    v.nat_name = intern(name)->c_str();
    global_env->set_local(lower(name), v);
}
void Engine::set_global(const std::string& name, Value v) { global_env->set_local(lower(name), std::move(v)); }
Value Engine::get_global(const std::string& name) {
    Value* v = global_env->find(lower(name));
    return v ? *v : Value::undef();
}

void Engine::poll_abort(int line) {
    if (abort_flag.load(std::memory_order_relaxed))
        throw MxsError("execution aborted", line);
    if (run_deadline) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now > run_deadline) {
            abort_flag = true;
            throw MxsError("script timed out (Engine::timeout_ms)", line);
        }
    }
}

void Engine::parse_only(const std::string& source, const std::string& source_name) {
    Parser p(source, source_name);
    NodeP root = p.parse_program();
    ast_roots.push_back(std::move(root));
}

Value Engine::execute(const std::string& source, const std::string& source_name) {
    static thread_local int depth = 0;                            // nested execute() (execute "...", script controllers)
    // [auditA] only the outermost run resets the abort request / deadline: a nested execute (a script
    // controller evaluated mid-run, 'execute "..."') used to clear a pending abort and restart the timeout
    if (depth == 0) {
        abort_flag = false;
        run_deadline = 0;
        if (timeout_ms) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count();
            run_deadline = now + (long long)timeout_ms;
        }
    }
    struct ActiveGuard {
        Engine* prev;
        explicit ActiveGuard(Engine* e) : prev(g_active_engine) { g_active_engine = e; }
        ~ActiveGuard() { g_active_engine = prev; }
    } activeGuard(this);

    // [auditA] identical sources (script controller / wire expressions every evaluation, repeated
    // Listener lines) reuse their AST instead of keeping one more parse per run forever
    Node* r = nullptr;
    std::string key;
    const bool cacheable = source.size() <= ((size_t)1 << 20);
    if (cacheable) {
        key.reserve(source_name.size() + 1 + source.size());
        key.append(source_name).push_back('\0');
        key.append(source);
        auto it = ast_cache_.find(key);
        if (it != ast_cache_.end()) r = it->second;
    }
    if (!r) {
        Parser p(source, source_name);
        NodeP root = p.parse_program();
        r = root.get();
        ast_roots.push_back(std::move(root));                      // ASTs persist: fn bodies reference them
        if (cacheable) ast_cache_.emplace(std::move(key), r);
    }
    struct Depth { Depth() { ++depth; } ~Depth() { --depth; } } depthGuard;
    try {
        Value res = eval_node(r, *global_env, *this);
        if (depth == 1) global_env->set_local("?", res);           // '?' in the next top-level statement
        return res;
    }
    catch (ReturnFn& rf) { return rf.v; }                         // top-level 'return': its value
    catch (ExitLoop&) { throw MxsError("-- 'exit' outside a loop"); }
    catch (ContinueLoop&) { throw MxsError("-- 'continue' outside a loop"); }
}

const RolloutDef* Engine::find_rollout(const std::string& name) const {
    auto it = rollouts_.find(lower(name));
    return it == rollouts_.end() ? nullptr : &it->second;
}
std::vector<std::string> Engine::rollout_names() const {
    std::vector<std::string> out;
    for (const auto& kv : rollouts_) out.push_back(kv.first);
    return out;
}
bool Engine::invoke_handler(const std::string& rollout, const std::string& control,
                            const std::string& event) {
    // Start each handler from a clean run state. A previous handler that timed
    // out or was stopped leaves abort_flag SET, and nothing cleared it -- so
    // every later button reported "execution aborted" and the only cure was
    // restarting the app.
    abort_flag = false;
    run_deadline = timeout_ms ? (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count()
                       + (long long)timeout_ms)
                              : 0;
    auto it = handler_bodies_.find(lower(rollout) + "." + lower(control) + "." + lower(event));
    if (it == handler_bodies_.end() || !it->second) return false;
    // The rollout's members must be reachable from the handler AND from
    // anything the handler calls. A child Env is not enough: a function body
    // binds to GLOBAL scope (see call_value), so a member fn invoked from the
    // handler would not see its sibling structs -- which is how a struct
    // declared inside a rollout ('struct fmtD8M_File' in juice_utility)
    // came back as "Call to undefined function" once the member-fold moved it
    // out of globals.
    //
    // So the members are STAGED into global scope for the duration and the
    // previous globals restored afterwards, exactly as the obj.method path
    // does. Same mechanism, same reasoning.
    Env* g = global_env.get();
    std::map<std::string, Value> saved;
    std::vector<std::string> added;
    Value* ro = g->find(lower(rollout));
    const bool haveRo = ro && ro->t == Type::Struct && ro->sinst;
    if (haveRo) {
        for (auto& slot : ro->sinst->slots) {
            auto ex = g->vars.find(slot.first);
            if (ex != g->vars.end()) saved[slot.first] = ex->second;
            else                     added.push_back(slot.first);
            g->vars[slot.first] = slot.second;
        }
    }
    auto restore = [&]() {
        if (!haveRo) return;
        Value* ro2 = g->find(lower(rollout));
        if (ro2 && ro2->t == Type::Struct && ro2->sinst)
            for (auto& slot : ro2->sinst->slots) {
                auto cur = g->vars.find(slot.first);
                if (cur != g->vars.end()) slot.second = cur->second;   // write back
            }
        for (auto& kv : saved) g->vars[kv.first] = kv.second;
        for (const std::string& nm : added) g->vars.erase(nm);
    };

    struct ActiveGuard {
        Engine* prev;
        explicit ActiveGuard(Engine* e) : prev(g_active_engine) { g_active_engine = e; }
        ~ActiveGuard() { g_active_engine = prev; }
    } activeGuard(this);
    const RolloutDef* rdef = find_rollout(rollout);
    CapabilityScope capScope(*this, rdef ? rdef->deniedCaps : 0u);   // [F08]

    Env local;
    local.parent = g;
    try {
        eval_node((Node*)it->second, local, *this);
    } catch (...) {
        restore();
        throw;
    }
    restore();
    return true;
}
Value Engine::get_control_prop(const std::string& rollout, const std::string& control,
                               const std::string& prop) const {
    Value* ro = global_env->find(lower(rollout));
    if (!ro || ro->t != Type::Struct) return Value::undef();
    auto c = ro->sinst->slots.find(lower(control));
    if (c == ro->sinst->slots.end() || c->second.t != Type::Struct) return Value::undef();
    auto p = c->second.sinst->slots.find(lower(prop));
    return p == c->second.sinst->slots.end() ? Value::undef() : p->second;
}
void Engine::set_control_prop(const std::string& rollout, const std::string& control,
                              const std::string& prop, const Value& v) {
    Value* ro = global_env->find(lower(rollout));
    if (!ro || ro->t != Type::Struct) return;
    auto c = ro->sinst->slots.find(lower(control));
    if (c == ro->sinst->slots.end() || c->second.t != Type::Struct) return;
    c->second.sinst->slots[lower(prop)] = v;
}

void Engine::reset_globals() {
    // Wipe user globals but keep the builtins, so a script can be re-run from
    // a clean slate without restarting the host. MAXScript has no equivalent
    // short of restarting 3ds Max, which is the source of a whole class of
    // stale-state bugs.
    Env* g = global_env.get();
    std::vector<std::string> drop;
    for (auto& kv : g->vars)
        if (!builtin_names.count(kv.first)) drop.push_back(kv.first);
    for (const std::string& n : drop) g->vars.erase(n);
    rollouts_.clear();
    handler_bodies_.clear();
}

std::vector<std::string> Engine::all_global_names() const {
    std::vector<std::string> out;
    for (const auto& kv : global_env->vars) out.push_back(kv.first);
    return out;
}

std::vector<std::string> Engine::user_global_names() const {
    std::vector<std::string> out;
    for (const auto& kv : global_env->vars)
        if (!builtin_names.count(kv.first)) out.push_back(kv.first);
    return out;
}

void Engine::mark_builtins_complete() {
    builtin_names.clear();
    for (const auto& kv : global_env->vars) builtin_names.insert(kv.first);
}

Value Engine::make_object(const std::string& className) {
    auto sd = std::make_shared<StructDef_t>(); sd->name = className;
    Value v; v.t = Type::Struct;
    v.sinst = std::make_shared<StructInst>();
    v.sinst->def = sd;
    return v;
}
void Engine::obj_set(Value& obj, const std::string& prop, const Value& v) {
    if (obj.t == Type::Struct && obj.sinst) obj.sinst->slots[lower(prop)] = v;
}
Value Engine::obj_get(const Value& obj, const std::string& prop) {
    if (obj.t != Type::Struct || !obj.sinst) return Value::undef();
    auto it = obj.sinst->slots.find(lower(prop));
    return it == obj.sinst->slots.end() ? Value::undef() : it->second;
}
bool Engine::obj_is(const Value& obj, const std::string& className) {
    return obj.t == Type::Struct && obj.sinst && obj.sinst->def &&
           lower(obj.sinst->def->name) == lower(className);
}
bool Engine::obj_same(const Value& a, const Value& b) {
    if (!(a.t == Type::Struct && b.t == Type::Struct && a.sinst && b.sinst)) return false;
    if (a.sinst == b.sinst) return true;
    if (auto& h = identity_hook()) return h(a, b) == 1;
    return false;
}

Value Engine::make_namespace(const std::string& name) {
    auto sd = std::make_shared<StructDef_t>(); sd->name = name;
    Value v; v.t = Type::Struct;
    v.sinst = std::make_shared<StructInst>(); v.sinst->def = sd;
    return v;
}
void Engine::ns_def(Value& ns, const std::string& name, NativeFn_t fn) {
    Value v; v.t = Type::NativeFn;
    v.nat = std::make_shared<NativeFn_t>(std::move(fn));
    v.nat_name = intern(name)->c_str();
    ns.sinst->slots[lower(name)] = std::move(v);
}

void Engine::tag_capability(const std::string& nativeName, unsigned caps) { cap_tags_[lower(nativeName)] |= caps; }
unsigned Engine::capability_of(const char* natName) const {
    if (!natName || cap_tags_.empty()) return 0;
    auto it = cap_tags_.find(lower(natName));
    return it == cap_tags_.end() ? 0u : it->second;
}
void Engine::require_capability(unsigned cap, const std::string& what) const {
    if (cap & denied_caps)
        throw MxsError("permission denied: " + what + " needs " + ((cap & denied_caps & CapFile) ? "file access" : "process access") +
                       ", which the host has disabled");
}

void Engine::install_builtins() {
    install_core(*this, *global_env);
    install_file(*this, *global_env);
    install_math3d(*this, *global_env);
    install_ext(*this, *global_env);      // core backlog: see tools/coverage_report.py
    install_scripted(*this, *global_env); // macroScripts, plugins, tools, dontCollect
    install_lang2(*this, *global_env);    // Max 8 language backlog (builtins_lang2.cpp)
    // [F08] engine-level natives that touch the file system or spawn processes.
    // Names that do not exist (shellLaunch, DOSCommand, python.execute ...) are
    // tagged too, so a host that later defines them is gated by default.
    static const char* fileNatives[] = {
        "fopen", "openfile", "createfile", "filein", "include", "deletefile", "renamefile", "copyfile", "makedir",
        "removedir", "getfiles", "getdirectories", "enumeratefiles", "doesfileexist", "getfilesize", "getfileattribute",
        "setfileattribute", "getfilecreatedate", "getfilemoddate", "getfullfilepath", "isdirectorywriteable",
        "getinisetting", "setinisetting", "delinisetting", "hasinisetting", "openlog", "flushlog", "closelog",
        "openencryptedfile", "encryptfile", "encryptscript", "loadpythonfile"};
    static const char* processNatives[] = {"shelllaunch", "doscommand", "hiddendoscommand", "python", "pythonexecute",
                                           "dotnetobject", "dotnetclass", "createoleobject", "registerole", "ole"};
    for (const char* n : fileNatives) tag_capability(n, CapFile);
    for (const char* n : processNatives) tag_capability(n, CapProcess | CapFile);
}

} // namespace mxs
