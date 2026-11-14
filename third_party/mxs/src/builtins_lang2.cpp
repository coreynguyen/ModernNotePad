// Max 8 language backlog, driven by the documented-examples conformance suite
// (tests/test_mxs_conformance.cpp): Dictionary, Interval / EulerAngles /
// AngleAxis constructors, StringStream reading, globalVars, units, assert_*,
// value-type reflection (getProperty on a Point3, ...), date/time helpers.
//
// Functions that already existed are wrapped, not replaced: the wrapper
// handles the new cases and falls through to the previous definition.
#include "mxs_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <random>

namespace mxs {

using Args = std::vector<Value>;
using KW = std::map<std::string, Value>;

[[noreturn]] static void lerr(const std::string& m) { throw MxsError(m); }
static Value& larg(Args& a, size_t i, const char* fn) {
    if (i >= a.size()) lerr(std::string(fn) + ": missing argument " + std::to_string(i + 1));
    return a[i];
}
static double lnum(const Value& v, const char* fn) {
    if (!v.is_num()) lerr(std::string(fn) + ": number expected, got " + v.to_display());
    return v.num();
}
static std::string keystr(const Value& v) { return lower(v.t == Type::Name ? *v.name : v.to_string()); }

// Override 'name' keeping the previous definition as a fallback: f returns
// false to defer to it.
using Hook = std::function<bool(Args&, KW&, Env&, Value&)>;
static void wrap(Engine& e, const char* name, Hook f) {
    Value prev = e.get_global(name);
    Engine* pe = &e;
    e.def(name, [prev, f, pe](Args& a, KW& kw, Env& env) mutable -> Value {
        Value out;
        if (f(a, kw, env, out)) return out;
        if (prev.t == Type::Undefined) lerr("no handler for these arguments");
        return call_value(prev, a, kw, env, *pe, 0);
    });
}

// ------------------------------------------------------------ helpers ----
static Value new_dict(const std::string& keyType) {
    Value d; d.t = Type::Dictionary; d.dict = std::make_shared<Dict_t>();
    d.dict->keyType = keyType;
    return d;
}
static bool deep_equal(const Value& x, const Value& y, bool strict) {
    if (x.t == Type::Array && y.t == Type::Array) {
        if (x.arr->size() != y.arr->size()) return false;
        for (size_t i = 0; i < x.arr->size(); ++i)
            if (!deep_equal((*x.arr)[i], (*y.arr)[i], strict)) return false;
        return true;
    }
    if (x.t == Type::Dictionary && y.t == Type::Dictionary) {
        if (x.dict->items.size() != y.dict->items.size()) return false;
        for (auto& kv : x.dict->items) {             // unordered
            int k = y.dict->find(kv.first);
            if (k < 0 || !deep_equal(kv.second, y.dict->items[(size_t)k].second, strict)) return false;
        }
        return true;
    }
    if (x.t == Type::Struct && y.t == Type::Struct && x.sinst && y.sinst) {
        if (x.sinst == y.sinst) return true;
        if (x.sinst->def != y.sinst->def || x.sinst->slots.size() != y.sinst->slots.size()) return false;
        for (auto& kv : x.sinst->slots) {
            auto it = y.sinst->slots.find(kv.first);
            if (it == y.sinst->slots.end() || !deep_equal(kv.second, it->second, strict)) return false;
        }
        return true;
    }
    if (strict && x.t != y.t) return false;
    if (x.t == Type::String && y.t == Type::String) return *x.s == *y.s;
    if (x.t == Type::Matrix3 && y.t == Type::Matrix3) return x.eq(y);
    return x.eq(y);
}
static Value deep_copy(const Value& v) {
    switch (v.t) {
        case Type::Array: { Array_t o; for (auto& e : *v.arr) o.push_back(deep_copy(e)); return Value::A(std::move(o)); }
        case Type::String: return Value::S(*v.s);
        case Type::BitArray: { Value o = v; o.bits = std::make_shared<std::vector<bool>>(*v.bits); return o; }
        case Type::Dictionary: {
            Value d = new_dict(v.dict->keyType);
            for (auto& kv : v.dict->items) d.dict->items.emplace_back(kv.first, deep_copy(kv.second));
            return d;
        }
        case Type::Struct: {
            if (!v.sinst || v.sinst->slots.count("__h") || v.sinst->slots.count("__a")) return v;   // host objects
            Value o = v; o.sinst = std::make_shared<StructInst>(*v.sinst);
            for (auto& kv : o.sinst->slots) if (kv.second.t != Type::Function) kv.second = deep_copy(kv.second);
            return o;
        }
        case Type::Matrix3: { Value o = v; o.mat = std::make_shared<Mat3>(*v.mat); return o; }
        default:
            if (v.vec) { Value o = v; o.vec = std::make_shared<Vec4>(*v.vec); return o; }
            return v;
    }
}

// Max's ParamType2 numbering (scripted plug-in parameter types).
static const char* kParamTypes[] = {
    "float", "integer", "color", "point3", "boolean", "angle", "percent", "worldUnits",
    "string", "filename", "hsv", "colorChannel", "time", "radiobtnIndex", "material",
    "texturemap", "bitmap", "node", "maxObject", "index", "matrix3", "paramBlock2",
    "point4", "frgba", nullptr };

// ------------------------------------------------- stringstream reading ----
static bool is_ss(const Value& v) { return v.t == Type::StringStream && v.ss; }
// Extent of one MAXScript literal/expression starting at i (after blanks).
static size_t scan_value(const std::string& b, size_t i) {
    if (i >= b.size()) return i;
    char c = b[i];
    if (c == '"') {
        ++i;
        while (i < b.size() && b[i] != '"') { if (b[i] == '\\') ++i; ++i; }
        return std::min(b.size(), i + 1);
    }
    auto group = [&](char open, char close) {
        int depth = 0;
        bool q = false;
        for (; i < b.size(); ++i) {
            if (b[i] == '"' && (i == 0 || b[i - 1] != '\\')) q = !q;
            if (q) continue;
            if (b[i] == open) ++depth;
            else if (b[i] == close && --depth == 0) return i + 1;
        }
        return b.size();
    };
    if (c == '[') return group('[', ']');
    if (c == '(') return group('(', ')');
    if (c == '#' && i + 1 < b.size() && b[i + 1] == '(') { ++i; return group('(', ')'); }
    if (c == '#' && i + 1 < b.size() && b[i + 1] == '{') { ++i; return group('{', '}'); }
    while (i < b.size() && !isspace((unsigned char)b[i]) && b[i] != ',') ++i;
    return i;
}

// -------------------------------------------------------------- units ----
static double unit_inches(const std::string& u) {
    // conversion factor from unit to inches
    std::string l = lower(u);
    if (l == "in" || l == "inches" || l == "\"" || l == "inch") return 1.0;
    if (l == "ft" || l == "feet" || l == "'" || l == "foot") return 12.0;
    if (l == "mi" || l == "miles" || l == "mile") return 63360.0;
    if (l == "fl" || l == "furlong" || l == "furlongs") return 7920.0;
    if (l == "mm" || l == "millimeters") return 1.0 / 25.4;
    if (l == "cm" || l == "centimeters") return 1.0 / 2.54;
    if (l == "m" || l == "meters") return 1.0 / 0.0254;
    if (l == "km" || l == "kilometers") return 1000.0 / 0.0254;
    return 0.0;
}
static double sys_unit_inches(const Value& units) {
    Value st = Engine::obj_get(units, "systemtype");
    std::string s = st.t == Type::Name ? lower(*st.name) : "inches";
    double scale = 1.0;
    Value sc = Engine::obj_get(units, "systemscale");
    if (sc.is_num()) scale = sc.num();
    double f = unit_inches(s);
    return (f > 0 ? f : 1.0) * scale;
}

void install_lang2(Engine& e, Env& g) {
    (void)g;
    Engine* pe = &e;

    // ---- constants / globals ----------------------------------------------
    e.set_global("ticksperframe", Value::I(160));
    if (e.get_global("framerate").t == Type::Undefined) e.set_global("framerate", Value::I(30));
    if (e.get_global("currenttime").t == Type::Undefined) e.set_global("currenttime", Value::T(0));
    if (e.get_global("animationrange").t == Type::Undefined) e.set_global("animationrange", Value::Ival(0, 100));
    for (const char* cn : { "Time", "Interval", "EulerAngles", "AngleAxis", "Dictionary", "Number",
                            "Point2", "Point3", "Point4", "Quat", "Matrix3", "Color", "StringStream", "FileStream" }) {
        // Class values for comparisons ('classOf x == Point3'): only where no
        // constructor of that name exists (constructors keep their binding and
        // classOf compares against them by name, see the eq hook below).
        if (e.get_global(cn).t == Type::Undefined) e.set_global(cn, Value::C(cn));
    }

    // ---- predefined globals (reference: "Predefined Globals") ---------------
    {
        struct C { const char* n; float r, g, b; };
        static const C cols[] = { {"red",255,0,0}, {"green",0,255,0}, {"blue",0,0,255}, {"white",255,255,255},
                                  {"black",0,0,0}, {"orange",255,128,0}, {"yellow",255,255,0}, {"brown",128,64,0},
                                  {"gray",128,128,128} };
        for (const C& c : cols) if (e.get_global(c.n).t == Type::Undefined) e.set_global(c.n, Value::ColorV(c.r, c.g, c.b));
        e.set_global("x_axis", Value::P3(1, 0, 0));
        e.set_global("y_axis", Value::P3(0, 1, 0));
        e.set_global("z_axis", Value::P3(0, 0, 1));
    }

    // ---- free --------------------------------------------------------------
    e.def("free", [](Args& a, KW&, Env&) -> Value {
        if (!a.empty()) {
            Value& v = a[0];
            if (v.t == Type::Array) v.arr->clear();
            else if (v.t == Type::String) v.s->clear();
            else if (v.t == Type::StringStream) { v.ss->buf.clear(); v.ss->rpos = 0; }
            else if (v.t == Type::Dictionary) v.dict->items.clear();
        }
        return Value::ok();
    });

    // ---- Dictionary (Max 2017+) -------------------------------------------
    e.def("dictionary", [](Args& a, KW&, Env&) -> Value {
        std::string kt = "name";
        size_t i = 0;
        if (!a.empty() && a[0].t == Type::Name) {
            std::string k = lower(*a[0].name);
            if (k == "name" || k == "string" || k == "integer") { kt = k; i = 1; }
        }
        Value d = new_dict(kt);
        for (; i < a.size(); ++i) {
            if (a[i].t != Type::Array || a[i].arr->size() != 2) lerr("Dictionary: #(key, value) pairs expected");
            const Value& k = (*a[i].arr)[0];
            if (i == 0 || (a[0].t != Type::Name || d.dict->items.empty()))
                d.dict->keyType = k.t == Type::String ? "string" : k.t == Type::Int ? "integer" : d.dict->keyType;
            int f = d.dict->find(k);
            if (f < 0) d.dict->items.emplace_back(k, (*a[i].arr)[1]);
            else d.dict->items[(size_t)f].second = (*a[i].arr)[1];
        }
        return d;
    });
    auto dictOf = [](Value& v, const char* fn) -> Dict_t& {
        if (v.t != Type::Dictionary) lerr(std::string(fn) + ": Dictionary expected, got " + v.to_display());
        return *v.dict;
    };
    e.def("putdictvalue", [dictOf](Args& a, KW& kw, Env&) -> Value {
        Dict_t& d = dictOf(larg(a, 0, "PutDictValue"), "PutDictValue");
        Value& k = larg(a, 1, "PutDictValue");
        Value& v = larg(a, 2, "PutDictValue");
        int f = d.find(k);
        auto it = kw.find("putonlyifnew");
        if (f >= 0 && it != kw.end() && it->second.truthy()) return Value::B(false);
        if (f < 0) d.items.emplace_back(k, v); else d.items[(size_t)f].second = v;
        return Value::B(true);
    });
    e.def("setdictvalue", [dictOf](Args& a, KW&, Env&) -> Value {
        Dict_t& d = dictOf(larg(a, 0, "SetDictValue"), "SetDictValue");
        int f = d.find(larg(a, 1, "SetDictValue"));
        if (f < 0) d.items.emplace_back(a[1], larg(a, 2, "SetDictValue")); else d.items[(size_t)f].second = larg(a, 2, "SetDictValue");
        return Value::ok();
    });
    e.def("getdictvalue", [dictOf](Args& a, KW&, Env&) -> Value {
        Dict_t& d = dictOf(larg(a, 0, "GetDictValue"), "GetDictValue");
        int f = d.find(larg(a, 1, "GetDictValue"));
        return f < 0 ? Value::undef() : d.items[(size_t)f].second;
    });
    e.def("hasdictvalue", [dictOf](Args& a, KW&, Env&) -> Value {
        Dict_t& d = dictOf(larg(a, 0, "HasDictValue"), "HasDictValue");
        return Value::B(d.find(larg(a, 1, "HasDictValue")) >= 0);
    });
    e.def("removedictvalue", [dictOf](Args& a, KW&, Env&) -> Value {
        Dict_t& d = dictOf(larg(a, 0, "RemoveDictValue"), "RemoveDictValue");
        int f = d.find(larg(a, 1, "RemoveDictValue"));
        if (f < 0) return Value::B(false);
        d.items.erase(d.items.begin() + f);
        return Value::B(true);
    });
    e.def("getdictkeys", [dictOf](Args& a, KW&, Env&) -> Value {
        Dict_t& d = dictOf(larg(a, 0, "GetDictKeys"), "GetDictKeys");
        Array_t o; for (auto& kv : d.items) o.push_back(kv.first); return Value::A(std::move(o));
    });
    e.def("getdictvalues", [dictOf](Args& a, KW&, Env&) -> Value {
        Dict_t& d = dictOf(larg(a, 0, "GetDictValues"), "GetDictValues");
        Array_t o; for (auto& kv : d.items) o.push_back(kv.second); return Value::A(std::move(o));
    });
    e.def("getdictkeytype", [dictOf](Args& a, KW&, Env&) -> Value {
        return Value::N(dictOf(larg(a, 0, "GetDictKeyType"), "GetDictKeyType").keyType);
    });

    // ---- deepEqual / copy / deepCopy ---------------------------------------
    e.def("deepequal", [](Args& a, KW& kw, Env&) -> Value {
        auto it = kw.find("strictcompare");
        bool strict = it != kw.end() && it->second.truthy();
        return Value::B(deep_equal(larg(a, 0, "deepEqual"), larg(a, 1, "deepEqual"), strict));
    });
    wrap(e, "copy", [](Args& a, KW&, Env&, Value& out) {
        if (a.empty()) return false;
        Value& v = a[0];
        // 'copy <array> #nomap': the #nomap flag only stops mapping over the
        // collection; the result is the same shallow copy.
        if (v.t == Type::Array) { out = Value::A(*v.arr); return true; }
        if (v.t == Type::BitArray) { out = v; out.bits = std::make_shared<std::vector<bool>>(*v.bits); return true; }
        if (v.t == Type::Dictionary) { out = new_dict(v.dict->keyType); out.dict->items = v.dict->items; return true; }
        if (v.t == Type::StringStream) { out = v; out.ss = std::make_shared<StringStream_t>(*v.ss); return true; }
        if (v.t == Type::Struct && v.sinst && !v.sinst->slots.count("__h") && !v.sinst->slots.count("__a") &&
            v.sinst->def && !v.sinst->def->members.empty()) {
            out = v; out.sinst = std::make_shared<StructInst>(*v.sinst); return true;
        }
        if (v.t == Type::Interval || v.t == Type::EulerAngles || v.t == Type::AngleAxis) {
            out = v; out.vec = std::make_shared<Vec4>(*v.vec); return true;
        }
        return false;
    });
    e.def("deepcopy", [](Args& a, KW&, Env&) -> Value { return deep_copy(larg(a, 0, "deepCopy")); });

    // ---- rotation / time value constructors --------------------------------
    e.def("interval", [](Args& a, KW&, Env&) -> Value {
        return Value::Ival(lnum(larg(a, 0, "interval"), "interval"), lnum(larg(a, 1, "interval"), "interval"));
    });
    e.def("eulerangles", [](Args& a, KW&, Env&) -> Value {
        auto c = [&](size_t i) { return a.size() > i ? (float)lnum(a[i], "eulerAngles") : 0.f; };
        return Value::Euler(c(0), c(1), c(2));
    });
    e.def("angleaxis", [](Args& a, KW&, Env&) -> Value {
        Value& a0 = larg(a, 0, "angleAxis");
        if (a0.t == Type::Quat || a0.t == Type::EulerAngles || a0.t == Type::Matrix3) {
            Mat3 m; rotation_of(a0, m);
            double d, x, y, z; quat_to_angleaxis(rot_to_quat(m), d, x, y, z);
            return Value::AngAx((float)d, (float)x, (float)y, (float)z);
        }
        Value& ax = larg(a, 1, "angleAxis");
        if (!ax.vec) lerr("angleAxis: point3 axis expected");
        return Value::AngAx((float)lnum(a0, "angleAxis"), ax.vec->x, ax.vec->y, ax.vec->z);
    });
    wrap(e, "quat", [](Args& a, KW&, Env&, Value& out) {
        // quat <degrees> <axis>
        if (a.size() == 2 && a[0].is_num() && a[1].vec) {
            Vec4 q = quat_from_angleaxis(a[0].num(), a[1].vec->x, a[1].vec->y, a[1].vec->z);
            out = Value::QuatV(q.x, q.y, q.z, q.w); return true;
        }
        if (a.size() == 1 && (a[0].t == Type::EulerAngles || a[0].t == Type::AngleAxis || a[0].t == Type::Matrix3)) {
            Mat3 m; rotation_of(a[0], m); Vec4 q = rot_to_quat(m);
            out = Value::QuatV(q.x, q.y, q.z, q.w); return true;
        }
        return false;
    });
    e.def("eulertoquat", [](Args& a, KW&, Env&) -> Value {
        Value& v = larg(a, 0, "eulerToQuat");
        if (!v.vec) lerr("eulerToQuat: eulerAngles expected");
        Mat3 m; rot_from_euler(v.vec->x, v.vec->y, v.vec->z, m);
        Vec4 q = rot_to_quat(m);
        return Value::QuatV(q.x, q.y, q.z, q.w);
    });
    e.def("quattoeuler", [](Args& a, KW&, Env&) -> Value {
        Value& v = larg(a, 0, "quatToEuler");
        Mat3 m;
        if (!rotation_of(v, m)) lerr("quatToEuler: quat expected");
        double x, y, z; rot_to_euler(m, x, y, z);
        return Value::Euler((float)x, (float)y, (float)z);
    });
    e.def("quattoeuler2", [](Args& a, KW&, Env&) -> Value {
        Value& v = larg(a, 0, "quatToEuler2");
        Mat3 m;
        if (!rotation_of(v, m)) lerr("quatToEuler2: quat expected");
        double x, y, z; rot_to_euler(m, x, y, z);
        return Value::Euler((float)x, (float)y, (float)z);
    });
    wrap(e, "inverse", [](Args& a, KW&, Env&, Value& out) {
        if (a.empty() || a[0].t != Type::Quat) return false;
        const Vec4& q = *a[0].vec;
        double n = (double)q.x*q.x + (double)q.y*q.y + (double)q.z*q.z + (double)q.w*q.w;
        if (n < 1e-20) lerr("inverse: zero quat");
        out = Value::QuatV((float)(-q.x/n), (float)(-q.y/n), (float)(-q.z/n), (float)(q.w/n));
        return true;
    });
    wrap(e, "normalize", [](Args& a, KW&, Env&, Value& out) {
        if (a.empty() || a[0].t != Type::Quat) return false;
        const Vec4& q = *a[0].vec;
        double l = std::sqrt((double)q.x*q.x + (double)q.y*q.y + (double)q.z*q.z + (double)q.w*q.w);
        if (l < 1e-20) { out = Value::QuatV(0, 0, 0, 1); return true; }
        out = Value::QuatV((float)(q.x/l), (float)(q.y/l), (float)(q.z/l), (float)(q.w/l));
        return true;
    });
    wrap(e, "rotate", [](Args& a, KW&, Env&, Value& out) {
        // rotate <matrix3> <quat|eulerAngles|angleAxis>  (in place, post-multiply)
        if (a.size() < 2 || a[0].t != Type::Matrix3) return false;
        Mat3 r;
        if (!rotation_of(a[1], r)) return false;
        Mat3& m = *a[0].mat;
        Mat3 o = m;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j)
                o.m[i][j] = m.m[i][0]*r.m[0][j] + m.m[i][1]*r.m[1][j] + m.m[i][2]*r.m[2][j];
        m = o;
        out = a[0];
        return true;
    });
    wrap(e, "prerotate", [](Args& a, KW&, Env&, Value& out) {
        if (a.size() < 2 || a[0].t != Type::Matrix3) return false;
        Mat3 r;
        if (!rotation_of(a[1], r)) return false;
        Mat3& m = *a[0].mat;
        Mat3 o = m;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                o.m[i][j] = r.m[i][0]*m.m[0][j] + r.m[i][1]*m.m[1][j] + r.m[i][2]*m.m[2][j];
        m = o;
        out = a[0];
        return true;
    });
    for (const char* nm : { "prerotatex", "prerotatey", "prerotatez" }) {
        const int ax = nm[9] - 'x';
        e.def(nm, [ax](Args& a, KW&, Env&) -> Value {
            Value& mv = larg(a, 0, "preRotate");
            if (mv.t != Type::Matrix3) lerr("preRotateX/Y/Z: matrix3 expected");
            double d = lnum(larg(a, 1, "preRotate"), "preRotate");
            Mat3 r; rot_from_euler(ax == 0 ? d : 0, ax == 1 ? d : 0, ax == 2 ? d : 0, r);
            Mat3& m = *mv.mat; Mat3 o = m;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    o.m[i][j] = r.m[i][0]*m.m[0][j] + r.m[i][1]*m.m[1][j] + r.m[i][2]*m.m[2][j];
            m = o;
            return mv;
        });
    }

