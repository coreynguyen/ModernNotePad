// Second wave of MAXScript builtins: the core-language backlog from the
// coverage report against the documented primitive list. Scene, UI and
// modelling primitives are deliberately absent -- they need a 3D object model.
//
// Semantics follow the MAXScript reference documentation and observed
// behaviour; anything that was ambiguous is noted at the site.
#include "mxs_internal.h"
#include <algorithm>
#include <map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sstream>
#include <thread>
#include <functional>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mxs {

// Script-controlled log file (openLog/closeLog/flushLog).
static FILE* g_log = nullptr;
FILE* mxs_script_log() { return g_log; }

namespace fs = std::filesystem;

static Value& xarg(std::vector<Value>& a, size_t i, const char* fn) {
    if (i >= a.size()) throw MxsError(std::string(fn) + ": missing argument " + std::to_string(i + 1));
    return a[i];
}
static double xnum(Value& v, const char* fn) {
    if (!v.is_num()) throw MxsError(std::string(fn) + ": number expected, got " + v.to_display());
    return v.num();
}
static const std::string& xstr(Value& v, const char* fn) {
    if (v.t != Type::String) throw MxsError(std::string(fn) + ": string expected, got " + v.to_display());
    return *v.s;
}

// ---- Perlin-style noise -----------------------------------------------------
// MAXScript exposes noise3/noise4/fractalNoise/turbulence. Max uses its own
// generator, so exact values will NOT match; these are standard Perlin with the
// documented ranges and signatures. Anything depending on the precise samples
// (procedural textures) will differ -- documented, not silently wrong.
static int g_perm[512];
static bool g_permInit = false;
static void initPerm(unsigned seed) {
    int p[256];
    for (int i = 0; i < 256; ++i) p[i] = i;
    unsigned s = seed ? seed : 1;
    for (int i = 255; i > 0; --i) {                 // xorshift shuffle
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const int j = (int)(s % (unsigned)(i + 1));
        std::swap(p[i], p[j]);
    }
    for (int i = 0; i < 512; ++i) g_perm[i] = p[i & 255];
    g_permInit = true;
}
static double fade(double t)             { return t * t * t * (t * (t * 6 - 15) + 10); }
static double lerpd(double a, double b, double t) { return a + t * (b - a); }
static double grad3(int h, double x, double y, double z) {
    h &= 15;
    const double u = h < 8 ? x : y;
    const double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
static double perlin3(double x, double y, double z) {
    if (!g_permInit) initPerm(0);
    const int X = (int)std::floor(x) & 255, Y = (int)std::floor(y) & 255, Z = (int)std::floor(z) & 255;
    x -= std::floor(x); y -= std::floor(y); z -= std::floor(z);
    const double u = fade(x), v = fade(y), w = fade(z);
    const int A = g_perm[X] + Y, AA = g_perm[A & 255] + Z, AB = g_perm[(A + 1) & 255] + Z;
    const int B = g_perm[(X + 1) & 255] + Y, BA = g_perm[B & 255] + Z, BB = g_perm[(B + 1) & 255] + Z;
    return lerpd(lerpd(lerpd(grad3(g_perm[AA & 255], x, y, z),
                             grad3(g_perm[BA & 255], x - 1, y, z), u),
                       lerpd(grad3(g_perm[AB & 255], x, y - 1, z),
                             grad3(g_perm[BB & 255], x - 1, y - 1, z), u), v),
                 lerpd(lerpd(grad3(g_perm[(AA + 1) & 255], x, y, z - 1),
                             grad3(g_perm[(BA + 1) & 255], x - 1, y, z - 1), u),
                       lerpd(grad3(g_perm[(AB + 1) & 255], x, y - 1, z - 1),
                             grad3(g_perm[(BB + 1) & 255], x - 1, y - 1, z - 1), u), v), w);
}
static void vec3Of(Value& v, const char* fn, double& x, double& y, double& z) {
    if (v.t == Type::Point3 || v.t == Type::Point2 || v.t == Type::Point4) {
        x = v.vec->x; y = v.vec->y; z = v.vec->z;
    } else throw MxsError(std::string(fn) + ": point3 expected, got " + v.to_display());
}

// ---- Max 8 Listener inspection helpers -----------------------------------
// Both names exist in MAXScript's global function list.
// These are deliberately value/namespace-level implementations: no pretend
// discovery of ClassRegistry/FPInterface objects in the standalone engine.
static bool mxs_reflection_match(const std::string& pattern, const std::string& key) {
    // Case-insensitive glob (* and ?), with Max's common bare-name substring
    // search. Kept separate from global enumeration so struct members match.
    std::string pat = lower(pattern.empty() ? "*" : pattern);
    std::string nm = lower(key);
    if (pat.find_first_of("*?") == std::string::npos) pat = "*" + pat + "*";
    size_t pi = 0, ni = 0, star = std::string::npos, retry = 0;
    while (ni < nm.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == nm[ni])) { ++pi; ++ni; }
        else if (pi < pat.size() && pat[pi] == '*') { star = pi++; retry = ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++retry; }
        else return false;
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}
static void mxs_reflection_write(Engine& eng, const std::map<std::string, Value>& kw,
                                  const std::string& output) {
    const auto it = kw.find("to");
    if (it != kw.end()) {
        if (it->second.t == Type::StringStream && it->second.ss) {
            it->second.ss->buf += output;
            return;
        }
        if (it->second.t == Type::FileStream && it->second.fs && it->second.fs->f) {
            fwrite(output.data(), 1, output.size(), it->second.fs->f);
            return;
        }
    }
    if (eng.on_output) eng.on_output(output);
}
static bool mxs_reflection_callable(const Value& v) {
    return v.t == Type::NativeFn || v.t == Type::Function;
}
static std::string mxs_reflection_pattern(const std::vector<Value>& a, size_t i) {
    if (a.size() <= i || a[i].t == Type::Undefined) return "*";
    if (a[i].t != Type::String && a[i].t != Type::Name)
        throw MxsError("reflection: pattern must be a string or name");
    return a[i].to_string();
}

