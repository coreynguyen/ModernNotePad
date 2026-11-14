// Value types added for Max 8 conformance: Time, Interval, EulerAngles,
// AngleAxis, Dictionary (Max 2017+, harmless) and the '&var' Reference box,
// plus the rotation conventions shared by 'as' conversions and builtins.
#include "mxs_internal.h"
#include <cmath>
#include <cstdio>

namespace mxs {

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kD2R = kPi / 180.0;

// Time is kept in frames but snapped to whole ticks (160 per frame at MAX's
// default 30 fps), so 1f/3 * 3 does not drift away from 1f.
Value Value::T(double frames) {
    Value v; v.t = Type::Time;
    v.f = std::round(frames * 160.0) / 160.0;
    return v;
}
Value Value::Ival(double s, double e) {
    Value v; v.t = Type::Interval; v.vec = std::make_shared<Vec4>();
    v.vec->x = (float)(std::round(s * 160.0) / 160.0);
    v.vec->y = (float)(std::round(e * 160.0) / 160.0);
    return v;
}
Value Value::Euler(float x, float y, float z) {
    Value v; v.t = Type::EulerAngles; v.vec = std::make_shared<Vec4>();
    v.vec->x = x; v.vec->y = y; v.vec->z = z;
    return v;
}
Value Value::AngAx(float a, float x, float y, float z) {
    Value v; v.t = Type::AngleAxis; v.vec = std::make_shared<Vec4>();
    v.vec->x = x; v.vec->y = y; v.vec->z = z; v.vec->w = a;
    return v;
}

int Dict_t::find(const Value& k) const {
    for (size_t i = 0; i < items.size(); ++i) {
        const Value& a = items[i].first;
        if (a.eq(k)) return (int)i;
        // #name keys are case-insensitive; string keys are case-sensitive
        if (a.t == Type::String && k.t == Type::String && *a.s == *k.s) return (int)i;
    }
    return -1;
}

std::function<int(const Value&, const Value&)>& Engine::identity_hook() {
    static std::function<int(const Value&, const Value&)> h; return h;
}
std::function<bool(const Value&, std::string&)>& Engine::display_hook() {
    static std::function<bool(const Value&, std::string&)> h;
    return h;
}

Value deref(const Value& v) { return v.t == Type::Reference && v.ref ? v.ref->v : v; }

// ---------------------------------------------------------- out params ----
thread_local OutArgFrame* g_out_frame = nullptr;

bool set_out_arg(size_t index, const Value& v) {
    if (!g_out_frame || index >= g_out_frame->pos.size() || !g_out_frame->pos[index]) return false;
    g_out_frame->pos[index]->v = v;
    g_out_frame->pos[index]->dirty = true;
    return true;
}
bool set_out_kwarg(const std::string& lname, const Value& v) {
    if (!g_out_frame) return false;
    auto it = g_out_frame->kw.find(lname);
    if (it == g_out_frame->kw.end() || !it->second) return false;
    it->second->v = v;
    it->second->dirty = true;
    return true;
}
bool is_out_arg(size_t index) {
    return g_out_frame && index < g_out_frame->pos.size() && g_out_frame->pos[index] != nullptr;
}

// ------------------------------------------------------------ printing ----
static std::string gnum(double d) {
    if (std::fabs(d) < 1e15 && d == (double)(int64_t)d) {   // [auditA] range first
        char b[40]; snprintf(b, sizeof b, "%lld", (long long)d); return b;
    }
    char b[40]; snprintf(b, sizeof b, "%g", d); return b;
}
std::string time_str(double frames) { return gnum(frames) + "f"; }

std::string ext_to_string(const Value& v) {
    switch (v.t) {
        case Type::Time:        return time_str(v.f);
        case Type::Interval:    return "(interval " + time_str(v.vec->x) + " " + time_str(v.vec->y) + ")";
        case Type::EulerAngles: return "(eulerAngles " + gnum(v.vec->x) + " " + gnum(v.vec->y) + " " + gnum(v.vec->z) + ")";
        case Type::AngleAxis:   return "(angleAxis " + gnum(v.vec->w) + " [" + gnum(v.vec->x) + "," + gnum(v.vec->y) + "," + gnum(v.vec->z) + "])";
        case Type::Dictionary: {
            CycleGuard cg(v.dict.get());   // [auditA]
            if (cg.cyclic) return "Dictionary #" + v.dict->keyType + " (...)";
            std::string o = "Dictionary #" + v.dict->keyType + " (";
            bool first = true;
            for (auto& kv : v.dict->items) {
                if (!first) o += ", ";
                first = false;
                o += "#(" + kv.first.to_display() + ", " + kv.second.to_display() + ")";
            }
            return o + ")";
        }
        case Type::Reference:   return v.ref ? v.ref->v.to_display() : "undefined";
        default: return "?";
    }
}

// ----------------------------------------------------------- rotations ----
static void mat_mul3(const Mat3& a, const Mat3& b, Mat3& o) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    for (int j = 0; j < 3; ++j) r.m[3][j] = 0;
    o = r;
}
static void axis_mat(int axis, double deg, Mat3& m) {
    m = Mat3();
    float c = (float)std::cos(deg * kD2R), s = (float)std::sin(deg * kD2R);
    // exact quarter turns: no 6e-17 residue in (eulerAngles 0 0 90) as matrix3
    const double q = deg / 90.0;
    if (q == std::floor(q)) {
        const int k = (((int)std::fmod(q, 4.0)) + 4) % 4;
        c = k == 0 ? 1.f : k == 2 ? -1.f : 0.f;
        s = k == 1 ? 1.f : k == 3 ? -1.f : 0.f;
    }
    // MAX's rotateX/Y/ZMatrix (row-vector): rotateZMatrix 90 maps x to +y.
    if (axis == 0) { m.m[1][1] = c; m.m[1][2] = s; m.m[2][1] = -s; m.m[2][2] = c; }
    if (axis == 1) { m.m[0][0] = c; m.m[0][2] = -s; m.m[2][0] = s; m.m[2][2] = c; }
    if (axis == 2) { m.m[0][0] = c; m.m[0][1] = s; m.m[1][0] = -s; m.m[1][1] = c; }
}
void rot_from_euler(double x, double y, double z, Mat3& out) {
    Mat3 a, b, c, ab;
    axis_mat(0, x, a); axis_mat(1, y, b); axis_mat(2, z, c);
    mat_mul3(a, b, ab); mat_mul3(ab, c, out);
}
void rot_to_euler(const Mat3& m0, double& x, double& y, double& z) {
    // normalise the rows so scaled matrices decompose too
    Mat3 m = m0;
    for (int r = 0; r < 3; ++r) {
        double l = std::sqrt((double)m.m[r][0] * m.m[r][0] + (double)m.m[r][1] * m.m[r][1] + (double)m.m[r][2] * m.m[r][2]);
        if (l > 1e-12) for (int c = 0; c < 3; ++c) m.m[r][c] = (float)(m.m[r][c] / l);
    }
    double sb = -m.m[0][2];
    sb = sb > 1 ? 1 : sb < -1 ? -1 : sb;
    double b = std::asin(sb), a, g;
    if (std::fabs(sb) < 0.99999) {
        a = std::atan2(m.m[1][2], m.m[2][2]);
        g = std::atan2(m.m[0][1], m.m[0][0]);
    } else {                                   // gimbal lock: fold z into x
        g = 0;
        a = std::atan2(m.m[1][0] / sb, m.m[1][1]);
    }
    x = a / kD2R; y = b / kD2R; z = g / kD2R;
    auto clean = [](double& d) { if (std::fabs(d) < 1e-9) d = 0; };
    clean(x); clean(y); clean(z);
}
// Standard (right-handed) quaternion -> row-vector rotation matrix.
static void std_quat_mat(double x, double y, double z, double w, Mat3& m) {
    m = Mat3();
    m.m[0][0] = (float)(1 - 2*(y*y + z*z)); m.m[0][1] = (float)(2*(x*y + z*w));     m.m[0][2] = (float)(2*(x*z - y*w));
    m.m[1][0] = (float)(2*(x*y - z*w));     m.m[1][1] = (float)(1 - 2*(x*x + z*z)); m.m[1][2] = (float)(2*(y*z + x*w));
    m.m[2][0] = (float)(2*(x*z + y*w));     m.m[2][1] = (float)(2*(y*z - x*w));     m.m[2][2] = (float)(1 - 2*(x*x + y*y));
}
void rot_from_quat(const Vec4& q, Mat3& out) {
    double l = std::sqrt((double)q.x*q.x + (double)q.y*q.y + (double)q.z*q.z + (double)q.w*q.w);
    if (l < 1e-12) { out = Mat3(); return; }
    // MAX quats are left-handed: the MXS quat is the conjugate of the
    // standard quat for the same rotation.
    std_quat_mat(-q.x / l, -q.y / l, -q.z / l, q.w / l, out);
}
Vec4 rot_to_quat(const Mat3& m) {
    // standard quat from the row-vector matrix, then conjugate
    const double t = (double)m.m[0][0] + m.m[1][1] + m.m[2][2];
    double x, y, z, w;
    if (t > 0) {
        double s = std::sqrt(t + 1.0) * 2;
        w = 0.25 * s;
        x = (m.m[1][2] - m.m[2][1]) / s; y = (m.m[2][0] - m.m[0][2]) / s; z = (m.m[0][1] - m.m[1][0]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        double s = std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        w = (m.m[1][2] - m.m[2][1]) / s; x = 0.25 * s;
        y = (m.m[0][1] + m.m[1][0]) / s; z = (m.m[2][0] + m.m[0][2]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        double s = std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        w = (m.m[2][0] - m.m[0][2]) / s; x = (m.m[0][1] + m.m[1][0]) / s;
        y = 0.25 * s; z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        double s = std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        w = (m.m[0][1] - m.m[1][0]) / s; x = (m.m[2][0] + m.m[0][2]) / s;
        y = (m.m[1][2] + m.m[2][1]) / s; z = 0.25 * s;
    }
    if (w < 0) { x = -x; y = -y; z = -z; w = -w; }
    auto c = [](double d) { return (float)(std::fabs(d) < 1e-7 ? 0.0 : d); };
    Vec4 q; q.x = c(-x); q.y = c(-y); q.z = c(-z); q.w = c(w);
    return q;
}
Vec4 quat_from_angleaxis(double deg, double ax, double ay, double az) {
    Vec4 q; q.w = 1;
    double l = std::sqrt(ax*ax + ay*ay + az*az);
    if (l < 1e-12) return q;
    double h = deg * kD2R / 2, s = std::sin(h);
    q.x = (float)(ax / l * s); q.y = (float)(ay / l * s); q.z = (float)(az / l * s); q.w = (float)std::cos(h);
    return q;
}
void quat_to_angleaxis(const Vec4& q0, double& deg, double& ax, double& ay, double& az) {
    double x = q0.x, y = q0.y, z = q0.z, w = q0.w;
    double l = std::sqrt(x*x + y*y + z*z + w*w);
    if (l > 1e-12) { x /= l; y /= l; z /= l; w /= l; }
    w = w > 1 ? 1 : w < -1 ? -1 : w;
    double s = std::sqrt(1 - w*w);
    deg = 2 * std::acos(w) / kD2R;
    if (s < 1e-9) { ax = 0; ay = 0; az = 1; deg = 0; return; }
    ax = x / s; ay = y / s; az = z / s;
}
bool rotation_of(const Value& v, Mat3& out) {
    switch (v.t) {
        case Type::Quat: rot_from_quat(*v.vec, out); return true;
        case Type::EulerAngles: rot_from_euler(v.vec->x, v.vec->y, v.vec->z, out); return true;
        case Type::AngleAxis: {
            Vec4 q = quat_from_angleaxis(v.vec->w, v.vec->x, v.vec->y, v.vec->z);
            rot_from_quat(q, out); return true;
        }
        case Type::Matrix3: out = *v.mat; for (int j = 0; j < 3; ++j) out.m[3][j] = 0; return true;
        default: return false;
    }
}

} // namespace mxs