    // ---- random with vector / time ranges; one seeded generator ------------
    {
        static std::mt19937 rng{ 0x4d415832u };
        static bool seeded = false;
        if (!seeded) { rng.seed(std::random_device{}()); seeded = true; }
        wrap(e, "seed", [](Args& a, KW&, Env&, Value&) {
            if (!a.empty() && a[0].is_num()) rng.seed((unsigned)a[0].inum());
            return false;   // the previous seed also re-seeds the noise permutation
        });
        e.def("random", [](Args& a, KW&, Env&) -> Value {
            Value& lo = larg(a, 0, "random"); Value& hi = larg(a, 1, "random");
            auto rf = [](double x, double y) { if (y < x) std::swap(x, y); std::uniform_real_distribution<double> d(x, y); return x == y ? x : d(rng); };
            if (lo.t == Type::Int && hi.t == Type::Int) {
                int64_t x = lo.i, y = hi.i; if (y < x) std::swap(x, y);
                std::uniform_int_distribution<int64_t> d(x, y); return Value::I(d(rng));
            }
            if (lo.t == Type::Time || hi.t == Type::Time) {
                double x = lo.num() * 160, y = hi.num() * 160; if (y < x) std::swap(x, y);
                std::uniform_int_distribution<int64_t> d((int64_t)x, (int64_t)y); return Value::T(d(rng) / 160.0);
            }
            if (lo.is_num() && hi.is_num()) return Value::F(rf(lo.num(), hi.num()));
            if (lo.vec && hi.vec && lo.t == hi.t) {
                Value o = lo; o.vec = std::make_shared<Vec4>();
                o.vec->x = (float)rf(lo.vec->x, hi.vec->x); o.vec->y = (float)rf(lo.vec->y, hi.vec->y);
                o.vec->z = (float)rf(lo.vec->z, hi.vec->z); o.vec->w = (float)rf(lo.vec->w, hi.vec->w);
                if (lo.t == Type::Quat) {   // random rotation between two quats: slerp-ish normalise
                    double l = std::sqrt((double)o.vec->x*o.vec->x + (double)o.vec->y*o.vec->y + (double)o.vec->z*o.vec->z + (double)o.vec->w*o.vec->w);
                    if (l > 1e-9) { o.vec->x /= (float)l; o.vec->y /= (float)l; o.vec->z /= (float)l; o.vec->w /= (float)l; }
                }
                return o;
            }
            lerr("random: numbers, times or matching vectors expected");
        });
    }