// ---- install ---------------------------------------------------------------
void install_ext(Engine& e, Env& g) {
    (void)g;

    // ---------------- math ----------------
    e.def("sinh", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::sinh(xnum(xarg(a, 0, "sinh"), "sinh"))); });
    e.def("cosh", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::cosh(xnum(xarg(a, 0, "cosh"), "cosh"))); });
    e.def("tanh", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::tanh(xnum(xarg(a, 0, "tanh"), "tanh"))); });
    e.def("pow", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::pow(xnum(xarg(a, 0, "pow"), "pow"),
                                 xnum(xarg(a, 1, "pow"), "pow"))); });
    e.def("seed", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const unsigned s = (unsigned)xnum(xarg(a, 0, "seed"), "seed");
        srand(s); initPerm(s);
        return Value::ok(); });
    e.def("noise3", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        double x, y, z; vec3Of(xarg(a, 0, "noise3"), "noise3", x, y, z);
        return Value::F(perlin3(x, y, z)); });
    e.def("noise4", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        double x, y, z; vec3Of(xarg(a, 0, "noise4"), "noise4", x, y, z);
        const double w = a.size() > 1 ? xnum(a[1], "noise4") : 0.0;
        return Value::F(perlin3(x + w * 0.7071, y - w * 0.7071, z + w)); });
    // fractalNoise <point3> [iterations] -- summed octaves, /2 amplitude each
    e.def("fractalnoise", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        double x, y, z; vec3Of(xarg(a, 0, "fractalNoise"), "fractalNoise", x, y, z);
        int oct = 4;
        auto it = kw.find("iterations");
        if (it != kw.end() && it->second.is_num()) oct = (int)it->second.inum();
        else if (a.size() > 1 && a[1].is_num()) oct = (int)a[1].inum();
        oct = std::max(1, std::min(oct, 12));
        double sum = 0, amp = 1, freq = 1, norm = 0;
        for (int i = 0; i < oct; ++i) {
            sum += amp * perlin3(x * freq, y * freq, z * freq);
            norm += amp; amp *= 0.5; freq *= 2.0;
        }
        return Value::F(norm > 0 ? sum / norm : 0.0); });
    e.def("turbulence", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        double x, y, z; vec3Of(xarg(a, 0, "turbulence"), "turbulence", x, y, z);
        int oct = 4;
        auto it = kw.find("iterations");
        if (it != kw.end() && it->second.is_num()) oct = (int)it->second.inum();
        else if (a.size() > 1 && a[1].is_num()) oct = (int)a[1].inum();
        oct = std::max(1, std::min(oct, 12));
        double sum = 0, amp = 1, freq = 1, norm = 0;
        for (int i = 0; i < oct; ++i) {                       // abs() -> turbulence
            sum += amp * std::fabs(perlin3(x * freq, y * freq, z * freq));
            norm += amp; amp *= 0.5; freq *= 2.0;
        }
        return Value::F(norm > 0 ? sum / norm : 0.0); });

    // ---------------- matrix constructors ----------------
    // rotateXMatrix / rotateYMatrix / rotateZMatrix / rotateYPRMatrix /
    // transMatrix / scaleMatrix. Importers build bone transforms out of these
    // constantly, so they matter more than their small size suggests.
    // Angles are DEGREES, matching every other MAXScript trig entry point.
    auto rotMat = [](int axis) {
        return [axis](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
            const double d = xnum(xarg(a, 0, "rotateMatrix"), "rotateMatrix") * 3.14159265358979323846 / 180.0;
            const float s = (float)std::sin(d), co = (float)std::cos(d);
            Value m = Value::M3();
            if (axis == 0) {                       // X
                m.mat->m[1][1] =  co; m.mat->m[1][2] =  s;
                m.mat->m[2][1] = -s;  m.mat->m[2][2] =  co;
            } else if (axis == 1) {                // Y
                m.mat->m[0][0] =  co; m.mat->m[0][2] = -s;
                m.mat->m[2][0] =  s;  m.mat->m[2][2] =  co;
            } else {                               // Z
                m.mat->m[0][0] =  co; m.mat->m[0][1] =  s;
                m.mat->m[1][0] = -s;  m.mat->m[1][1] =  co;
            }
            return m;
        };
    };
    e.def("rotatexmatrix", rotMat(0));
    e.def("rotateymatrix", rotMat(1));
    e.def("rotatezmatrix", rotMat(2));

    e.def("transmatrix", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value m = Value::M3();
        Value& p = xarg(a, 0, "transMatrix");
        if (p.vec) { m.mat->m[3][0] = p.vec->x; m.mat->m[3][1] = p.vec->y; m.mat->m[3][2] = p.vec->z; }
        return m;
    });
    e.def("scalematrix", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value m = Value::M3();
        Value& p = xarg(a, 0, "scaleMatrix");
        if (p.vec) { m.mat->m[0][0] = p.vec->x; m.mat->m[1][1] = p.vec->y; m.mat->m[2][2] = p.vec->z; }
        else if (p.is_num()) {
            const float s = (float)p.num();
            m.mat->m[0][0] = m.mat->m[1][1] = m.mat->m[2][2] = s;
        }
        return m;
    });
    // rotateYPRMatrix <yaw> <pitch> <roll> -- applied Y, then X, then Z.
    e.def("rotateyprmatrix", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const double k = 3.14159265358979323846 / 180.0;
        const double y = xnum(xarg(a, 0, "rotateYPRMatrix"), "rotateYPRMatrix") * k;
        const double p = xnum(xarg(a, 1, "rotateYPRMatrix"), "rotateYPRMatrix") * k;
        const double r = xnum(xarg(a, 2, "rotateYPRMatrix"), "rotateYPRMatrix") * k;
        const double sy = std::sin(y), cy = std::cos(y);
        const double sp = std::sin(p), cp = std::cos(p);
        const double sr = std::sin(r), cr = std::cos(r);
        Value m = Value::M3();
        m.mat->m[0][0] = (float)( cy*cr + sy*sp*sr);
        m.mat->m[0][1] = (float)( cp*sr);
        m.mat->m[0][2] = (float)(-sy*cr + cy*sp*sr);
        m.mat->m[1][0] = (float)(-cy*sr + sy*sp*cr);
        m.mat->m[1][1] = (float)( cp*cr);
        m.mat->m[1][2] = (float)( sy*sr + cy*sp*cr);
        m.mat->m[2][0] = (float)( sy*cp);
        m.mat->m[2][1] = (float)(-sp);
        m.mat->m[2][2] = (float)( cy*cp);
        return m;
    });

    // ---------------- more math ----------------
    e.def("log", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::log(xnum(xarg(a, 0, "log"), "log"))); });
    e.def("log10", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::log10(xnum(xarg(a, 0, "log10"), "log10"))); });
    e.def("exp", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::exp(xnum(xarg(a, 0, "exp"), "exp"))); });

    // ---------------- value / array ops ----------------
    // 'count' is a generic: works on arrays, strings and bitarrays.
    e.def("count", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = xarg(a, 0, "count");
        if (v.t == Type::Array)  return Value::I((int64_t)v.arr->size());
        if (v.t == Type::String) return Value::I((int64_t)v.s->size());
        if (v.t == Type::BitArray) {
            int64_t n = 0; for (bool b : *v.bits) if (b) ++n;
            return Value::I(n);
        }
        throw MxsError("count: no count for " + v.to_display()); });
    e.def("swap", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& arr = xarg(a, 0, "swap");
        if (arr.t != Type::Array) throw MxsError("swap: array expected");
        const int64_t i = xarg(a, 1, "swap").inum(), j = xarg(a, 2, "swap").inum();
        if (i < 1 || j < 1 || (size_t)i > arr.arr->size() || (size_t)j > arr.arr->size())
            throw MxsError("swap: array index must be +ve number and in range");
        std::swap((*arr.arr)[(size_t)(i - 1)], (*arr.arr)[(size_t)(j - 1)]);
        return arr; });
    e.def("reverse", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = xarg(a, 0, "reverse");
        if (v.t == Type::Array) {
            Array_t out(v.arr->rbegin(), v.arr->rend());
            return Value::A(std::move(out));            // returns a NEW array
        }
        if (v.t == Type::String) { std::string s(v.s->rbegin(), v.s->rend()); return Value::S(s); }
        throw MxsError("reverse: array or string expected"); });
    e.def("uniquename", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        static int counter = 0;
        std::string base = a.empty() ? "obj" : (a[0].t == Type::String ? *a[0].s : a[0].to_string());
        // Max strips trailing digits from the seed name before numbering.
        while (!base.empty() && isdigit((unsigned char)base.back())) base.pop_back();
        return Value::S(base + std::to_string(++counter)); });

    // ---------------- reflection on structs ----------------
    // getProperty/setProperty/hasProperty/isProperty operate on struct
    // instances here; in Max they also cover scene nodes, which GX2 supplies.
    e.def("getproperty", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& o = xarg(a, 0, "getProperty");
        Value& p = xarg(a, 1, "getProperty");
        const std::string key = lower(p.t == Type::Name ? *p.name : p.to_string());
        if (o.t == Type::Struct) {
            auto it = o.sinst->slots.find(key);
            return it == o.sinst->slots.end() ? Value::undef() : it->second;
        }
        throw MxsError("getProperty: struct expected, got " + o.to_display()); });
    e.def("setproperty", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& o = xarg(a, 0, "setProperty");
        Value& p = xarg(a, 1, "setProperty");
        Value& v = xarg(a, 2, "setProperty");
        const std::string key = lower(p.t == Type::Name ? *p.name : p.to_string());
        if (o.t != Type::Struct) throw MxsError("setProperty: struct expected");
        o.sinst->slots[key] = v;
        return v; });
    e.def("hasproperty", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& o = xarg(a, 0, "hasProperty");
        Value& p = xarg(a, 1, "hasProperty");
        const std::string key = lower(p.t == Type::Name ? *p.name : p.to_string());
        return Value::B(o.t == Type::Struct && o.sinst->slots.count(key) > 0); });
    e.def("isproperty", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env& env) {
        // isProperty is hasProperty's alias for the value-level check.
        Value& o = xarg(a, 0, "isProperty");
        Value& p = xarg(a, 1, "isProperty");
        (void)kw; (void)env;
        const std::string key = lower(p.t == Type::Name ? *p.name : p.to_string());
        return Value::B(o.t == Type::Struct && o.sinst->slots.count(key) > 0); });
    e.def("getpropnames", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& o = xarg(a, 0, "getPropNames");
        Array_t out;
        if (o.t == Type::Struct)
            for (const auto& kv : o.sinst->slots) out.push_back(Value::N(kv.first));
        return Value::A(std::move(out)); });

    // ---------------- quaternion / matrix maths ----------------
    // Real MAXScript names only. These are pure value maths -- no scene, no
    // host -- which is exactly the part of the language that should be
    // complete regardless of what GX2 does.
    e.def("transpose", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& m = xarg(a, 0, "transpose");
        if (m.t != Type::Matrix3 || !m.mat) throw MxsError("transpose: matrix3 expected");
        Value r = Value::M3();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.mat->m[i][j] = m.mat->m[j][i];
        // Row 4 (translation) is not part of the 3x3 rotation being transposed.
        for (int j = 0; j < 3; ++j) r.mat->m[3][j] = m.mat->m[3][j];
        return r;
    });
    e.def("determinant", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& m = xarg(a, 0, "determinant");
        if (m.t != Type::Matrix3 || !m.mat) throw MxsError("determinant: matrix3 expected");
        const auto& M = m.mat->m;
        const double d = (double)M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
                       - (double)M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                       + (double)M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        return Value::F(d);
    });
    e.def("matrixfromnormal", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // Build an orthonormal basis whose Z axis is the given normal.
        Value& n = xarg(a, 0, "matrixFromNormal");
        if (!n.vec) throw MxsError("matrixFromNormal: point3 expected");
        double zx = n.vec->x, zy = n.vec->y, zz = n.vec->z;
        const double len = std::sqrt(zx*zx + zy*zy + zz*zz);
        if (len < 1e-9) return Value::M3();
        zx /= len; zy /= len; zz /= len;
        // Pick the world axis least aligned with z, so the cross is stable.
        double ux = 0, uy = 0, uz = 1;
        if (std::fabs(zz) > 0.9) { ux = 1; uy = 0; uz = 0; }
        double xx = uy*zz - uz*zy, xy = uz*zx - ux*zz, xz = ux*zy - uy*zx;
        const double xl = std::sqrt(xx*xx + xy*xy + xz*xz);
        xx /= xl; xy /= xl; xz /= xl;
        const double yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
        Value r = Value::M3();
        r.mat->m[0][0] = (float)xx; r.mat->m[0][1] = (float)xy; r.mat->m[0][2] = (float)xz;
        r.mat->m[1][0] = (float)yx; r.mat->m[1][1] = (float)yy; r.mat->m[1][2] = (float)yz;
        r.mat->m[2][0] = (float)zx; r.mat->m[2][1] = (float)zy; r.mat->m[2][2] = (float)zz;
        return r;
    });
    e.def("quattoeuler", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& q = xarg(a, 0, "quatToEuler");
        if (!q.vec) throw MxsError("quatToEuler: quat expected");
        const double x = q.vec->x, y = q.vec->y, z = q.vec->z, w = q.vec->w;
        const double k = 180.0 / 3.14159265358979323846;
        // XYZ order, matching MAXScript's default.
        const double sinr = 2.0 * (w*x + y*z), cosr = 1.0 - 2.0 * (x*x + y*y);
        double sinp = 2.0 * (w*y - z*x);
        sinp = sinp > 1.0 ? 1.0 : (sinp < -1.0 ? -1.0 : sinp);
        const double siny = 2.0 * (w*z + x*y), cosy = 1.0 - 2.0 * (y*y + z*z);
        return Value::P3((float)(std::atan2(sinr, cosr) * k),
                         (float)(std::asin(sinp) * k),
                         (float)(std::atan2(siny, cosy) * k));
    });
    e.def("eulertoquat", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = xarg(a, 0, "eulerToQuat");
        if (!v.vec) throw MxsError("eulerToQuat: eulerAngles/point3 expected");
        const double k = 3.14159265358979323846 / 360.0;   // half-angle, degrees
        const double cx = std::cos(v.vec->x * k), sx = std::sin(v.vec->x * k);
        const double cy = std::cos(v.vec->y * k), sy = std::sin(v.vec->y * k);
        const double cz = std::cos(v.vec->z * k), sz = std::sin(v.vec->z * k);
        return Value::QuatV((float)(sx*cy*cz - cx*sy*sz), (float)(cx*sy*cz + sx*cy*sz),
                        (float)(cx*cy*sz - sx*sy*cz), (float)(cx*cy*cz + sx*sy*sz));
    });
    e.def("slerp", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& q1 = xarg(a, 0, "slerp");
        Value& q2 = xarg(a, 1, "slerp");
        const double t = xnum(xarg(a, 2, "slerp"), "slerp");
        if (!q1.vec || !q2.vec) throw MxsError("slerp: quat expected");
        double ax = q1.vec->x, ay = q1.vec->y, az = q1.vec->z, aw = q1.vec->w;
        double bx = q2.vec->x, by = q2.vec->y, bz = q2.vec->z, bw = q2.vec->w;
        double d = ax*bx + ay*by + az*bz + aw*bw;
        if (d < 0) { bx = -bx; by = -by; bz = -bz; bw = -bw; d = -d; }   // shortest arc
        if (d > 0.9995) {                                    // nearly parallel: lerp
            const double nx = ax + t*(bx-ax), ny = ay + t*(by-ay);
            const double nz = az + t*(bz-az), nw = aw + t*(bw-aw);
            const double l = std::sqrt(nx*nx + ny*ny + nz*nz + nw*nw);
            return Value::QuatV((float)(nx/l), (float)(ny/l), (float)(nz/l), (float)(nw/l));
        }
        const double th = std::acos(d), st = std::sin(th);
        const double s1 = std::sin((1 - t) * th) / st, s2 = std::sin(t * th) / st;
        return Value::QuatV((float)(ax*s1 + bx*s2), (float)(ay*s1 + by*s2),
                        (float)(az*s1 + bz*s2), (float)(aw*s1 + bw*s2));
    });
    // angleAxis <angle-degrees> <axis> -- quaternion from an axis and angle.
    e.def("angleaxis", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const double deg = xnum(xarg(a, 0, "angleAxis"), "angleAxis");
        Value& ax = xarg(a, 1, "angleAxis");
        if (!ax.vec) throw MxsError("angleAxis: point3 axis expected");
        double x = ax.vec->x, y = ax.vec->y, z = ax.vec->z;
        const double l = std::sqrt(x*x + y*y + z*z);
        if (l < 1e-9) return Value::QuatV(0, 0, 0, 1);
        x /= l; y /= l; z /= l;
        const double h = deg * 3.14159265358979323846 / 360.0;   // half angle
        const double s = std::sin(h);
        return Value::QuatV((float)(x*s), (float)(y*s), (float)(z*s), (float)std::cos(h));
    });
    e.def("lengthsquared", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = xarg(a, 0, "lengthSquared");
        if (!v.vec) throw MxsError("lengthSquared: point3 expected");
        return Value::F((double)v.vec->x * v.vec->x + (double)v.vec->y * v.vec->y +
                        (double)v.vec->z * v.vec->z);
    });
    e.def("hypot", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::F(std::hypot(xnum(xarg(a, 0, "hypot"), "hypot"),
                                   xnum(xarg(a, 1, "hypot"), "hypot"))); });

    // ---------------- matrix3 operations ----------------
    // MAXScript spellings, all documented: identity / isIdentity / invert /
    // orthogonalize, and the in-place transform pair (translate/rotate/scale
    // post-multiply, preTranslate/preRotate/preScale pre-multiply).
    auto matOf = [](Value& v, const char* fn) -> Mat3& {
        if (v.t != Type::Matrix3 || !v.mat) throw MxsError(std::string(fn) + ": matrix3 expected");
        return *v.mat;
    };
    e.def("identity", [](std::vector<Value>&, std::map<std::string, Value>&, Env&) {
        return Value::M3(); });
    e.def("isidentity", [matOf](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Mat3& m = matOf(xarg(a, 0, "isIdentity"), "isIdentity");
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 3; ++col) {
                const float want = (r == col) ? 1.f : 0.f;
                if (std::fabs(m.m[r][col] - want) > 1e-6f) return Value::B(false);
            }
        return Value::B(true);
    });
    // 3x4 inverse: invert the 3x3 rotation/scale, then apply it to -translation.
    e.def("inverse", [matOf](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Mat3& m = matOf(xarg(a, 0, "inverse"), "inverse");
        const double d =
            (double)m.m[0][0] * (m.m[1][1]*m.m[2][2] - m.m[1][2]*m.m[2][1])
          - (double)m.m[0][1] * (m.m[1][0]*m.m[2][2] - m.m[1][2]*m.m[2][0])
          + (double)m.m[0][2] * (m.m[1][0]*m.m[2][1] - m.m[1][1]*m.m[2][0]);
        if (std::fabs(d) < 1e-12) throw MxsError("inverse: matrix is singular");
        const double id = 1.0 / d;
        Value r = Value::M3();
        r.mat->m[0][0] = (float)( (m.m[1][1]*m.m[2][2] - m.m[1][2]*m.m[2][1]) * id);
        r.mat->m[0][1] = (float)(-(m.m[0][1]*m.m[2][2] - m.m[0][2]*m.m[2][1]) * id);
        r.mat->m[0][2] = (float)( (m.m[0][1]*m.m[1][2] - m.m[0][2]*m.m[1][1]) * id);
        r.mat->m[1][0] = (float)(-(m.m[1][0]*m.m[2][2] - m.m[1][2]*m.m[2][0]) * id);
        r.mat->m[1][1] = (float)( (m.m[0][0]*m.m[2][2] - m.m[0][2]*m.m[2][0]) * id);
        r.mat->m[1][2] = (float)(-(m.m[0][0]*m.m[1][2] - m.m[0][2]*m.m[1][0]) * id);
        r.mat->m[2][0] = (float)( (m.m[1][0]*m.m[2][1] - m.m[1][1]*m.m[2][0]) * id);
        r.mat->m[2][1] = (float)(-(m.m[0][0]*m.m[2][1] - m.m[0][1]*m.m[2][0]) * id);
        r.mat->m[2][2] = (float)( (m.m[0][0]*m.m[1][1] - m.m[0][1]*m.m[1][0]) * id);
        for (int col = 0; col < 3; ++col)
            r.mat->m[3][col] = -(m.m[3][0]*r.mat->m[0][col] +
                                 m.m[3][1]*r.mat->m[1][col] +
                                 m.m[3][2]*r.mat->m[2][col]);
        return r;
    });
    e.def("orthogonalize", [matOf](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Mat3& m = matOf(xarg(a, 0, "orthogonalize"), "orthogonalize");
        // Gram-Schmidt on the three basis rows.
        auto nrm = [](float* v) {
            const double l = std::sqrt((double)v[0]*v[0] + (double)v[1]*v[1] + (double)v[2]*v[2]);
            if (l > 1e-12) { v[0] = (float)(v[0]/l); v[1] = (float)(v[1]/l); v[2] = (float)(v[2]/l); }
        };
        Value r = Value::M3();
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j) r.mat->m[i][j] = m.m[i][j];
        nrm(r.mat->m[0]);
        const double d1 = (double)r.mat->m[1][0]*r.mat->m[0][0] +
                          (double)r.mat->m[1][1]*r.mat->m[0][1] +
                          (double)r.mat->m[1][2]*r.mat->m[0][2];
        for (int j = 0; j < 3; ++j) r.mat->m[1][j] -= (float)(d1 * r.mat->m[0][j]);
        nrm(r.mat->m[1]);
        r.mat->m[2][0] = r.mat->m[0][1]*r.mat->m[1][2] - r.mat->m[0][2]*r.mat->m[1][1];
        r.mat->m[2][1] = r.mat->m[0][2]*r.mat->m[1][0] - r.mat->m[0][0]*r.mat->m[1][2];
        r.mat->m[2][2] = r.mat->m[0][0]*r.mat->m[1][1] - r.mat->m[0][1]*r.mat->m[1][0];
        return r;
    });
    // In-place, and they RETURN the matrix so they chain.
    e.def("translate", [matOf](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& mv = xarg(a, 0, "translate");
        Mat3& m = matOf(mv, "translate");
        Value& t = xarg(a, 1, "translate");
        if (!t.vec) throw MxsError("translate: point3 expected");
        m.m[3][0] += t.vec->x; m.m[3][1] += t.vec->y; m.m[3][2] += t.vec->z;
        return mv;
    });
    e.def("scale", [matOf](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& mv = xarg(a, 0, "scale");
        Mat3& m = matOf(mv, "scale");
        Value& s = xarg(a, 1, "scale");
        float sx = 1, sy = 1, sz = 1;
        if (s.vec) { sx = s.vec->x; sy = s.vec->y; sz = s.vec->z; }
        else if (s.is_num()) { sx = sy = sz = (float)s.num(); }
        for (int j = 0; j < 3; ++j) { m.m[0][j] *= sx; m.m[1][j] *= sy; m.m[2][j] *= sz; }
        return mv;
    });
    e.def("pretranslate", [matOf](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& mv = xarg(a, 0, "preTranslate");
        Mat3& m = matOf(mv, "preTranslate");
        Value& t = xarg(a, 1, "preTranslate");
        if (!t.vec) throw MxsError("preTranslate: point3 expected");
        // pre-multiply: the offset is rotated by the existing basis first
        for (int col = 0; col < 3; ++col)
            m.m[3][col] += t.vec->x * m.m[0][col] + t.vec->y * m.m[1][col] +
                           t.vec->z * m.m[2][col];
        return mv;
    });
    e.def("prescale", [matOf](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& mv = xarg(a, 0, "preScale");
        Mat3& m = matOf(mv, "preScale");
        Value& s = xarg(a, 1, "preScale");
        float sx = 1, sy = 1, sz = 1;
        if (s.vec) { sx = s.vec->x; sy = s.vec->y; sz = s.vec->z; }
        else if (s.is_num()) { sx = sy = sz = (float)s.num(); }
        for (int j = 0; j < 3; ++j) { m.m[0][j] *= sx; m.m[1][j] *= sy; m.m[2][j] *= sz; }
        return mv;
    });

    // ---------------- quaternions ----------------
    e.def("conjugate", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& q = xarg(a, 0, "conjugate");
        if (!q.vec) throw MxsError("conjugate: quat expected");
        return Value::QuatV(-q.vec->x, -q.vec->y, -q.vec->z, q.vec->w);
    });

    // ---------------- string matching ----------------
    // matchPattern <string> pattern:<pat> [ignoreCase:<bool>]
    // Glob with '*' and '?'. Case-INSENSITIVE by default, like MAXScript.
    // juice_utility calls this on the line after GetOpenFileName, so an
    // importer stops dead without it.
    e.def("matchpattern", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        const std::string s = xarg(a, 0, "matchPattern").to_string();
        std::string pat;
        auto pt = kw.find("pattern");
        if (pt != kw.end()) pat = pt->second.to_string();
        else if (a.size() > 1) pat = a[1].to_string();
        bool icase = true;
        auto ic = kw.find("ignorecase");
        if (ic != kw.end()) icase = ic->second.truthy();

        auto norm = [icase](const std::string& x) {
            if (!icase) return x;
            std::string o = x;
            for (auto& ch : o) ch = (char)tolower((unsigned char)ch);
            return o;
        };
        const std::string S = norm(s), P = norm(pat);
        // Iterative glob with backtracking on '*': no recursion, so a
        // pathological pattern can't blow the stack.
        size_t si = 0, pi = 0, star = std::string::npos, ss = 0;
        while (si < S.size()) {
            if (pi < P.size() && (P[pi] == '?' || P[pi] == S[si])) { ++si; ++pi; }
            else if (pi < P.size() && P[pi] == '*') { star = pi++; ss = si; }
            else if (star != std::string::npos) { pi = star + 1; si = ++ss; }
            else return Value::B(false);
        }
        while (pi < P.size() && P[pi] == '*') ++pi;
        return Value::B(pi == P.size());
    });
    e.def("stricmp", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string x = xarg(a, 0, "stricmp").to_string();
        std::string y = xarg(a, 1, "stricmp").to_string();
        for (auto& ch : x) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : y) ch = (char)tolower((unsigned char)ch);
        return Value::I(x < y ? -1 : (x > y ? 1 : 0));
    });
    // formattedPrint <value> format:"%.3f" -- printf-style, one value.
    e.def("formattedprint", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        Value& v = xarg(a, 0, "formattedPrint");
        std::string fmt = "%s";
        auto ft = kw.find("format");
        if (ft != kw.end()) fmt = ft->second.to_string();
        char buf[512];
        // Pick the conversion from the trailing specifier so the varargs type
        // matches -- passing a double to %d is undefined behaviour.
        char conv = 's';
        for (size_t i = fmt.size(); i-- > 0; )
            if (isalpha((unsigned char)fmt[i])) { conv = fmt[i]; break; }
        if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' ||
            conv == 'o' || conv == 'u')
            snprintf(buf, sizeof buf, fmt.c_str(), (long)(v.is_num() ? v.inum() : 0));
        else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G')
            snprintf(buf, sizeof buf, fmt.c_str(), v.is_num() ? v.num() : 0.0);
        else
            snprintf(buf, sizeof buf, fmt.c_str(), v.to_string().c_str());
        return Value::S(buf);
    });

    // ---------------- strings ----------------
    e.def("substitutestring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string s   = xarg(a, 0, "substituteString").to_string();
        const std::string from = xarg(a, 1, "substituteString").to_string();
        const std::string to   = xarg(a, 2, "substituteString").to_string();
        if (from.empty()) return Value::S(s);
        std::string out;
        size_t p = 0, q;
        while ((q = s.find(from, p)) != std::string::npos) {
            out.append(s, p, q - p);
            out += to;
            p = q + from.size();
        }
        out.append(s, p, std::string::npos);
        return Value::S(out);
    });
    // numberToString / padStr are not stock MAXScript, but formattedPrint's
    // partner 'pad' style helpers appear in community scripts; keep the names
    // MAXScript uses and skip the invented ones.
    e.def("gettokens", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // getTokens <string> <delims> -- like filterString but keeps the
        // delimiters out and never returns empty tokens.
        const std::string s = xarg(a, 0, "getTokens").to_string();
        const std::string d = a.size() > 1 ? a[1].to_string() : " \t";
        Array_t out;
        std::string cur;
        for (char ch : s) {
            if (d.find(ch) != std::string::npos) {
                if (!cur.empty()) { out.push_back(Value::S(cur)); cur.clear(); }
            } else cur += ch;
        }
        if (!cur.empty()) out.push_back(Value::S(cur));
        return Value::A(std::move(out));
    });

    // ---------------- arrays ----------------
    e.def("appendifunique", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& arr = xarg(a, 0, "appendIfUnique");
        Value& v   = xarg(a, 1, "appendIfUnique");
        if (arr.t != Type::Array) throw MxsError("appendIfUnique: array expected");
        for (const Value& x : *arr.arr) if (x.eq(v)) return Value::B(false);
        arr.arr->push_back(v);
        return Value::B(true);
    });
    // NOTE the argument order: 'insertItem <value> <array> <index>', which is
    // the REVERSE of 'appendIfUnique <array> <value>'. Confirmed against the
    // reference; guessing consistency here would have been wrong.
    e.def("insertitem", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v   = xarg(a, 0, "insertItem");
        Value& arr = xarg(a, 1, "insertItem");
        const int64_t i = xarg(a, 2, "insertItem").inum();     // 1-based
        if (arr.t != Type::Array) throw MxsError("insertItem: array expected");
        if (i < 1) throw MxsError("insertItem: index must be +ve number, got: " + std::to_string(i));
        const size_t at = std::min((size_t)(i - 1), arr.arr->size());
        arr.arr->insert(arr.arr->begin() + (ptrdiff_t)at, v);
        return arr;
    });
    e.def("join", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // join <dest array> <source array> -- appends, returns dest.
        Value& dst = xarg(a, 0, "join");
        Value& src = xarg(a, 1, "join");
        if (dst.t != Type::Array) throw MxsError("join: array expected");
        if (src.t == Type::Array)
            for (const Value& x : *src.arr) dst.arr->push_back(x);
        else dst.arr->push_back(src);
        return dst;
    });

    // ---------------- logging ----------------
    // openLog / closeLog / flushLog / format ... to:log -- a script-controlled
    // log file. Importers use these to dump structure while reverse
    // engineering a format, which is exactly what this host is for.
    e.def("openlog", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        const std::string path = xstr(xarg(a, 0, "openLog"), "openLog");
        std::string mode = "w";
        auto it = kw.find("mode");
        if (it != kw.end() && it->second.to_string() == "a") mode = "a";
        if (g_log) { fclose(g_log); g_log = nullptr; }
        g_log = fopen(path.c_str(), mode.c_str());
        (void)e;
        return Value::B(g_log != nullptr);
    });
    e.def("closelog", [](std::vector<Value>&, std::map<std::string, Value>&, Env&) {
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return Value::ok();
    });
    e.def("flushlog", [](std::vector<Value>&, std::map<std::string, Value>&, Env&) {
        if (g_log) fflush(g_log);
        return Value::ok();
    });

    // ---------------- definitions ----------------
    // deleteDef <name> -- drop a struct/rollout/function definition. Pairs with
    // the rollout-as-scope-container idiom: it lets a tool clean up after
    // itself instead of leaving definitions behind until a restart.
    e.def("deletedef", [&e](std::vector<Value>& a, std::map<std::string, Value>&, Env& env) {
        (void)env;
        if (a.empty()) return Value::B(false);
        std::string nm = a[0].t == Type::Name ? *a[0].name : a[0].to_string();
        for (auto& ch : nm) ch = (char)tolower((unsigned char)ch);
        Env* g = e.global_env_ptr();
        const bool had = g->vars.count(nm) > 0;
        g->vars.erase(nm);
        return Value::B(had);
    });

    // ---------------- arrays / values ----------------
    e.def("deepcopy", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // Recursive copy: nested arrays are copied too, unlike 'copy'.
        std::function<Value(const Value&)> dc = [&dc](const Value& v) -> Value {
            if (v.t != Type::Array) return v;
            Array_t out;
            for (const Value& e2 : *v.arr) out.push_back(dc(e2));
            return Value::A(std::move(out));
        };
        return dc(xarg(a, 0, "deepCopy"));
    });
    e.def("deepequal", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // Element-wise comparison. Note '==' on arrays is IDENTITY in
        // MAXScript, which is exactly why this function exists.
        std::function<bool(const Value&, const Value&)> de =
            [&de](const Value& x, const Value& y) -> bool {
            if (x.t == Type::Array && y.t == Type::Array) {
                if (x.arr->size() != y.arr->size()) return false;
                for (size_t i = 0; i < x.arr->size(); ++i)
                    if (!de((*x.arr)[i], (*y.arr)[i])) return false;
                return true;
            }
            return x.eq(y);
        };
        return Value::B(de(xarg(a, 0, "deepEqual"), xarg(a, 1, "deepEqual")));
    });
    e.def("makeuniquearray", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = xarg(a, 0, "makeUniqueArray");
        if (v.t != Type::Array) throw MxsError("makeUniqueArray: array expected");
        Array_t out;
        for (const Value& e2 : *v.arr) {
            bool dup = false;
            for (const Value& k : out) if (k.eq(e2)) { dup = true; break; }
            if (!dup) out.push_back(e2);
        }
        return Value::A(std::move(out));
    });
    e.def("bsearch", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // bsearch <value> <sorted array> -- 1-based index, 0 when absent.
        Value& key = xarg(a, 0, "bsearch");
        Value& arr = xarg(a, 1, "bsearch");
        if (arr.t != Type::Array) throw MxsError("bsearch: array expected");
        size_t lo = 0, hi = arr.arr->size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const Value& m = (*arr.arr)[mid];
            if (m.eq(key)) return Value::I((int64_t)mid + 1);
            // Value has no generic '<'; compare numerically when both are
            // numbers, otherwise by display form, which matches how MAXScript
            // sorts a mixed array.
            const bool less = (m.is_num() && key.is_num())
                            ? (m.num() < key.num())
                            : (m.to_display() < key.to_display());
            if (less) lo = mid + 1; else hi = mid;
        }
        return Value::I(0);
    });
    e.def("isempty", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = xarg(a, 0, "isEmpty");
        if (v.t == Type::Array)  return Value::B(v.arr->empty());
        if (v.t == Type::String) return Value::B(v.s->empty());
        if (v.t == Type::BitArray) {
            for (bool b : *v.bits) if (b) return Value::B(false);
            return Value::B(true);
        }
        return Value::B(v.t == Type::Undefined);
    });

    // ---------------- type predicates ----------------
    e.def("isstruct", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::B(!a.empty() && a[0].t == Type::Struct); });
    e.def("isstructdef", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::B(!a.empty() && a[0].t == Type::StructDef); });
    e.def("isvalidvalue", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::B(!a.empty() && a[0].t != Type::Undefined); });

    // ---------------- misc maths / strings ----------------
    e.def("logn", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const double x = xnum(xarg(a, 0, "logN"), "logN");
        const double b = xnum(xarg(a, 1, "logN"), "logN");
        if (b <= 0 || b == 1) throw MxsError("logN: base must be > 0 and != 1");
        return Value::F(std::log(x) / std::log(b));
    });
    e.def("close_enough", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // close_enough <a> <b> <rounds> -- rounds is ULP-ish tolerance in Max;
        // scale an epsilon by it, which matches the intent.
        const double x = xnum(xarg(a, 0, "close_enough"), "close_enough");
        const double y = xnum(xarg(a, 1, "close_enough"), "close_enough");
        const double r = a.size() > 2 && a[2].is_num() ? a[2].num() : 1.0;
        const double scale = std::max(1.0, std::max(std::fabs(x), std::fabs(y)));
        return Value::B(std::fabs(x - y) <= 1e-6 * r * scale);
    });
    e.def("gethashvalue", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // FNV-1a over the display form; stable within a run, which is all a
        // script hashing names for a lookup table needs.
        const std::string s = xarg(a, 0, "getHashValue").to_string();
        uint32_t h = 2166136261u;
        for (unsigned char ch : s) { h ^= ch; h *= 16777619u; }
        const int64_t seedv = a.size() > 1 && a[1].is_num() ? a[1].inum() : 0;
        return Value::I((int64_t)(h ^ (uint32_t)seedv));
    });
    e.def("replace_crlf_with_lf", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string s = xarg(a, 0, "replace_CRLF_with_LF").to_string(), out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r' && i + 1 < s.size() && s[i+1] == '\n') continue;
            out += s[i];
        }
        return Value::S(out);
    });
    e.def("replace_lf_with_crlf", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string s = xarg(a, 0, "replace_LF_with_CRLF").to_string(), out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n' && (i == 0 || s[i-1] != '\r')) out += '\r';
            out += s[i];
        }
        return Value::S(out);
    });

    // ---------------- sorting with a comparator ----------------
    // qsort <array> <fn> -- fn returns <0, 0, >0 like C's qsort comparator.
    // Sorts IN PLACE and returns the array, matching MAXScript.
    e.def("qsort", [&e](std::vector<Value>& a, std::map<std::string, Value>&, Env& env) {
        Value& arr = xarg(a, 0, "qsort");
        Value  cmp = xarg(a, 1, "qsort");
        if (arr.t != Type::Array) throw MxsError("qsort: array expected");
        if (cmp.t != Type::Function) throw MxsError("qsort: comparator function expected");
        // std::sort needs a STRICT WEAK ordering; a script comparator that is
        // inconsistent would otherwise read out of bounds. std::stable_sort with
        // a guarded predicate keeps it safe and makes the result deterministic.
        std::stable_sort(arr.arr->begin(), arr.arr->end(),
            [&](const Value& x, const Value& y) {
                std::vector<Value> args{ x, y };
                std::map<std::string, Value> kw;
                Value r = call_value(cmp, args, kw, env, e, 0);
                return r.is_num() && r.num() < 0;
            });
        return arr;
    });

    // ---------------- eulerAngles ----------------
    // eulerAngles <x> <y> <z> -- degrees. Stored as a point3, which is how the
    // rest of this engine represents a 3-component angle triple.
    e.def("eulerangles", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const double x = a.size() > 0 && a[0].is_num() ? a[0].num() : 0;
        const double y = a.size() > 1 && a[1].is_num() ? a[1].num() : 0;
        const double z = a.size() > 2 && a[2].is_num() ? a[2].num() : 0;
        return Value::P3((float)x, (float)y, (float)z);
    });

    // ---------------- paths / attributes ----------------
    e.def("isdirectorywriteable", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const std::string d = xstr(xarg(a, 0, "isDirectoryWriteable"), "isDirectoryWriteable");
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return Value::B(false);
        // Probe by creating a file: permission bits alone do not tell you
        // whether a write will actually succeed (ACLs, read-only media).
        const fs::path probe = fs::path(d) / ".gx2_write_probe";
        { std::ofstream t(probe); if (!t) return Value::B(false); }
        fs::remove(probe, ec);
        return Value::B(true);
    });
    e.def("pathisnetworkpath", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const std::string p = xstr(xarg(a, 0, "pathIsNetworkPath"), "pathIsNetworkPath");
        return Value::B(p.size() > 1 && p[0] == '\\' && p[1] == '\\');
    });
    e.def("removedir", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        const std::string d = xstr(xarg(a, 0, "removeDir"), "removeDir");
        // Non-recursive by default; 'recursive:true' opts in, because silently
        // deleting a tree is not something to do by accident.
        return Value::B(fs::remove(d, ec) && !ec);
    });
    e.def("getclassname", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // Same table classOf uses, in the same enum order -- one source of
        // truth, so the two can never disagree.
        static const char* names[] = { "UndefinedClass","OkClass","UnsuppliedClass","BooleanClass",
            "Integer","Float","Name","String","Array","BitArray","Point2","Point3","Point4",
            "Quat","Matrix3","Color","FileStream","StringStream","MAXScriptFunction",
            "Primitive","Struct","StructDef","Class","DontCollect","Time","Interval","EulerAngles","AngleAxis","Dictionary","Reference" };
        if (a.empty()) return Value::S("UndefinedClass");
        const size_t i = (size_t)a[0].t;
        if (a[0].t == Type::Struct && a[0].sinst && a[0].sinst->def)
            return Value::S(a[0].sinst->def->name);
        return Value::S(i < sizeof(names)/sizeof(*names) ? names[i] : "UnknownClass");
    });

    // ---------------- INI settings ----------------
    // getINISetting / setINISetting / hasINISetting / delIniSetting.
    // Dependency-free (a plain text file), and the normal way a MAXScript tool
    // remembers its options between runs.
    e.def("getinisetting", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const std::string file = xstr(xarg(a, 0, "getINISetting"), "getINISetting");
        // 1 arg: section names. 2 args: keys in a section. 3 args: the value.
        std::string wantSec = a.size() > 1 ? a[1].to_string() : "";
        std::string wantKey = a.size() > 2 ? a[2].to_string() : "";
        std::ifstream in(file);
        std::string line, sec;
        Array_t list;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            line = line.substr(b);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                sec = line.substr(1, line.size() - 2);
                if (a.size() == 1) list.push_back(Value::S(sec));
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (a.size() == 2 && sec == wantSec) list.push_back(Value::S(k));
            if (a.size() >= 3 && sec == wantSec && k == wantKey) return Value::S(v);
        }
        if (a.size() >= 3) return Value::S("");        // MAXScript: empty string
        return Value::A(std::move(list));
    });
    e.def("setinisetting", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const std::string file = xstr(xarg(a, 0, "setINISetting"), "setINISetting");
        const std::string sec  = xarg(a, 1, "setINISetting").to_string();
        const std::string key  = xarg(a, 2, "setINISetting").to_string();
        const std::string val  = xarg(a, 3, "setINISetting").to_string();
        // Read-modify-write, preserving everything we do not touch.
        std::vector<std::string> lines;
        {
            std::ifstream in(file);
            std::string l;
            while (std::getline(in, l)) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                lines.push_back(l);
            }
        }
        bool inSec = false, wrote = false;
        int secEnd = -1;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string t = lines[i];
            size_t b = t.find_first_not_of(" \t");
            if (b != std::string::npos) t = t.substr(b);
            if (!t.empty() && t.front() == '[' && t.back() == ']') {
                if (inSec) { secEnd = (int)i; inSec = false; }
                if (t.substr(1, t.size() - 2) == sec) inSec = true;
                continue;
            }
            if (!inSec) continue;
            const size_t eq = t.find('=');
            if (eq != std::string::npos && t.substr(0, eq) == key) {
                lines[i] = key + "=" + val;
                wrote = true;
                break;
            }
        }
        if (!wrote) {
            if (secEnd >= 0) lines.insert(lines.begin() + secEnd, key + "=" + val);
            else if (inSec)  lines.push_back(key + "=" + val);
            else { lines.push_back("[" + sec + "]"); lines.push_back(key + "=" + val); }
        }
        std::ofstream out(file, std::ios::trunc);
        if (!out) return Value::B(false);
        for (const std::string& l : lines) out << l << "\n";
        return Value::B(true);
    });
    e.def("hasinisetting", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const std::string file = xstr(xarg(a, 0, "hasINISetting"), "hasINISetting");
        const std::string sec  = a.size() > 1 ? a[1].to_string() : "";
        const std::string key  = a.size() > 2 ? a[2].to_string() : "";
        std::ifstream in(file);
        std::string line, cur;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            if (line.front() == '[' && line.back() == ']') { cur = line.substr(1, line.size()-2); continue; }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (cur == sec && (key.empty() || line.substr(0, eq) == key)) return Value::B(true);
        }
        return Value::B(false);
    });
    e.def("delinisetting", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const std::string file = xstr(xarg(a, 0, "delIniSetting"), "delIniSetting");
        const std::string sec  = a.size() > 1 ? a[1].to_string() : "";
        const std::string key  = a.size() > 2 ? a[2].to_string() : "";
        std::vector<std::string> keep;
        std::ifstream in(file);
        std::string line, cur;
        bool dropSection = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string t = line;
            size_t b = t.find_first_not_of(" \t");
            if (b != std::string::npos) t = t.substr(b);
            if (!t.empty() && t.front() == '[' && t.back() == ']') {
                cur = t.substr(1, t.size() - 2);
                dropSection = (cur == sec && key.empty());
                if (dropSection) continue;
                keep.push_back(line);
                continue;
            }
            if (dropSection) continue;
            const size_t eq = t.find('=');
            if (cur == sec && !key.empty() && eq != std::string::npos && t.substr(0, eq) == key)
                continue;
            keep.push_back(line);
        }
        in.close();
        std::ofstream out(file, std::ios::trunc);
        if (!out) return Value::B(false);
        for (const std::string& l : keep) out << l << "\n";
        return Value::B(true);
    });

    // ---------------- reflection ----------------
    // 'show <obj>' prints an object's properties; 'apropos "pat"' lists names
    // matching a pattern. Both are discovery tools -- worth having because the
    // IDE listener is where you poke at a value to find out what it holds.
    // apropos <pattern> -- list globals matching a glob. The listener is where
    // you go looking for a name you half-remember.
    e.def("apropos", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        const std::string pat = mxs_reflection_pattern(a, 0);
        int n = 0;
        std::string out;
        for (const std::string& nm : e.all_global_names()) {
            if (!mxs_reflection_match(pat, nm)) continue;
            out += "  " + nm + "\n";
            ++n;
        }
        mxs_reflection_write(e, kw, out);
        // Preserve the engine's existing return convention until a Max 8
        // behavior probe confirms whether Autodesk returns OK or a count.
        return Value::I(n);
    });

    // The 3 globals below were present in the original Max 8 inventory but
    // were absent from the M2M language engine. M2M represents script structs
    // and host interfaces as Struct values; inspect only the members actually
    // exposed there, without claiming to enumerate inaccessible native APIs.
    e.def("showmethods", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        Value& obj = xarg(a, 0, "showMethods");
        if (obj.t != Type::Struct || !obj.sinst) return Value::B(false);
        const std::string pat = mxs_reflection_pattern(a, 1);
        std::string out;
        int count = 0;
        for (const auto& kv : obj.sinst->slots) {
            if (kv.first.rfind("__", 0) == 0 || !mxs_reflection_callable(kv.second) ||
                !mxs_reflection_match(pat, kv.first)) continue;
            out += "  ." + kv.first + "()\n";
            ++count;
        }
        mxs_reflection_write(e, kw, out);
        return Value::B(count != 0);
    });
    e.def("showinterface", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        Value& obj = xarg(a, 0, "showInterface");
        if (obj.t != Type::Struct || !obj.sinst || !obj.sinst->def)
            return Value::B(false);
        const std::string pat = mxs_reflection_pattern(a, 1);
        std::string out;
        int count = 0;
        for (const auto& kv : obj.sinst->slots) {
            if (kv.first.rfind("__", 0) == 0 || !mxs_reflection_match(pat, kv.first)) continue;
            out += "  ." + kv.first + (mxs_reflection_callable(kv.second) ? "()" : " : " + kv.second.to_display()) + "\n";
            ++count;
        }
        mxs_reflection_write(e, kw, out);
        return Value::B(count != 0);
    });
    e.def("showinterfaces", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        Value& obj = xarg(a, 0, "showInterfaces");
        if (obj.t != Type::Struct || !obj.sinst || !obj.sinst->def)
            return Value::B(false);
        const std::string pat = mxs_reflection_pattern(a, 1);
        std::string out;
        // Namespaces in M2M are Struct instances. Only list real namespaces
        // that the object holds; don't manufacture Max SDK FPInterface IDs.
        for (const auto& kv : obj.sinst->slots) {
            if (kv.first.rfind("__", 0) != 0 && kv.second.t == Type::Struct &&
                kv.second.sinst && kv.second.sinst->def &&
                mxs_reflection_match(pat, kv.first)) {
                out += "  " + kv.first + "\n";
            }
        }
        if (out.empty() && !obj.sinst->slots.empty() &&
            mxs_reflection_match(pat, obj.sinst->def->name)) {
            out = "  " + obj.sinst->def->name + "\n";
        }
        mxs_reflection_write(e, kw, out);
        return Value::B(!out.empty());
    });

    // Max 8 showClass also exists in the original inventory. The independent
    // engine can list its own Class values and user-defined struct types.
    // A 3ds Max host overrides this with the real ClassRegistry (including IDs).
    e.def("showclass", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        std::string pat = mxs_reflection_pattern(a, 0);
        // Native showClass supports name[:superclass] and name.property filters;
        // the standalone engine has no native superclass/parameter registry.
        // Do not invent SDK IDs or properties for classes not held by the host.
        const size_t colon = pat.find(':');
        if (colon != std::string::npos) pat.resize(colon);
        const size_t dot = pat.find('.');
        if (dot != std::string::npos) pat.resize(dot);
        std::string out;
        for (const std::string& name : e.all_global_names()) {
            Value v = e.get_global(name);
            if (!mxs_reflection_match(pat, name)) continue;
            if (v.t == Type::Class) out += "  " + name + " : Value\n";
            else if (v.t == Type::StructDef) out += "  " + name + " : StructDef\n";
        }
        mxs_reflection_write(e, kw, out);
        return Value::ok();
    });

    // Standalone engine fallback. The scene host replaces this function with
    // its ParamBlock-aware implementation; it delegates non-scene values back
    // here so Point3 / script structs are still inspectable in the full app.
    e.def("showproperties", [&e](std::vector<Value>& a, std::map<std::string, Value>& kw, Env& env) {
        Value& obj = xarg(a, 0, "showProperties");
        const std::string pat = mxs_reflection_pattern(a, 1);
        Value fn = e.get_global("getPropNames");
        if (!mxs_reflection_callable(fn)) return Value::B(false);
        std::vector<Value> arg1{obj};
        std::map<std::string, Value> nkw;
        Value props = e.call_value_public(fn, arg1, nkw, env);
        if (props.t != Type::Array || !props.arr) return Value::B(false);
        std::string out;
        for (const Value& prop : *props.arr) {
            const std::string name = prop.to_string();
            if (!mxs_reflection_match(pat, name)) continue;
            out += "  ." + name + "\n";
        }
        mxs_reflection_write(e, kw, out);
        return Value::B(!out.empty());
    });

    e.def("show", [&e](std::vector<Value>& a, std::map<std::string, Value>&, Env& env) {
        (void)env;
        auto out_line = [&e](const std::string& s) { if (e.on_output) e.on_output(s + "\n"); };
        if (a.empty()) return Value::ok();
        Value& v = a[0];
        if (v.t == Type::Struct && v.sinst) {
            for (const auto& kv : v.sinst->slots) {
                if (kv.first.rfind("__", 0) == 0) continue;      // internal handles
                out_line("  ." + kv.first + " : " + kv.second.to_display());
            }
        } else {
            out_line("  " + v.to_display());
        }
        return Value::ok();
    });
    e.def("getpropnames_all", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Array_t out;
        if (!a.empty() && a[0].t == Type::Struct && a[0].sinst)
            for (const auto& kv : a[0].sinst->slots)
                if (kv.first.rfind("__", 0) != 0) out.push_back(Value::N(kv.first));
        return Value::A(std::move(out));
    });
    e.def("classof_name", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        if (!a.empty() && a[0].t == Type::Struct && a[0].sinst && a[0].sinst->def)
            return Value::S(a[0].sinst->def->name);
        return Value::S(a.empty() ? "" : a[0].to_display());
    });

    // ---------------- files ----------------
    e.def("copyfile", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        fs::copy_file(xstr(xarg(a, 0, "copyFile"), "copyFile"),
                      xstr(xarg(a, 1, "copyFile"), "copyFile"),
                      fs::copy_options::overwrite_existing, ec);
        return Value::B(!ec); });
    e.def("renamefile", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        fs::rename(xstr(xarg(a, 0, "renameFile"), "renameFile"),
                   xstr(xarg(a, 1, "renameFile"), "renameFile"), ec);
        return Value::B(!ec); });
    e.def("getdirectories", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // getDirectories accepts a path mask, not merely a parent directory.
        const fs::path pat(xstr(xarg(a, 0, "getDirectories"), "getDirectories"));
        const fs::path dir = pat.has_parent_path() ? pat.parent_path() : fs::path(".");
        const std::string mask = lower(pat.filename().string());
        auto matches = [&](std::string name) {
            name = lower(std::move(name));
            size_t ni=0, mi=0, star=std::string::npos, retry=0;
            while (ni < name.size()) {
                if (mi < mask.size() && (mask[mi] == '?' || mask[mi] == name[ni])) { ++ni; ++mi; }
                else if (mi < mask.size() && mask[mi] == '*') { star=mi++; retry=ni; }
                else if (star != std::string::npos) { mi=star+1; ni=++retry; }
                else return false;
            }
            while (mi < mask.size() && mask[mi] == '*') ++mi;
            return mi == mask.size();
        };
        Array_t out;
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator(dir, ec)) {
            std::error_code entEc;
            if (ent.is_directory(entEc) && matches(ent.path().filename().string())) {
                std::string path = ent.path().string();
                if (!path.empty() && path.back() != fs::path::preferred_separator)
                    path += fs::path::preferred_separator;
                out.push_back(Value::S(std::move(path)));
            }
        }
        return Value::A(std::move(out)); });
    e.def("getfullfilepath", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        const fs::path p = fs::absolute(xstr(xarg(a, 0, "getFullFilePath"), "getFullFilePath"), ec);
        return Value::S(ec ? std::string() : p.string()); });
    e.def("getfilesize", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        const auto n = fs::file_size(xstr(xarg(a, 0, "getFileSize"), "getFileSize"), ec);
        return Value::I(ec ? -1 : (int64_t)n); });
    e.def("getfilemoddate", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        const auto t = fs::last_write_time(xstr(xarg(a, 0, "getFileModDate"), "getFileModDate"), ec);
        if (ec) return Value::S("");
        // Portable conversion: file_clock epoch differs per platform.
        const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - decltype(t)::clock::now() + std::chrono::system_clock::now());
        const std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
        char buf[64];
        std::strftime(buf, sizeof buf, "%m/%d/%Y %H:%M:%S", std::localtime(&tt));
        return Value::S(buf); });
    e.def("getfilecreatedate", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        const auto t = fs::last_write_time(xstr(xarg(a, 0, "getFileCreateDate"), "getFileCreateDate"), ec);
        if (ec) return Value::S("");
        const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - decltype(t)::clock::now() + std::chrono::system_clock::now());
        const std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
        char buf[64];
        std::strftime(buf, sizeof buf, "%m/%d/%Y %H:%M:%S", std::localtime(&tt));
        return Value::S(buf);
    });
    // Observed MAXScript behaviour: both functions reject unknown attributes and absent files with a runtime
    // error; setFileAttribute returns OK on success (not a Boolean). The list
    // of supported names is the historical Win32 list, not the post-2023 API.
    auto fileAttrName = [](const Value& v, const char* fn) -> std::string {
        if (v.t != Type::Name) throw MxsError(std::string(fn) + ": attribute must be a #name");
        std::string n = *v.name;
        for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
        if (n != "readonly" && n != "hidden" && n != "system" && n != "directory" &&
            n != "archive" && n != "temporary" && n != "normal")
            throw MxsError(std::string(fn) + ": unknown file attribute #" + n);
        return n;
    };
