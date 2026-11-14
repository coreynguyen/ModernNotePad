#include "mxs_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

namespace mxs {

static Value& arg(std::vector<Value>& a, size_t i, const char* fn) {
    if (i >= a.size()) throw MxsError(std::string(fn) + ": missing argument " + std::to_string(i + 1));
    return a[i];
}
static double dnum(Value& v, const char* fn) {
    if (!v.is_num()) throw MxsError(std::string(fn) + ": number expected, got " + v.to_display());
    return v.num();
}
static int64_t inum(Value& v, const char* fn) {
    if (!v.is_num()) throw MxsError(std::string(fn) + ": integer expected, got " + v.to_display());
    return v.inum();
}
static std::string& sstr(Value& v, const char* fn) {
    if (v.t != Type::String) throw MxsError(std::string(fn) + ": string expected, got " + v.to_display());
    return *v.s;
}
static Array_t& sarr(Value& v, const char* fn) {
    if (v.t != Type::Array) throw MxsError(std::string(fn) + ": array expected, got " + v.to_display());
    return *v.arr;
}

// format "% and %\n" a b  — '%' consumes next arg; '\%' escaping handled by lexer already
std::string mxs_format(const std::string& fmt, const std::vector<Value>& args) {
    std::string out; size_t ai = 0;
    for (size_t k = 0; k < fmt.size(); ++k) {
        if (fmt[k] == '%' && ai < args.size()) {
            const Value& v = args[ai++];
            out += v.t == Type::String ? *v.s : v.to_display();   // format: strings raw, others display repr
        }
        else out += fmt[k];
    }
    return out;
}

static void emit(Engine& e, std::map<std::string, Value>& kw, const std::string& text) {
    auto it = kw.find("to");
    // 'format ... to:log' targets the file opened by openLog(). Note that
    // 'log' also resolves to the log() MATH function, so the value arriving
    // here is a Function, not a Name -- MAXScript has both spellings and the
    // stream target is the special case. Any non-stream 'to:' target means the
    // log, which is the only such target the language defines.
    if (it != kw.end() && it->second.t != Type::StringStream &&
        it->second.t != Type::FileStream) {
        if (FILE* lf = mxs_script_log()) { fwrite(text.data(), 1, text.size(), lf); return; }
        // No log open: fall through to normal output rather than losing it.
    }
    if (it != kw.end() && it->second.t == Type::StringStream) { it->second.ss->buf += text; return; }
    if (it != kw.end() && it->second.t == Type::FileStream && it->second.fs->f) {
        fwrite(text.data(), 1, text.size(), it->second.fs->f); return;
    }
    e.on_output(text);
}

void install_core(Engine& e, Env& g) {
    // ---- output ----
    e.def("print", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        emit(e, kw, arg(a, 0, "print").to_display() + "\n");
        return a[0];
    });
    e.def("format", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        std::string& f = sstr(arg(a, 0, "format"), "format");
        emit(e, kw, mxs_format(f, std::vector<Value>(a.begin() + 1, a.end())));
        return Value::ok();
    });
    e.def("messagebox", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        emit(e, kw, "[MessageBox] " + arg(a, 0, "messageBox").to_string() + "\n");
        return Value::ok();
    });
    e.def("classof", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // Order MUST track enum class Type exactly (mxs.h). classOf's own
        // result type (Class) is last, matching where Class sits in the enum.
        static const char* names[] = { "UndefinedClass","OkClass","UnsuppliedClass","BooleanClass","Integer","Float",
            "Name","String","Array","BitArray","Point2","Point3","Point4","Quat","Matrix3","Color",
            "FileStream","StringStream","MAXScriptFunction","Primitive","<struct>","StructDef","Class","DontCollect","Time","Interval","EulerAngles","AngleAxis","Dictionary","Reference" };
        Value& v = arg(a, 0, "classOf");
        // Confirmed against the docs (Structure-Definition: "classOf bill
        // --returns person"): a struct instance's class prints as its struct
        // name, bare -- not quoted, not '#'-prefixed.
        if (v.t == Type::Struct) return Value::C(v.sinst->def->name);
        return Value::C(names[(int)v.t]);
    });
    // isKindOf <value> <class> -- true when the value's class matches. Belongs
    // in the CORE engine, not the scene bindings: scripts test it on ordinary
    // values (isKindOf x Array) long before any scene exists, and it was only
    // reachable when GX2's bindings were installed.
    e.def("iskindof", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        if (a.size() < 2) return Value::B(false);
        static const char* names[] = { "UndefinedClass","OkClass","UnsuppliedClass","BooleanClass",
            "Integer","Float","Name","String","Array","BitArray","Point2","Point3","Point4",
            "Quat","Matrix3","Color","FileStream","StringStream","MAXScriptFunction",
            "Primitive","Struct","StructDef","Class","DontCollect","Time","Interval","EulerAngles","AngleAxis","Dictionary","Reference" };
        Value cls = a[0].t == Type::Struct && a[0].sinst && a[0].sinst->def
                  ? Value::C(a[0].sinst->def->name)
                  : Value::C(names[(int)a[0].t]);
        if (cls.eq(a[1])) return Value::B(true);
        // Integer is a kind of Number/Float for these tests, which is how
        // scripts use it ('isKindOf v Float' on an int must not be a surprise).
        if (a[0].t == Type::Int && a[1].t == Type::Class && lower(*a[1].name) == "number")
            return Value::B(true);
        if (a[0].t == Type::Float && a[1].t == Type::Class && lower(*a[1].name) == "number")
            return Value::B(true);
        return Value::B(false);
    });
    e.def("superclassof", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        if (a.empty()) return Value::C("Value");
        switch (a[0].t) {
            case Type::Int: case Type::Float:  return Value::C("Number");
            case Type::Point2: case Type::Point3: case Type::Point4:
            case Type::Quat: case Type::Matrix3: case Type::Color:
                                               return Value::C("Value");
            case Type::Struct:                 return Value::C("StructDef");
            default:                           return Value::C("Value");
        }
    });
    e.set_global("Number", Value::C("Number"));

    e.def("getcurrentexception", [](std::vector<Value>&, std::map<std::string, Value>&, Env& env) {
        Value* v = env.global()->find("__current_exception");
        return v ? *v : Value::S("");
    });
    e.def("gc", [](std::vector<Value>&, std::map<std::string, Value>&, Env&) { return Value::ok(); });

    // ---- execute (reentrant eval) ----
    e.def("execute", [&e](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return e.execute(sstr(arg(a, 0, "execute"), "execute"), "<execute>");
    });

    // ---- strings ----
    e.def("substring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string& s = sstr(arg(a, 0, "substring"), "substring");
        int64_t start = inum(arg(a, 1, "substring"), "substring");
        int64_t count = a.size() > 2 ? inum(a[2], "substring") : -1;
        if (start < 1) start = 1;
        if ((size_t)start > s.size()) return Value::S("");
        size_t avail = s.size() - (size_t)(start - 1);
        size_t n = count < 0 ? avail : std::min<size_t>((size_t)count, avail);
        return Value::S(s.substr((size_t)(start - 1), n));
    });
    e.def("findstring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string h = lower(sstr(arg(a, 0, "findString"), "findString"));
        std::string ne = lower(sstr(arg(a, 1, "findString"), "findString"));
        auto p = h.find(ne);
        if (p == std::string::npos) return Value::undef();
        return Value::I((int64_t)p + 1);
    });
    e.def("replace", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string s = sstr(arg(a, 0, "replace"), "replace");
        int64_t i = inum(arg(a, 1, "replace"), "replace");
        int64_t n = inum(arg(a, 2, "replace"), "replace");
        std::string& rep = sstr(arg(a, 3, "replace"), "replace");
        if (i < 1 || (size_t)(i - 1) > s.size()) throw MxsError("replace: index out of range");
        s.replace((size_t)(i - 1), (size_t)n, rep);
        return Value::S(std::move(s));
    });
    e.def("tolower", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::S(lower(sstr(arg(a, 0, "toLower"), "toLower"))); });
    e.def("toupper", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string s = sstr(arg(a, 0, "toUpper"), "toUpper");
        for (auto& c : s) c = (char)toupper((unsigned char)c);
        return Value::S(std::move(s)); });
    e.def("trimleft", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string s = sstr(arg(a, 0, "trimLeft"), "trimLeft");
        std::string cut = a.size() > 1 ? sstr(a[1], "trimLeft") : " \t\r\n";
        size_t p = s.find_first_not_of(cut);
        return Value::S(p == std::string::npos ? "" : s.substr(p)); });
    e.def("trimright", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string s = sstr(arg(a, 0, "trimRight"), "trimRight");
        std::string cut = a.size() > 1 ? sstr(a[1], "trimRight") : " \t\r\n";
        size_t p = s.find_last_not_of(cut);
        return Value::S(p == std::string::npos ? "" : s.substr(0, p + 1)); });
    e.def("filterstring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string& s = sstr(arg(a, 0, "filterString"), "filterString");
        std::string& seps = sstr(arg(a, 1, "filterString"), "filterString");
        Array_t out; std::string curp;
        for (char c : s) {
            if (seps.find(c) != std::string::npos) { if (!curp.empty()) { out.push_back(Value::S(curp)); curp.clear(); } }
            else curp += c;
        }
        if (!curp.empty()) out.push_back(Value::S(curp));
        return Value::A(std::move(out));
    });
    e.def("stringstream", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value v; v.t = Type::StringStream; v.ss = std::make_shared<StringStream_t>();
        if (!a.empty() && a[0].t == Type::String) v.ss->buf = *a[0].s;
        return v;
    });
    e.def("copy", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = arg(a, 0, "copy");
        if (v.t == Type::Array)  { return Value::A(*v.arr); }
        if (v.t == Type::String) { return Value::S(*v.s); }
        if (v.vec) { Value o = v; o.vec = std::make_shared<Vec4>(*v.vec); return o; }
        if (v.t == Type::Matrix3) { Value o = v; o.mat = std::make_shared<Mat3>(*v.mat); return o; }
        return v;
    });

    // ---- arrays ----
    e.def("append", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        sarr(arg(a, 0, "append"), "append").push_back(arg(a, 1, "append"));
        return a[0];
    });
    e.def("join", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Array_t& d = sarr(arg(a, 0, "join"), "join");
        Array_t& s = sarr(arg(a, 1, "join"), "join");
        d.insert(d.end(), s.begin(), s.end());
        return a[0];
    });
    e.def("deleteitem", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Array_t& arr2 = sarr(arg(a, 0, "deleteItem"), "deleteItem");
        int64_t i = inum(arg(a, 1, "deleteItem"), "deleteItem");
        if (i < 1 || (size_t)i > arr2.size()) throw MxsError("deleteItem: index out of range");
        arr2.erase(arr2.begin() + (ptrdiff_t)(i - 1));
        return a[0];
    });
    e.def("insertitem", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = arg(a, 0, "insertItem");
        Array_t& arr2 = sarr(arg(a, 1, "insertItem"), "insertItem");
        int64_t i = inum(arg(a, 2, "insertItem"), "insertItem");
        if (i < 1) i = 1;
        if ((size_t)i > arr2.size() + 1) i = arr2.size() + 1;
        arr2.insert(arr2.begin() + (ptrdiff_t)(i - 1), v);
        return Value::ok();
    });
    e.def("finditem", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Array_t& arr2 = sarr(arg(a, 0, "findItem"), "findItem");
        Value& v = arg(a, 1, "findItem");
        for (size_t k = 0; k < arr2.size(); ++k)
            if (arr2[k].eq(v)) return Value::I((int64_t)k + 1);
        return Value::I(0);
    });
    e.def("sort", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Array_t& arr2 = sarr(arg(a, 0, "sort"), "sort");
        std::stable_sort(arr2.begin(), arr2.end(), [](const Value& x, const Value& y) {
            if (x.is_num() && y.is_num()) return x.num() < y.num();
            if (x.t == Type::String && y.t == Type::String) return lower(*x.s) < lower(*y.s);
            return false;
        });
        return a[0];
    });
    auto minmax = [](std::vector<Value>& a, bool want_min) {
        const Array_t* items;
        Array_t tmp;
        if (a.size() == 1 && a[0].t == Type::Array) items = a[0].arr.get();
        else { tmp = a; items = &tmp; }
        if (items->empty()) return Value::undef();
        Value best = (*items)[0];
        for (auto& v : *items) {
            if (!v.is_num() || !best.is_num()) continue;
            if (want_min ? v.num() < best.num() : v.num() > best.num()) best = v;
        }
        return best;
    };
    e.def("amin", [minmax](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return minmax(a, true); });
    e.def("amax", [minmax](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return minmax(a, false); });

    // ---- math (MXS trig operates in DEGREES) ----
    constexpr double D2R = 3.14159265358979323846 / 180.0;
    e.set_global("pi", Value::F(3.14159265358979323846));

    // Bare class-reference globals, so the idiom every real MAXScript uses --
    // `classOf x == Integer` -- works without quoting or '#'.
    //
    // Deliberately NOT bound here: Point2/3/4, Quat, Matrix3, Color,
    // FileStream, StringStream -- those names already resolve to real,
    // working constructor functions (point3/matrix3/quat/color/
    // stringstream/...). In real MAXScript a class value is itself callable
    // ("Point3 1 2 3" invokes the constructor via the class reference), so
    // the class name and the constructor share one binding; this engine
    // doesn't implement that callable-class dispatch yet, so rebinding those
    // names to a bare Class value here would break the constructor instead.
    // Until callable-class dispatch exists, compare those specific types with
    // `classOf x as string == "Point3"`. classOf(x) itself is unaffected --
    // printing/logging the class is correct for every type either way.
    // Array/BitArray have no such collision (no bare array()/bitarray()
    // constructor exists), so they're bound normally.
    // UndefinedClass in particular MUST be bound. Scripts guard optional
    // plugins with
    //     if classOf JuicedMaterial != UndefinedClass do (mat = JuicedMaterial())
    // and if the name is unbound the comparison is Class != undefined, which is
    // ALWAYS true -- so the guard passes and the script calls a plugin that is
    // not there. The rest of these have no constructor to collide with either.
    for (const char* cn : { "Integer","Float","Name","String","Array","BitArray",
                            "StructDef","Class",
                            "UndefinedClass","OkClass","UnsuppliedClass",
                            "BooleanClass","MAXScriptFunction","Primitive",
                            "Value" })
    // NOT StringStream: binding it as a class shadows the stringstream()
    // CONSTRUCTOR, so '(stringstream "hi") as string' stops returning a
    // String. Same reason point3/matrix3/quat/color are excluded above -- the
    // comment there says exactly this and I bound it anyway. classOf(x) still
    // reports the right name; only the bare global is withheld.
        e.set_global(cn, Value::C(cn));
    e.set_global("e",  Value::F(2.71828182845904523536));
    auto def1 = [&e](const char* n, double (*f)(double)) {
        e.def(n, [f, n](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::F(f(dnum(arg(a, 0, n), n))); });
    };
    def1("sqrt", std::sqrt); def1("exp", std::exp); def1("log", std::log); def1("log10", std::log10);
    e.def("abs", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = arg(a, 0, "abs");
        if (v.t == Type::Int) return Value::I(std::llabs(v.i));
        return Value::F(std::fabs(dnum(v, "abs"))); });
    e.def("floor", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::floor(dnum(arg(a, 0, "floor"), "floor"))); });
    e.def("ceil", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::ceil(dnum(arg(a, 0, "ceil"), "ceil"))); });
    e.def("mod", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::fmod(dnum(arg(a, 0, "mod"), "mod"), dnum(arg(a, 1, "mod"), "mod"))); });
    e.def("sin",  [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(std::sin(dnum(arg(a,0,"sin"),"sin") * D2R)); });
    e.def("cos",  [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(std::cos(dnum(arg(a,0,"cos"),"cos") * D2R)); });
    e.def("tan",  [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(std::tan(dnum(arg(a,0,"tan"),"tan") * D2R)); });
    e.def("asin", [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(std::asin(dnum(arg(a,0,"asin"),"asin")) / D2R); });
    e.def("acos", [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(std::acos(dnum(arg(a,0,"acos"),"acos")) / D2R); });
    e.def("atan", [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(std::atan(dnum(arg(a,0,"atan"),"atan")) / D2R); });
    e.def("atan2",[D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::atan2(dnum(arg(a,0,"atan2"),"atan2"), dnum(arg(a,1,"atan2"),"atan2")) / D2R); });
    e.def("degtorad", [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(dnum(arg(a,0,"degToRad"),"degToRad") * D2R); });
    e.def("radtodeg", [D2R](std::vector<Value>& a, std::map<std::string, Value>&, Env&) { return Value::F(dnum(arg(a,0,"radToDeg"),"radToDeg") / D2R); });
    e.def("random", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        static std::mt19937 rng{ std::random_device{}() };
        Value& lo = arg(a, 0, "random"); Value& hi = arg(a, 1, "random");
        if (lo.t == Type::Int && hi.t == Type::Int) {
            std::uniform_int_distribution<int64_t> d(lo.i, hi.i); return Value::I(d(rng));
        }
        std::uniform_real_distribution<double> d(lo.num(), hi.num()); return Value::F(d(rng));
    });
    // Observed MAXScript behaviour:
    // maxVersion() -> #(8000, 17, 0). This engine targets Max 8 scripting.
    // Keep the version as one policy point so a later host profile may override it.
    e.def("maxversion", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        if (!a.empty()) throw MxsError("maxVersion: expected no arguments");
        return Value::A({Value::I(8000), Value::I(17), Value::I(0)}); });
    e.def("timestamp", [](std::vector<Value>&, std::map<std::string, Value>&, Env&) {
        using namespace std::chrono;
        return Value::I(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    });

    // ---- bit struct: implemented as a Struct instance with native members ----
    {
        auto sd = std::make_shared<StructDef_t>(); sd->name = "bit";
        Value bi; bi.t = Type::Struct;
        bi.sinst = std::make_shared<StructInst>(); bi.sinst->def = sd;
        auto natv = [](const char* n, NativeFn_t f) {
            Value v; v.t = Type::NativeFn; v.nat = std::make_shared<NativeFn_t>(std::move(f)); v.nat_name = intern(n)->c_str(); return v;
        };
        auto& slots = bi.sinst->slots;
        auto u32 = [](int64_t x) { return (uint32_t)(uint64_t)x; };
        slots["and"] = natv("bit.and", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::I((int64_t)(u32(inum(arg(a,0,"bit.and"),"bit.and")) & u32(inum(arg(a,1,"bit.and"),"bit.and")))); });
        slots["or"] = natv("bit.or", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::I((int64_t)(u32(inum(arg(a,0,"bit.or"),"bit.or")) | u32(inum(arg(a,1,"bit.or"),"bit.or")))); });
        slots["xor"] = natv("bit.xor", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::I((int64_t)(u32(inum(arg(a,0,"bit.xor"),"bit.xor")) ^ u32(inum(arg(a,1,"bit.xor"),"bit.xor")))); });
        slots["not"] = natv("bit.not", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::I((int64_t)(int32_t)(~u32(inum(arg(a,0,"bit.not"),"bit.not")))); });
        slots["shift"] = natv("bit.shift", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            uint32_t v = u32(inum(arg(a,0,"bit.shift"),"bit.shift"));
            int64_t n = inum(arg(a,1,"bit.shift"),"bit.shift");
            uint32_t r = n >= 0 ? (n >= 32 ? 0 : v << n) : (-n >= 32 ? 0 : v >> -n);
            return Value::I((int64_t)r); });
        slots["get"] = natv("bit.get", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            uint32_t v = u32(inum(arg(a,0,"bit.get"),"bit.get"));
            int64_t i = inum(arg(a,1,"bit.get"),"bit.get");            // 1-based
            return Value::B(i >= 1 && i <= 32 && ((v >> (i - 1)) & 1)); });
        slots["set"] = natv("bit.set", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            uint32_t v = u32(inum(arg(a,0,"bit.set"),"bit.set"));
            int64_t i = inum(arg(a,1,"bit.set"),"bit.set");
            bool on = arg(a,2,"bit.set").truthy();
            if (i >= 1 && i <= 32) { if (on) v |= 1u << (i - 1); else v &= ~(1u << (i - 1)); }
            return Value::I((int64_t)v); });
        slots["intasfloat"] = natv("bit.intAsFloat", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            uint32_t v = u32(inum(arg(a,0,"bit.intAsFloat"),"bit.intAsFloat"));
            float f2; std::memcpy(&f2, &v, 4); return Value::F(f2); });
        slots["floatasint"] = natv("bit.floatAsInt", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            float f2 = (float)dnum(arg(a,0,"bit.floatAsInt"),"bit.floatAsInt");
            uint32_t v; std::memcpy(&v, &f2, 4); return Value::I((int64_t)v); });
        slots["intashex"] = natv("bit.intAsHex", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            char buf[16]; snprintf(buf, sizeof buf, "%x", u32(inum(arg(a,0,"bit.intAsHex"),"bit.intAsHex")));
            return Value::S(buf); });
        slots["charasint"] = natv("bit.charAsInt", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            std::string& s = sstr(arg(a,0,"bit.charAsInt"),"bit.charAsInt");
            return Value::I(s.empty() ? 0 : (int64_t)(unsigned char)s[0]); });
        slots["intaschar"] = natv("bit.intAsChar", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::S(std::string(1, (char)inum(arg(a,0,"bit.intAsChar"),"bit.intAsChar"))); });
        // Max 8 globals inventory explicitly exposes these four bit members.
        // bit positions use the same one-based convention as bit.get/bit.set.
        slots["flip"] = natv("bit.flip", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            uint32_t v = u32(inum(arg(a,0,"bit.flip"),"bit.flip"));
            int64_t i = inum(arg(a,1,"bit.flip"),"bit.flip");
            if (i < 1 || i > 32) throw MxsError("bit.flip: bit index out of range (1..32)");
            return Value::I((int64_t)(v ^ (1u << (i-1)))); });
        slots["hexasint"] = natv("bit.hexAsInt", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            const std::string& raw = sstr(arg(a,0,"bit.hexAsInt"),"bit.hexAsInt");
            size_t i = (raw.size() >= 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) ? 2 : 0;
            if (i == raw.size() || raw.size()-i > 8) throw MxsError("bit.hexAsInt: 1..8 hexadecimal digits expected");
            uint32_t v = 0;
            for (; i < raw.size(); ++i) {
                const char c = raw[i];
                unsigned digit;
                if (c >= '0' && c <= '9') digit = (unsigned)(c-'0');
                else if (c >= 'a' && c <= 'f') digit = (unsigned)(c-'a'+10);
                else if (c >= 'A' && c <= 'F') digit = (unsigned)(c-'A'+10);
                else throw MxsError("bit.hexAsInt: invalid hexadecimal digit");
                v = (v << 4) | digit;
            }
            // Signed 32-bit view, matching MAXScript's Integer type.
            return Value::I((int64_t)(int32_t)v); });
        slots["isfinite"] = natv("bit.isFinite", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::B(std::isfinite(dnum(arg(a,0,"bit.isFinite"),"bit.isFinite"))); });
        slots["isnan"] = natv("bit.isNAN", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            return Value::B(std::isnan(dnum(arg(a,0,"bit.isNAN"),"bit.isNAN"))); });
        slots["swapbytes"] = natv("bit.swapBytes", [u32](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            uint32_t v = u32(inum(arg(a,0,"bit.swapBytes"),"bit.swapBytes"));
            int64_t i = inum(arg(a,1,"bit.swapBytes"),"bit.swapBytes");
            int64_t j = inum(arg(a,2,"bit.swapBytes"),"bit.swapBytes");
            if (i < 1 || i > 4 || j < 1 || j > 4) return Value::I((int64_t)v);
            uint8_t b[4]; std::memcpy(b, &v, 4); std::swap(b[i-1], b[j-1]); std::memcpy(&v, b, 4);
            return Value::I((int64_t)v); });
        g.set_local("bit", bi);
    }
    (void)g;
}

} // namespace mxs