    // ---- strings ------------------------------------------------------------
    e.def("filterstring", [](Args& a, KW& kw, Env&) -> Value {
        const std::string s = larg(a, 0, "filterString").to_string();
        const std::string seps = larg(a, 1, "filterString").to_string();
        auto it = kw.find("splitemptytokens");
        const bool keepEmpty = it != kw.end() && it->second.truthy();
        Array_t out; std::string cur;
        bool any = false;
        for (char c : s) {
            if (seps.find(c) != std::string::npos) {
                if (!cur.empty() || keepEmpty) out.push_back(Value::S(cur));
                cur.clear(); any = true;
            } else cur += c;
        }
        if (!cur.empty() || (keepEmpty && any)) out.push_back(Value::S(cur));
        return Value::A(std::move(out));
    });
    wrap(e, "substring", [](Args& a, KW&, Env&, Value& out) {
        // substring s start -1 / substring s start <count past end>: whole rest
        if (a.size() >= 3 && a[0].t == Type::String && a[2].is_num() && a[2].inum() < 0 && a[1].is_num()) {
            int64_t st = std::max<int64_t>(1, a[1].inum());
            out = Value::S((size_t)st > a[0].s->size() ? std::string() : a[0].s->substr((size_t)st - 1));
            return true;
        }
        return false;
    });
    e.def("isspace", [](Args& a, KW&, Env&) -> Value {
        Value& v = larg(a, 0, "isSpace");
        return Value::B(v.t == Type::String && !v.s->empty() && isspace((unsigned char)(*v.s)[0]));
    });
    e.def("isdigit", [](Args& a, KW&, Env&) -> Value {
        Value& v = larg(a, 0, "isDigit");
        return Value::B(v.t == Type::String && !v.s->empty() && isdigit((unsigned char)(*v.s)[0]));
    });