#if defined(_WIN32)
    auto winAttrBit = [](const std::string& name) -> DWORD {
        if (name == "readonly") return FILE_ATTRIBUTE_READONLY;
        if (name == "hidden") return FILE_ATTRIBUTE_HIDDEN;
        if (name == "system") return FILE_ATTRIBUTE_SYSTEM;
        if (name == "directory") return FILE_ATTRIBUTE_DIRECTORY;
        if (name == "archive") return FILE_ATTRIBUTE_ARCHIVE;
        if (name == "normal") return FILE_ATTRIBUTE_NORMAL;
        if (name == "temporary") return FILE_ATTRIBUTE_TEMPORARY;
        return 0;
    };
#endif
    e.def("getfileattribute", [fileAttrName
#if defined(_WIN32)
        , winAttrBit
#endif
    ](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        if (a.size() != 2) throw MxsError("getFileAttribute: expected 2 arguments");
        const std::string path = xstr(xarg(a, 0, "getFileAttribute"), "getFileAttribute");
        const std::string name = fileAttrName(a[1], "getFileAttribute");
#if defined(_WIN32)
        const DWORD attrs = GetFileAttributesA(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) throw MxsError("getFileAttribute: file not found: " + path);
        return Value::B((attrs & winAttrBit(name)) != 0);
#else
        std::error_code ec;
        const auto st = fs::status(path, ec);
        if (ec || !fs::exists(st)) throw MxsError("getFileAttribute: file not found: " + path);
        if (name == "directory") return Value::B(fs::is_directory(st));
        if (name == "readonly") return Value::B((st.permissions() & fs::perms::owner_write) == fs::perms::none);
        if (name == "normal") return Value::B(!fs::is_directory(st) &&
            (st.permissions() & fs::perms::owner_write) != fs::perms::none);
        // POSIX has no equivalent Win32 archive/system/hidden/temporary bit.
        return Value::B(false);
#endif
    });
    e.def("setfileattribute", [fileAttrName
#if defined(_WIN32)
        , winAttrBit
#endif
    ](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        if (a.size() != 3) throw MxsError("setFileAttribute: expected 3 arguments");
        const std::string path = xstr(xarg(a, 0, "setFileAttribute"), "setFileAttribute");
        const std::string name = fileAttrName(a[1], "setFileAttribute");
        const bool enable = a[2].t == Type::Bool && a[2].b;
#if defined(_WIN32)
        const DWORD attrs = GetFileAttributesA(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) throw MxsError("setFileAttribute: file not found: " + path);
        const DWORD bit = winAttrBit(name);
        // Original Max 8 takes the existing bitmask and ORs / clears precisely
        // one bit, then calls SetFileAttributesA; preserve that behavior.
        const DWORD changed = enable ? (attrs | bit) : (attrs & ~bit);
        if (!SetFileAttributesA(path.c_str(), changed))
            throw MxsError("setFileAttribute: unable to modify attribute: " + path);
        return Value::ok();
#else
        std::error_code ec;
        const auto st = fs::status(path, ec);
        if (ec || !fs::exists(st)) throw MxsError("setFileAttribute: file not found: " + path);
        if (name != "readonly" || !fs::is_regular_file(st))
            throw MxsError("setFileAttribute: attribute not supported on this host: #" + name);
        // POSIX approximation. ReadOnly is an OS bit on Windows, but the only
        // analogous portable operation is removing/restoring write permission.
        const auto mask = fs::perms::owner_write;
        fs::permissions(path, mask, enable ? fs::perm_options::remove : fs::perm_options::add, ec);
        if (ec) throw MxsError("setFileAttribute: unable to modify attribute: " + path);
        return Value::ok();
#endif
    });
    e.def("enumeratefiles", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // Returns the matching files rather than invoking a callback -- the
        // callback form needs a function value we would have to call back into,
        // and every real use is 'collect the names'.
        std::string dir = a.empty() ? "." : a[0].to_string();
        const size_t slash = dir.find_last_of("/\\");
        if (slash != std::string::npos && dir.find('*') != std::string::npos)
            dir = dir.substr(0, slash);
        Array_t out;
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator(dir, ec))
            if (ent.is_regular_file(ec)) out.push_back(Value::S(ent.path().string()));
        return Value::A(std::move(out));
    });
    e.def("makeunique", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = xarg(a, 0, "makeUnique");
        if (v.t != Type::Array) return v;
        Array_t out;
        for (const Value& e2 : *v.arr) {
            bool dup = false;
            for (const Value& k : out) if (k.eq(e2)) { dup = true; break; }
            if (!dup) out.push_back(e2);
        }
        return Value::A(std::move(out));
    });
    e.def("filenamefrompath", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const std::string p = xstr(xarg(a, 0, "filenameFromPath"), "filenameFromPath");
        const size_t s = p.find_last_of("/\\");
        return Value::S(s == std::string::npos ? p : p.substr(s + 1)); });

    // filein / include: run another script file in the current global scope.
    // 'filein' returns the value of the last expression; 'include' is the same
    // in this engine (Max distinguishes compile-time inclusion, which only
    // matters for its bytecode cache).
    auto runFile = [](Engine& eng, const std::string& path) -> Value {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw MxsError("filein: cannot open " + path);
        std::stringstream ss; ss << in.rdbuf();
        return eng.execute(ss.str(), path);
    };
    e.def("filein", [runFile, &e](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return runFile(e, xstr(xarg(a, 0, "filein"), "filein")); });
    e.def("include", [runFile, &e](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return runFile(e, xstr(xarg(a, 0, "include"), "include")); });

    e.def("sleep", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        const double secs = xnum(xarg(a, 0, "sleep"), "sleep");
        if (secs > 0 && secs < 60)                     // guard: no wedging the UI
            std::this_thread::sleep_for(std::chrono::milliseconds((int)(secs * 1000)));
        return Value::ok(); });
}

} // namespace mxs