    // ---- StringStream reading (FileStream versions fall through) -----------
    auto ssOf = [](Args& a) -> StringStream_t* { return !a.empty() && is_ss(a[0]) ? a[0].ss.get() : nullptr; };
    wrap(e, "readline", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        size_t nl = s->buf.find('\n', s->rpos);
        std::string line = s->buf.substr(s->rpos, nl == std::string::npos ? std::string::npos : nl - s->rpos);
        s->rpos = nl == std::string::npos ? s->buf.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out = Value::S(line); return true;
    });
    wrap(e, "readchar", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        if (s->rpos >= s->buf.size()) { out = Value::undef(); return true; }
        out = Value::S(std::string(1, s->buf[s->rpos++])); return true;
    });
    wrap(e, "peekchar", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        out = s->rpos >= s->buf.size() ? Value::undef() : Value::S(std::string(1, s->buf[s->rpos])); return true;
    });
    wrap(e, "readchars", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        int64_t n = a.size() > 1 ? a[1].inum() : 1;
        size_t take = std::min((size_t)std::max<int64_t>(0, n), s->buf.size() - std::min(s->rpos, s->buf.size()));
        out = Value::S(s->buf.substr(s->rpos, take)); s->rpos += take; return true;
    });
    wrap(e, "readdelimitedstring", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        std::string d = a.size() > 1 ? a[1].to_string() : std::string(" ");
        std::string o;
        while (s->rpos < s->buf.size() && d.find(s->buf[s->rpos]) == std::string::npos) o += s->buf[s->rpos++];
        if (s->rpos < s->buf.size()) ++s->rpos;
        out = Value::S(o); return true;
    });
    wrap(e, "skiptostring", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        std::string t = a.size() > 1 ? a[1].to_string() : std::string();
        size_t p = s->buf.find(t, s->rpos);
        if (p == std::string::npos) { s->rpos = s->buf.size(); out = Value::undef(); return true; }
        s->rpos = p + t.size(); out = Value::ok(); return true;
    });
    wrap(e, "skiptonextline", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        size_t nl = s->buf.find('\n', s->rpos);
        s->rpos = nl == std::string::npos ? s->buf.size() : nl + 1;
        out = Value::ok(); return true;
    });
    wrap(e, "eof", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        out = Value::B(s->rpos >= s->buf.size()); return true;
    });
    wrap(e, "filepos", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        out = Value::I((int64_t)s->rpos); return true;
    });
    wrap(e, "seek", [ssOf](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        if (a.size() > 1 && a[1].t == Type::Name && lower(*a[1].name) == "eof") s->rpos = s->buf.size();
        else if (a.size() > 1 && a[1].is_num()) s->rpos = std::min((size_t)std::max<int64_t>(0, a[1].inum()), s->buf.size());
        out = Value::ok(); return true;
    });
    wrap(e, "close", [](Args& a, KW&, Env&, Value& out) {
        if (a.empty() || !is_ss(a[0])) return false;
        out = Value::ok(); return true;
    });
    auto readTok = [pe](StringStream_t* s, bool evaluate) -> Value {
        while (s->rpos < s->buf.size() && (isspace((unsigned char)s->buf[s->rpos]) || s->buf[s->rpos] == ',')) ++s->rpos;
        if (s->rpos >= s->buf.size()) return Value::undef();
        size_t end = scan_value(s->buf, s->rpos);
        std::string text = s->buf.substr(s->rpos, end - s->rpos);
        s->rpos = end;
        (void)evaluate;
        return pe->execute(text, "<readValue>");
    };
    wrap(e, "readvalue", [ssOf, readTok](Args& a, KW&, Env&, Value& out) {
        StringStream_t* s = ssOf(a); if (!s) return false;
        out = readTok(s, false); return true;
    });
    e.def("readexpr", [ssOf, readTok, pe](Args& a, KW&, Env&) -> Value {
        StringStream_t* s = ssOf(a);
        if (s) return readTok(s, true);
        // FileStream: read the rest of the line as an expression
        if (!a.empty() && a[0].t == Type::FileStream && a[0].fs->f) {
            std::string line; int ch;
            while ((ch = fgetc(a[0].fs->f)) != EOF && ch != '\n') line += (char)ch;
            return pe->execute(line, "<readExpr>");
        }
        lerr("readExpr: stream expected");
    });
    e.def("readtoken", [ssOf](Args& a, KW&, Env&) -> Value {
        StringStream_t* s = ssOf(a);
        if (!s) lerr("readToken: stringstream expected");
        while (s->rpos < s->buf.size() && isspace((unsigned char)s->buf[s->rpos])) ++s->rpos;
        if (s->rpos >= s->buf.size()) return Value::undef();
        size_t end = scan_value(s->buf, s->rpos);
        Value o = Value::S(s->buf.substr(s->rpos, end - s->rpos)); s->rpos = end; return o;
    });

    // ---- reflection on any value (getProperty $box #radius, p3.x ...) -------
    e.def("getproperty", [](Args& a, KW&, Env&) -> Value {
        Value& o = larg(a, 0, "getProperty");
        std::string k = keystr(larg(a, 1, "getProperty"));
        if (o.t == Type::Struct && o.sinst && o.sinst->slots.count(k) && !o.sinst->slots.count("__h") && !o.sinst->slots.count("__a"))
            return o.sinst->slots[k];
        try { return value_get_prop(o, k); } catch (MxsError&) {}
        return Value::undef();
    });
    e.def("setproperty", [](Args& a, KW&, Env&) -> Value {
        Value& o = larg(a, 0, "setProperty");
        std::string k = keystr(larg(a, 1, "setProperty"));
        Value& v = larg(a, 2, "setProperty");
        value_set_prop(o, k, v);
        return v;
    });
    auto hasProp = [](Value& o, const std::string& k) {
        if (o.t == Type::Struct && o.sinst && o.sinst->slots.count(k)) return true;
        try { value_get_prop(o, k); return true; } catch (MxsError&) { return false; }
    };
    e.def("hasproperty", [hasProp](Args& a, KW&, Env&) -> Value {
        return Value::B(hasProp(larg(a, 0, "hasProperty"), keystr(larg(a, 1, "hasProperty"))));
    });
    e.def("isproperty", [hasProp](Args& a, KW&, Env&) -> Value {
        return Value::B(hasProp(larg(a, 0, "isProperty"), keystr(larg(a, 1, "isProperty"))));
    });
    wrap(e, "getpropnames", [](Args& a, KW&, Env&, Value& out) {
        if (a.empty()) return false;
        static const std::map<Type, std::vector<const char*>> props = {
            { Type::Point2, {"x", "y"} }, { Type::Point3, {"x", "y", "z"} }, { Type::Point4, {"x", "y", "z", "w"} },
            { Type::Quat, {"x", "y", "z", "w", "angle", "axis"} }, { Type::Color, {"r", "g", "b", "a", "h", "s", "v"} },
            { Type::Matrix3, {"row1", "row2", "row3", "row4", "translation", "rotationPart", "scalePart"} },
            { Type::EulerAngles, {"x", "y", "z"} }, { Type::AngleAxis, {"angle", "axis", "numrevs"} },
            { Type::Interval, {"start", "end"} }, { Type::Time, {"frame", "ticks", "normalized"} },
            { Type::Dictionary, {"count", "keys", "type"} }, { Type::Array, {"count"} },
            { Type::String, {"count"} }, { Type::BitArray, {"count", "numberSet", "isEmpty"} },
        };
        auto it = props.find(a[0].t);
        if (it == props.end()) {
            if (a[0].t == Type::Struct && a[0].sinst && a[0].sinst->def && !a[0].sinst->def->members.empty() &&
                !a[0].sinst->slots.count("__h") && !a[0].sinst->slots.count("__a")) {
                Array_t o; for (auto& m : a[0].sinst->def->members) o.push_back(Value::N(m));
                out = Value::A(std::move(o)); return true;
            }
            return false;
        }
        Array_t o; for (auto* n : it->second) o.push_back(Value::N(n));
        out = Value::A(std::move(o)); return true;
    });

    // ---- globalVars ---------------------------------------------------------
    {
        Value gv = Engine::make_namespace("globalVars");
        Engine::ns_def(gv, "get", [pe](Args& a, KW&, Env&) -> Value {
            return pe->get_global(keystr(larg(a, 0, "globalVars.get")));
        });
        Engine::ns_def(gv, "set", [pe](Args& a, KW&, Env&) -> Value {
            Value& v = larg(a, 1, "globalVars.set");
            pe->set_global(keystr(larg(a, 0, "globalVars.set")), v);
            return v;
        });
        Engine::ns_def(gv, "isglobal", [pe](Args& a, KW&, Env&) -> Value {
            return Value::B(pe->globals().vars.count(keystr(larg(a, 0, "globalVars.isGlobal"))) != 0);
        });
        Engine::ns_def(gv, "remove", [pe](Args& a, KW&, Env&) -> Value {
            return Value::B(pe->globals().vars.erase(keystr(larg(a, 0, "globalVars.remove"))) != 0);
        });
        Engine::ns_def(gv, "gather", [pe](Args&, KW&, Env&) -> Value {
            Array_t o; for (auto& n : pe->user_global_names()) o.push_back(Value::N(n)); return Value::A(std::move(o));
        });
        Engine::ns_def(gv, "getvaluetag", [pe](Args& a, KW& kw, Env& env) -> Value {
            Value v = pe->get_global(keystr(larg(a, 0, "globalVars.getValueTag")));
            Value co = pe->get_global("classof");
            Args ca{ v };
            Value c = call_value(co, ca, kw, env, *pe, 0);
            return Value::N(c.to_string());
        });
        Engine::ns_def(gv, "gettypetag", [pe](Args& a, KW&, Env&) -> Value {
            // an opaque per-global tag; the name's hash keeps it stable
            std::string k = keystr(larg(a, 0, "globalVars.getTypeTag"));
            if (!pe->globals().vars.count(k)) return Value::undef();
            uint32_t h = 2166136261u; for (unsigned char c : k) { h ^= c; h *= 16777619u; }
            return Value::I((int64_t)(h & 0x7fffffff));
        });
        e.set_global("globalvars", gv);
    }

    // ---- scripted plug-in parameter type names ------------------------------
    e.def("paramtypetotypename", [](Args& a, KW&, Env&) -> Value {
        int64_t i = (int64_t)lnum(larg(a, 0, "ParamTypeToTypeName"), "ParamTypeToTypeName");
        int n = 0; while (kParamTypes[n]) ++n;
        if (i < 0 || i >= n) return Value::undef();
        return Value::N(kParamTypes[i]);
    });
    e.def("typenametoparamtype", [](Args& a, KW&, Env&) -> Value {
        std::string k = keystr(larg(a, 0, "TypeNameToParamType"));
        for (int i = 0; kParamTypes[i]; ++i) if (lower(kParamTypes[i]) == k) return Value::I(i);
        return Value::I(-1);
    });

    // ---- units struct ---------------------------------------------------------
    {
        Value u = Engine::make_namespace("units");
        Engine::obj_set(u, "displaytype", Value::N("Generic"));
        Engine::obj_set(u, "systemtype", Value::N("inches"));
        Engine::obj_set(u, "systemscale", Value::F(1.0));
        Engine::obj_set(u, "metrictype", Value::N("meters"));
        Engine::obj_set(u, "ustype", Value::N("Frac_In"));
        Engine::obj_set(u, "usfrac", Value::I(8));
        Engine::obj_set(u, "customname", Value::S("FL"));
        Engine::obj_set(u, "customvalue", Value::F(660.0));
        Engine::obj_set(u, "customunit", Value::N("feet"));
        Value* self = nullptr; (void)self;
        Engine::ns_def(u, "formatvalue", [pe](Args& a, KW&, Env&) -> Value {
            double v = lnum(larg(a, 0, "units.formatValue"), "units.formatValue");
            Value U = pe->get_global("units");
            Value dt = Engine::obj_get(U, "displaytype");
            std::string d = dt.t == Type::Name ? lower(*dt.name) : "generic";
            char buf[64];
            double inches = v * sys_unit_inches(U);
            auto trim = [](std::string s) {
                if (s.find('.') != std::string::npos) { while (!s.empty() && s.back() == '0') s.pop_back(); if (!s.empty() && s.back() == '.') s.pop_back(); }
                return s;
            };
            if (d == "metric") {
                Value mt = Engine::obj_get(U, "metrictype");
                std::string m = mt.t == Type::Name ? lower(*mt.name) : "meters";
                static const std::pair<const char*, const char*> sfx[] = { {"millimeters","mm"}, {"centimeters","cm"}, {"meters","m"}, {"kilometers","km"} };
                const char* s = "m";
                for (auto& p : sfx) if (m == p.first) s = p.second;
                snprintf(buf, sizeof buf, "%.3f", inches / unit_inches(s));
                return Value::S(trim(buf) + s);
            }
            if (d == "us") {
                Value ut = Engine::obj_get(U, "ustype");
                std::string m = ut.t == Type::Name ? lower(*ut.name) : "frac_in";
                if (m == "dec_ft") { snprintf(buf, sizeof buf, "%.3f'", inches / 12.0); return Value::S(buf); }
                if (m == "dec_in") { snprintf(buf, sizeof buf, "%.3f\"", inches); return Value::S(buf); }
                snprintf(buf, sizeof buf, "%.3f\"", inches); return Value::S(buf);
            }
            snprintf(buf, sizeof buf, "%.3f", v);
            return Value::S(trim(buf));
        });
        Engine::ns_def(u, "decodevalue", [pe](Args& a, KW&, Env&) -> Value {
            std::string s = larg(a, 0, "units.decodeValue").to_string();
            Value U = pe->get_global("units");
            const double sys = sys_unit_inches(U);
            // feet'inches fraction" forms: 10'3 14/32"
            double total = 0; size_t i = 0;
            bool any = false;
            auto ws = [&]() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; };
            while (true) {
                ws();
                if (i >= s.size()) break;
                const char* b = s.c_str() + i; char* en = nullptr;
                double num = strtod(b, &en);
                if (en == b) lerr("units.decodeValue: cannot parse " + s);
                i += (size_t)(en - b);
                // fraction "14/32"
                if (i < s.size() && s[i] == '/') {
                    ++i; const char* b2 = s.c_str() + i; char* e2 = nullptr;
                    double den = strtod(b2, &e2); i += (size_t)(e2 - b2);
                    if (den != 0) num /= den;
                }
                ws();
                std::string unit;
                while (i < s.size() && (isalpha((unsigned char)s[i]) || s[i] == '\'' || s[i] == '"')) {
                    unit += s[i++];
                    if (unit == "'" || unit == "\"") break;
                }
                double f = unit.empty() ? 0 : unit_inches(unit);
                if (unit.empty()) {
                    // bare number: after feet it is inches, else system units
                    f = any ? 1.0 : sys;
                } else if (f == 0) {
                    Value cn = Engine::obj_get(U, "customname");
                    if (cn.t == Type::String && lower(*cn.s) == lower(unit)) {
                        Value cv = Engine::obj_get(U, "customvalue"), cu = Engine::obj_get(U, "customunit");
                        f = (cv.is_num() ? cv.num() : 1) * unit_inches(cu.t == Type::Name ? *cu.name : "feet");
                    } else lerr("units.decodeValue: unknown unit " + unit);
                }
                total += num * f;
                any = true;
            }
            return Value::F(total / sys);
        });
        e.set_global("units", u);
    }

    // ---- assert_* (Max 2010+; harmless) -------------------------------------
    {
        auto report = [pe](bool ok, const std::string& msg, KW& kw) -> Value {
            if (!ok) {
                std::string m = msg;
                auto it = kw.find("message");
                if (it != kw.end()) m += " " + it->second.to_string();
                if (pe->on_output) pe->on_output("-- " + m + "\n");
                return Value::B(false);
            }
            return Value::B(true);
        };
        e.def("assert_true", [report](Args& a, KW& kw, Env&) -> Value {
            Value& v = larg(a, 0, "assert_true");
            return report(v.t == Type::Bool && v.b, "Assert - Expected true, got " + v.to_display(), kw); });
        e.def("assert_false", [report](Args& a, KW& kw, Env&) -> Value {
            Value& v = larg(a, 0, "assert_false");
            return report(v.t == Type::Bool && !v.b, "Assert - Expected false, got " + v.to_display(), kw); });
        e.def("assert_equal", [report](Args& a, KW& kw, Env&) -> Value {
            Value& x = larg(a, 0, "assert_equal"); Value& y = larg(a, 1, "assert_equal");
            return report(x.eq(y), "Assert - Expected " + x.to_display() + ", got " + y.to_display(), kw); });
        e.def("assert_not_equal", [report](Args& a, KW& kw, Env&) -> Value {
            Value& x = larg(a, 0, "assert_not_equal"); Value& y = larg(a, 1, "assert_not_equal");
            return report(!x.eq(y), "Assert - Expected not " + x.to_display() + ", got " + y.to_display(), kw); });
        e.def("assert_defined", [report](Args& a, KW& kw, Env&) -> Value {
            Value& v = larg(a, 0, "assert_defined");
            return report(v.t != Type::Undefined, "Assert - Expected defined value", kw); });
        e.def("assert_undefined", [report](Args& a, KW& kw, Env&) -> Value {
            Value& v = larg(a, 0, "assert_undefined");
            return report(v.t == Type::Undefined, "Assert - Expected undefined, got " + v.to_display(), kw); });
        e.def("assert_float", [report](Args& a, KW& kw, Env&) -> Value {
            double x = lnum(larg(a, 0, "assert_float"), "assert_float"), y = lnum(larg(a, 1, "assert_float"), "assert_float");
            auto it = kw.find("tolerance");
            double tol = it != kw.end() && it->second.is_num() ? it->second.num() : 1e-6;
            return report(std::fabs(x - y) <= tol, "Assert - Expected " + Value::F(x).to_display() + ", got " + Value::F(y).to_display(), kw); });
        e.def("assert_string_equal", [report](Args& a, KW& kw, Env&) -> Value {
            std::string x = larg(a, 0, "assert_string_equal").to_string(), y = larg(a, 1, "assert_string_equal").to_string();
            auto it = kw.find("ignorecase");
            bool ic = it == kw.end() || it->second.truthy();
            return report(ic ? lower(x) == lower(y) : x == y, "Assert - Expected \"" + x + "\", got \"" + y + "\"", kw); });
        e.def("assert_point3_equal", [report](Args& a, KW& kw, Env&) -> Value {
            Value& x = larg(a, 0, "assert_point3_equal"); Value& y = larg(a, 1, "assert_point3_equal");
            auto it = kw.find("tolerance");
            double tol = it != kw.end() && it->second.is_num() ? it->second.num() : 1e-6;
            bool ok = x.vec && y.vec && std::fabs(x.vec->x - y.vec->x) <= tol && std::fabs(x.vec->y - y.vec->y) <= tol && std::fabs(x.vec->z - y.vec->z) <= tol;
            return report(ok, "Assert - Expected " + x.to_display() + ", got " + y.to_display(), kw); });
    }

    // ---- date / time ----------------------------------------------------------
    {
        auto nowTm = [](bool utc) {
            std::time_t t = std::time(nullptr);
            std::tm tmv{};
#ifdef _WIN32
            if (utc) gmtime_s(&tmv, &t); else localtime_s(&tmv, &t);
#else
            if (utc) gmtime_r(&t, &tmv); else localtime_r(&t, &tmv);
#endif
            return tmv;
        };
        auto fmtTm = [](const std::tm& t) {
            char buf[64];
            int h = t.tm_hour % 12; if (h == 0) h = 12;
            snprintf(buf, sizeof buf, "%d/%d/%02d %d:%02d:%02d %s", t.tm_mon + 1, t.tm_mday, t.tm_year % 100,
                     h, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "AM" : "PM");
            return std::string(buf);
        };
        auto arrTm = [](const std::tm& t) {
            using namespace std::chrono;
            int ms = (int)(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() % 1000);
            return Value::A(Array_t{ Value::I(t.tm_year + 1900), Value::I(t.tm_mon + 1), Value::I(t.tm_wday + 1),
                                     Value::I(t.tm_mday), Value::I(t.tm_hour), Value::I(t.tm_min), Value::I(t.tm_sec), Value::I(ms) });
        };
        e.global_read["localtime"] = [nowTm, fmtTm]() { return Value::S(fmtTm(nowTm(false))); };
        e.set_global("localtime", Value::S(""));
        e.def("getlocaltime", [nowTm, arrTm](Args&, KW&, Env&) -> Value { return arrTm(nowTm(false)); });
        e.def("getuniversaltime", [nowTm, arrTm](Args&, KW&, Env&) -> Value { return arrTm(nowTm(true)); });
    }

    // ---- ordering: numbers / times, strings (case-insensitive), names --------
    auto less = [](const Value& x, const Value& y) -> int {   // -1 / 0 / 1, 2 = incomparable
        if (x.is_num() && y.is_num()) return x.num() < y.num() ? -1 : x.num() > y.num() ? 1 : 0;
        auto txt = [](const Value& v, std::string& o) {
            if (v.t == Type::String) { o = lower(*v.s); return true; }
            if (v.t == Type::Name) { o = lower(*v.name); return true; }
            return false;
        };
        std::string a, b;
        if (txt(x, a) && txt(y, b) && x.t == y.t) return a < b ? -1 : a > b ? 1 : 0;
        return 2;
    };
    e.def("sort", [less](Args& a, KW&, Env&) -> Value {
        Value& v = larg(a, 0, "sort");
        if (v.t != Type::Array) lerr("sort: array expected, got " + v.to_display());
        for (size_t i = 1; i < v.arr->size(); ++i)
            if (less((*v.arr)[0], (*v.arr)[i]) == 2) lerr("sort: cannot compare " + (*v.arr)[0].to_display() + " with " + (*v.arr)[i].to_display());
        std::stable_sort(v.arr->begin(), v.arr->end(), [&](const Value& x, const Value& y) { return less(x, y) == -1; });
        return v;
    });
    for (int want = 0; want < 2; ++want) {
        e.def(want ? "amax" : "amin", [less, want](Args& a, KW&, Env&) -> Value {
            const Array_t* items; Array_t tmp;
            if (a.size() == 1 && a[0].t == Type::Array) items = a[0].arr.get();
            else { tmp = a; items = &tmp; }
            if (items->empty()) return Value::undef();
            Value best = (*items)[0];
            for (auto& v : *items) {
                int c = less(v, best);
                if (c == 2) lerr(std::string(want ? "amax" : "amin") + ": cannot compare " + v.to_display());
                if (want ? c > 0 : c < 0) best = v;
            }
            return best;
        });
    }
    wrap(e, "matrix3", [](Args& a, KW&, Env&, Value& out) {
        if (a.size() == 1 && a[0].is_num() && a[0].num() == 0) {   // matrix3 0: all zeros
            out = Value::M3();
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 3; ++c) out.mat->m[r][c] = 0;
            return true;
        }
        return false;
    });
    for (const char* nm : { "rotatex", "rotatey", "rotatez" }) {
        const int ax = nm[6] - 'x';
        wrap(e, nm, [ax](Args& a, KW&, Env&, Value& out) {
            if (a.size() < 2 || a[0].t != Type::Matrix3 || !a[1].is_num()) return false;
            double d = a[1].num();
            Mat3 r; rot_from_euler(ax == 0 ? d : 0, ax == 1 ? d : 0, ax == 2 ? d : 0, r);
            Mat3& m = *a[0].mat; Mat3 o = m;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 3; ++j)
                    o.m[i][j] = m.m[i][0]*r.m[0][j] + m.m[i][1]*r.m[1][j] + m.m[i][2]*r.m[2][j];
            m = o;
            out = a[0];
            return true;
        });
    }
    wrap(e, "iskindof", [](Args& a, KW&, Env&, Value& out) {
        if (a.size() == 2 && a[0].t == Type::Time && a[1].t == Type::Class && lower(*a[1].name) == "number") {
            out = Value::B(true); return true;
        }
        return false;
    });

    // formattedPrint <value> format:"6.2f" -- Max's format strings omit the '%'
    wrap(e, "formattedprint", [](Args& a, KW& kw, Env&, Value&) {
        auto it = kw.find("format");
        if (it != kw.end() && it->second.t == Type::String && it->second.s->find('%') == std::string::npos)
            it->second = Value::S("%" + *it->second.s);
        (void)a;
        return false;
    });

    // ---- headless dialogs ------------------------------------------------------
    // A UI host (the IDE) replaces these with real windows. Headless, opening a
    // rollout re-initialises it the way Max does -- locals and controls back to
    // their declared values -- and marks it displayed.
    auto rolloutOf = [](const Value& v) -> StructDef_t* {
        return v.t == Type::Struct && v.sinst && v.sinst->def && v.sinst->def->is_rollout ? v.sinst->def.get() : nullptr;
    };
    auto openRollout = [pe, rolloutOf](Args& a, KW&, Env&) -> Value {
        StructDef_t* d = a.empty() ? nullptr : rolloutOf(a[0]);
        if (!d || !d->rollout_node) lerr("createDialog: rollout expected");
        Value v = eval_node(d->rollout_node, *pe->global_env_ptr(), *pe);
        Engine::obj_set(v, "isdisplayed", Value::B(true));
        Engine::obj_set(v, "open", Value::B(true));
        return v;
    };
    e.def("createdialog", openRollout);
    e.def("addrollout", openRollout);
    auto closeRollout = [rolloutOf](Args& a, KW&, Env&) -> Value {
        if (!a.empty() && rolloutOf(a[0])) {
            Value v = a[0];
            Engine::obj_set(v, "isdisplayed", Value::B(false));
            Engine::obj_set(v, "open", Value::B(false));
        }
        return Value::ok();
    };
    e.def("destroydialog", closeRollout);
    e.def("removerollout", closeRollout);

    // ---- misc ----------------------------------------------------------------
    e.def("ismsplugin", [](Args& a, KW&, Env&) -> Value {
        (void)a; return Value::B(false);
    });
    e.def("ismspluginclass", [](Args& a, KW&, Env&) -> Value {
        (void)a; return Value::B(false);
    });
    e.def("isvalidobj", [](Args& a, KW&, Env&) -> Value { return Value::B(!a.empty() && a[0].t != Type::Undefined); });
    e.def("isdeleted", [](Args& a, KW&, Env&) -> Value { return Value::B(a.empty() || a[0].t == Type::Undefined); });
    e.def("getgenericid", [](Args& a, KW&, Env&) -> Value { (void)a; return Value::I(0); });
    e.def("normtime", [pe](Args& a, KW&, Env&) -> Value {
        double f = lnum(larg(a, 0, "normTime"), "normTime");
        Value r = pe->get_global("animationrange");
        double s = 0, en = 100;
        if (r.t == Type::Interval || r.t == Type::Point2) { s = r.vec->x; en = r.vec->y; }
        return Value::T(s + f * (en - s));
    });
    // classOf on the appended value types; struct instances keep their own name
    wrap(e, "superclassof", [](Args& a, KW&, Env&, Value& out) {
        if (a.empty()) return false;
        switch (a[0].t) {
            case Type::Time: out = Value::C("Number"); return true;
            case Type::Interval: case Type::EulerAngles: case Type::AngleAxis: case Type::Dictionary:
                out = Value::C("Value"); return true;
            case Type::StructDef: out = Value::C("Value"); return true;
            default: return false;
        }
    });
}

} // namespace mxs
