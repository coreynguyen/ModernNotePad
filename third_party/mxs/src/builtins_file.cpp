// File I/O — the surface community importers live on:
//   fopen/fclose/fseek/ftell, readByte/Short/Long/LongLong/Float/Double [#unsigned],
//   readString (NUL-terminated), readFixedString, readLine, write* mirrors,
//   getFilenamePath/File/Type, doesFileExist, getFileSize, getFiles.
// Plus math3d constructors: point2/3/4 via [..] handled at parse? no — MXS uses
// [x,y,z] literal syntax; we expose point3 x y z ctor style AND [..] via bracket?
// MXS point literal is [1,2,3]. Our parser reserves [] for indexing; provide
// point2/point3/point4/quat/matrix3/color constructor functions (importers use both;
// [..] literal support is a listed TODO in docs/MAXSCRIPT.md).
#include "mxs_internal.h"
#include <cstring>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace mxs {

static Value& arg(std::vector<Value>& a, size_t i, const char* fn) {
    if (i >= a.size()) throw MxsError(std::string(fn) + ": missing argument " + std::to_string(i + 1));
    return a[i];
}
static FILE* file_of(Value& v, const char* fn) {
    if (v.t != Type::FileStream || !v.fs->f)
        throw MxsError(std::string(fn) + ": open FileStream expected, got " + v.to_display());
    return v.fs->f;
}
static std::string& sstr(Value& v, const char* fn) {
    if (v.t != Type::String) throw MxsError(std::string(fn) + ": string expected, got " + v.to_display());
    return *v.s;
}
static bool want_unsigned(std::vector<Value>& a, std::map<std::string, Value>& kw) {
    for (auto& v : a) if (v.t == Type::Name && lower(*v.name) == "unsigned") return true;
    auto it = kw.find("unsigned"); // tolerate keyword form
    return it != kw.end() && it->second.truthy();
}

template <typename T>
static Value read_scalar(FILE* f, bool uns, const char* fn) {
    T v{};
    if (fread(&v, sizeof(T), 1, f) != 1) throw MxsError(std::string(fn) + ": read past end of file");
    if constexpr (std::is_integral_v<T>) {
        if (uns) return Value::I((int64_t)(std::make_unsigned_t<T>)v);
        return Value::I((int64_t)(std::make_signed_t<T>)v);
    } else {
        return Value::F((double)v);
    }
}
template <typename T>
static Value write_scalar(FILE* f, Value& v, const char* fn) {
    T x;
    if constexpr (std::is_integral_v<T>) x = (T)v.inum(); else x = (T)v.num();
    if (fwrite(&x, sizeof(T), 1, f) != 1) throw MxsError(std::string(fn) + ": write failed");
    return Value::ok();
}

void install_file(Engine& e, Env&) {
    e.def("fopen", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::string& path = sstr(arg(a, 0, "fopen"), "fopen");
        std::string mode = a.size() > 1 ? sstr(a[1], "fopen") : "rb";
        FILE* f = fopen(path.c_str(), mode.c_str());
        if (!f) return Value::undef();                              // MXS returns undefined on failure
        Value v; v.t = Type::FileStream;
        v.fs = std::make_shared<FileStream_t>();
        v.fs->f = f; v.fs->path = path; v.fs->mode = mode;
        return v;
    });
    e.def("fclose", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = arg(a, 0, "fclose");
        if (v.t == Type::FileStream && v.fs->f) { fclose(v.fs->f); v.fs->f = nullptr; }
        return Value::B(true);
    });
    e.def("fseek", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "fseek"), "fseek");
        int64_t off = arg(a, 1, "fseek").inum();
        int whence = SEEK_SET;
        if (a.size() > 2 && a[2].t == Type::Name) {
            std::string w = lower(*a[2].name);
            whence = w == "seek_cur" ? SEEK_CUR : w == "seek_end" ? SEEK_END : SEEK_SET;
        }
        return Value::B(fseek(f, (long)off, whence) == 0);
    });
    e.def("ftell", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::I(ftell(file_of(arg(a, 0, "ftell"), "ftell"))); });
    e.def("filepos", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::I(ftell(file_of(arg(a, 0, "filePos"), "filePos"))); });
    e.def("eof", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "eof"), "eof");
        long cur = ftell(f); fseek(f, 0, SEEK_END);
        long endp = ftell(f); fseek(f, cur, SEEK_SET);
        return Value::B(cur >= endp);
    });

    e.def("readbyte",  [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        return read_scalar<int8_t >(file_of(arg(a,0,"readByte"),"readByte"),  want_unsigned(a,kw), "readByte"); });
    e.def("readshort", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        return read_scalar<int16_t>(file_of(arg(a,0,"readShort"),"readShort"),want_unsigned(a,kw), "readShort"); });
    e.def("readlong",  [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        return read_scalar<int32_t>(file_of(arg(a,0,"readLong"),"readLong"),  want_unsigned(a,kw), "readLong"); });
    e.def("readlonglong", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        return read_scalar<int64_t>(file_of(arg(a,0,"readLongLong"),"readLongLong"), want_unsigned(a,kw), "readLongLong"); });
    e.def("readfloat", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return read_scalar<float >(file_of(arg(a,0,"readFloat"),"readFloat"), false, "readFloat"); });
    e.def("readdouble",[](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return read_scalar<double>(file_of(arg(a,0,"readDouble"),"readDouble"), false, "readDouble"); });
    e.def("readstring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "readString"), "readString");
        std::string out; int c;
        while ((c = fgetc(f)) != EOF && c != 0) out += (char)c;
        return Value::S(std::move(out));
    });
    // readChars <stream> <count> [errorAtEOF:] -- exactly N characters.
    // openFile / createFile / close -- MAXScript's own spellings alongside the
    // C-style fopen/fclose this engine already had. openFile defaults to text
    // read; createFile truncates for writing.
    e.def("openfile", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        const std::string path = arg(a, 0, "openFile").to_string();
        std::string mode = "rb";
        auto it = kw.find("mode");
        if (it != kw.end()) mode = it->second.to_string();
        else if (a.size() > 1 && a[1].t == Type::String) mode = *a[1].s;
        FILE* f = fopen(path.c_str(), mode.c_str());
        if (!f) return Value::undef();               // MAXScript: undefined
        Value v; v.t = Type::FileStream;
        v.fs = std::make_shared<FileStream_t>();
        v.fs->f = f; v.fs->path = path; v.fs->mode = mode;
        return v;
    });
    e.def("createfile", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        const std::string path = arg(a, 0, "createFile").to_string();
        std::string mode = "wb";
        auto it = kw.find("mode");
        if (it != kw.end()) mode = it->second.to_string();
        FILE* f = fopen(path.c_str(), mode.c_str());
        if (!f) return Value::undef();
        Value v; v.t = Type::FileStream;
        v.fs = std::make_shared<FileStream_t>();
        v.fs->f = f; v.fs->path = path; v.fs->mode = mode;
        return v;
    });
    // close <stream> -- works on a file stream; harmless on anything else, so
    // 'close someRollout' in a UI script does not abort.
    e.def("close", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        if (!a.empty() && a[0].t == Type::FileStream && a[0].fs && a[0].fs->f) {
            fclose(a[0].fs->f);
            a[0].fs->f = nullptr;
        }
        return Value::ok();
    });
    e.def("readchars", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        FILE* f = file_of(arg(a, 0, "readChars"), "readChars");
        const int64_t n = arg(a, 1, "readChars").inum();
        if (n < 0) throw MxsError("readChars: count must be >= 0");
        std::string out;
        out.resize((size_t)n);
        const size_t got = n ? fread(&out[0], 1, (size_t)n, f) : 0;
        bool errAtEof = false;
        auto it = kw.find("erroratoef");
        if (it == kw.end()) it = kw.find("erroratoeof");
        if (it != kw.end()) errAtEof = it->second.truthy();
        if (got < (size_t)n) {
            if (errAtEof) throw MxsError("readChars: unexpected end of file");
            out.resize(got);
        }
        return Value::S(std::move(out));
    });
    // readDelimitedString <stream> <delim> -- reads up to (and consumes) the
    // first delimiter character; the delimiter itself is not returned.
    e.def("readdelimitedstring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "readDelimitedString"), "readDelimitedString");
        Value& d = arg(a, 1, "readDelimitedString");
        const std::string delim = d.t == Type::String ? *d.s : d.to_string();
        std::string out;
        int ch;
        while ((ch = fgetc(f)) != EOF) {
            if (!delim.empty() && delim.find((char)ch) != std::string::npos) break;
            out += (char)ch;
        }
        return Value::S(std::move(out));
    });
    // skipToNextLine <stream> -- consume through the next newline.
    e.def("skiptonextline", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "skipToNextLine"), "skipToNextLine");
        int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') {}
        return Value::ok();
    });
    // readValue <stream> -- read one whitespace-delimited token and convert it
    // the way MAXScript would: integer, float, or string.
    e.def("readvalue", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "readValue"), "readValue");
        int ch;
        while ((ch = fgetc(f)) != EOF && isspace(ch)) {}
        if (ch == EOF) return Value::undef();
        std::string tok(1, (char)ch);
        while ((ch = fgetc(f)) != EOF && !isspace(ch)) tok += (char)ch;
        const char* p = tok.c_str();
        char* end = nullptr;
        const double num = strtod(p, &end);
        if (end != p && *end == 0)
            return tok.find_first_of(".eE") == std::string::npos
                 ? Value::I((int64_t)num) : Value::F(num);
        return Value::S(tok);
    });

    e.def("readfixedstring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "readFixedString"), "readFixedString");
        int64_t n = arg(a, 1, "readFixedString").inum();
        std::string out(n > 0 ? (size_t)n : 0, '\0');
        size_t got = n > 0 ? fread(out.data(), 1, (size_t)n, f) : 0;
        out.resize(got);
        if (auto p = out.find('\0'); p != std::string::npos) out.resize(p);   // MXS trims at NUL
        return Value::S(std::move(out));
    });
    e.def("readline", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "readLine"), "readLine");
        std::string out; int c;
        bool any = false;
        while ((c = fgetc(f)) != EOF) { any = true; if (c == '\n') break; if (c != '\r') out += (char)c; }
        if (!any) throw MxsError("readLine: read past end of file");
        return Value::S(std::move(out));
    });
    e.def("skiptostring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "skipToString"), "skipToString");
        std::string& pat = sstr(arg(a, 1, "skipToString"), "skipToString");
        if (pat.empty()) return Value::B(true);
        size_t m = 0; int c;
        while ((c = fgetc(f)) != EOF) {
            m = (char)c == pat[m] ? m + 1 : ((char)c == pat[0] ? 1 : 0);
            if (m == pat.size()) return Value::B(true);
        }
        return Value::undef();
    });

    e.def("writebyte",  [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return write_scalar<int8_t >(file_of(arg(a,0,"writeByte"),"writeByte"),  arg(a,1,"writeByte"),  "writeByte"); });
    e.def("writeshort", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return write_scalar<int16_t>(file_of(arg(a,0,"writeShort"),"writeShort"),arg(a,1,"writeShort"), "writeShort"); });
    e.def("writelong",  [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return write_scalar<int32_t>(file_of(arg(a,0,"writeLong"),"writeLong"),  arg(a,1,"writeLong"),  "writeLong"); });
    e.def("writelonglong", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return write_scalar<int64_t>(file_of(arg(a,0,"writeLongLong"),"writeLongLong"), arg(a,1,"writeLongLong"), "writeLongLong"); });
    e.def("writefloat", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return write_scalar<float>(file_of(arg(a,0,"writeFloat"),"writeFloat"), arg(a,1,"writeFloat"), "writeFloat"); });
    e.def("writestring", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "writeString"), "writeString");
        std::string& s = sstr(arg(a, 1, "writeString"), "writeString");
        fwrite(s.data(), 1, s.size(), f); fputc(0, f);              // MXS writeString appends NUL
        return Value::ok();
    });
    e.def("writeline", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        FILE* f = file_of(arg(a, 0, "writeLine"), "writeLine");
        std::string& s = sstr(arg(a, 1, "writeLine"), "writeLine");
        fwrite(s.data(), 1, s.size(), f); fputc('\n', f);
        return Value::ok();
    });

    // ---- path/query ----
    e.def("getfilenamepath", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        fs::path p(sstr(arg(a, 0, "getFilenamePath"), "getFilenamePath"));
        std::string d = p.parent_path().string();
        if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '\\';
        return Value::S(d);
    });
    e.def("getfilenamefile", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::S(fs::path(sstr(arg(a, 0, "getFilenameFile"), "getFilenameFile")).stem().string()); });
    e.def("getfilenametype", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::S(fs::path(sstr(arg(a, 0, "getFilenameType"), "getFilenameType")).extension().string()); });
    e.def("doesfileexist", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        return Value::B(fs::exists(sstr(arg(a, 0, "doesFileExist"), "doesFileExist"), ec)); });
    e.def("getfilesize", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        auto n = fs::file_size(sstr(arg(a, 0, "getFileSize"), "getFileSize"), ec);
        return ec ? Value::I(0) : Value::I((int64_t)n); });
    e.def("deletefile", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        std::error_code ec;
        return Value::B(fs::remove(sstr(arg(a, 0, "deleteFile"), "deleteFile"), ec)); });
    e.def("makedir", [](std::vector<Value>& a, std::map<std::string, Value>& kw, Env&) {
        const fs::path p(sstr(arg(a, 0, "makeDir"), "makeDir"));
        std::error_code ec;
        // MAXScript 8 defaults all:false. The default changed to all:true
        // in Max 9; always recursing silently changes Max 8 script behavior.
        const auto it = kw.find("all");
        const bool parents = it != kw.end() && it->second.truthy();
        if (fs::is_directory(p, ec)) return Value::B(true); // existing directory is a success
        ec.clear();
        const bool created = parents ? fs::create_directories(p, ec) : fs::create_directory(p, ec);
        return Value::B(!ec && (created || fs::is_directory(p, ec))); });
    e.def("getfiles", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        // wildcard pattern: dir/*.ext
        fs::path pat(sstr(arg(a, 0, "getFiles"), "getFiles"));
        fs::path dir = pat.parent_path();
        std::string mask = lower(pat.filename().string());
        Array_t out;
        std::error_code ec;
        if (dir.empty()) dir = ".";
        auto match = [&](const std::string& name) {
            // glob: '*' and '?' only
            std::string n = lower(name);
            size_t mi = 0, ni = 0, star = std::string::npos, ss = 0;
            while (ni < n.size()) {
                if (mi < mask.size() && (mask[mi] == '?' || mask[mi] == n[ni])) { ++mi; ++ni; }
                else if (mi < mask.size() && mask[mi] == '*') { star = mi++; ss = ni; }
                else if (star != std::string::npos) { mi = star + 1; ni = ++ss; }
                else return false;
            }
            while (mi < mask.size() && mask[mi] == '*') ++mi;
            return mi == mask.size();
        };
        for (auto& de : fs::directory_iterator(dir, ec)) {
            if (!de.is_regular_file(ec)) continue;
            if (match(de.path().filename().string()))
                out.push_back(Value::S(de.path().string()));
        }
        return Value::A(std::move(out));
    });
}

// ---------------- 3D math constructors ----------------
void install_math3d(Engine& e, Env&) {
    auto f = [](Value& v) { return (float)v.num(); };
    e.def("point2", [f](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::P2(f(arg(a,0,"point2")), f(arg(a,1,"point2"))); });
    e.def("point3", [f](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::P3(f(arg(a,0,"point3")), f(arg(a,1,"point3")), f(arg(a,2,"point3"))); });
    e.def("point4", [f](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::P4(f(arg(a,0,"point4")), f(arg(a,1,"point4")), f(arg(a,2,"point4")), f(arg(a,3,"point4"))); });
    e.def("quat", [f](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::QuatV(f(arg(a,0,"quat")), f(arg(a,1,"quat")), f(arg(a,2,"quat")), f(arg(a,3,"quat"))); });
    e.def("color", [f](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        return Value::ColorV(f(arg(a,0,"color")), f(arg(a,1,"color")), f(arg(a,2,"color")),
                             a.size() > 3 ? f(a[3]) : 255.f); });
    e.def("matrix3", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value out; out.t = Type::Matrix3; out.mat = std::make_shared<Mat3>();
        if (a.size() == 1 && a[0].is_num()) return out;             // matrix3 1 = identity
        if (a.size() == 4) {
            for (int r = 0; r < 4; ++r) {
                if (a[r].t != Type::Point3) throw MxsError("matrix3: point3 rows expected");
                out.mat->m[r][0] = a[r].vec->x; out.mat->m[r][1] = a[r].vec->y; out.mat->m[r][2] = a[r].vec->z;
            }
            return out;
        }
        throw MxsError("matrix3: expected 'matrix3 1' or 4 point3 rows");
    });
    e.def("inverse", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = arg(a, 0, "inverse");
        if (v.t == Type::Quat) return Value::QuatV(-v.vec->x, -v.vec->y, -v.vec->z, v.vec->w);
        if (v.t != Type::Matrix3) throw MxsError("inverse: matrix3 or quat expected");
        auto& m = v.mat->m;
        float det = m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1])
                  - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])
                  + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
        if (det == 0) throw MxsError("inverse: singular matrix");
        float id = 1.f / det;
        Value out; out.t = Type::Matrix3; out.mat = std::make_shared<Mat3>();
        auto& o = out.mat->m;
        o[0][0] =  (m[1][1]*m[2][2] - m[1][2]*m[2][1]) * id;
        o[0][1] = -(m[0][1]*m[2][2] - m[0][2]*m[2][1]) * id;
        o[0][2] =  (m[0][1]*m[1][2] - m[0][2]*m[1][1]) * id;
        o[1][0] = -(m[1][0]*m[2][2] - m[1][2]*m[2][0]) * id;
        o[1][1] =  (m[0][0]*m[2][2] - m[0][2]*m[2][0]) * id;
        o[1][2] = -(m[0][0]*m[1][2] - m[0][2]*m[1][0]) * id;
        o[2][0] =  (m[1][0]*m[2][1] - m[1][1]*m[2][0]) * id;
        o[2][1] = -(m[0][0]*m[2][1] - m[0][1]*m[2][0]) * id;
        o[2][2] =  (m[0][0]*m[1][1] - m[0][1]*m[1][0]) * id;
        // translation: -T * R^-1
        float tx = v.mat->m[3][0], ty = v.mat->m[3][1], tz = v.mat->m[3][2];
        o[3][0] = -(tx*o[0][0] + ty*o[1][0] + tz*o[2][0]);
        o[3][1] = -(tx*o[0][1] + ty*o[1][1] + tz*o[2][1]);
        o[3][2] = -(tx*o[0][2] + ty*o[1][2] + tz*o[2][2]);
        return out;
    });
    e.def("normalize", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = arg(a, 0, "normalize");
        if (!v.vec) throw MxsError("normalize: vector expected");
        float x = v.vec->x, y = v.vec->y, z = v.vec->z, w = v.vec->w;
        int n = v.t == Type::Point2 ? 2 : v.t == Type::Point3 ? 3 : 4;
        float len = std::sqrt(x*x + y*y + (n > 2 ? z*z : 0) + (n > 3 ? w*w : 0));
        if (len == 0) return v;
        Value o = v; o.vec = std::make_shared<Vec4>(*v.vec);
        o.vec->x /= len; o.vec->y /= len;
        if (n > 2) o.vec->z /= len;
        if (n > 3) o.vec->w /= len;
        return o;
    });
    e.def("length", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& v = arg(a, 0, "length");
        if (!v.vec) throw MxsError("length: vector expected");
        float x = v.vec->x, y = v.vec->y, z = v.vec->z, w = v.vec->w;
        int n = v.t == Type::Point2 ? 2 : v.t == Type::Point3 ? 3 : 4;
        return Value::F(std::sqrt(x*x + y*y + (n > 2 ? z*z : 0) + (n > 3 ? w*w : 0)));
    });
    e.def("distance", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& p = arg(a, 0, "distance"); Value& q = arg(a, 1, "distance");
        if (!p.vec || !q.vec) throw MxsError("distance: vectors expected");
        float dx = p.vec->x - q.vec->x, dy = p.vec->y - q.vec->y, dz = p.vec->z - q.vec->z;
        return Value::F(std::sqrt(dx*dx + dy*dy + dz*dz));
    });
    e.def("dot", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& p = arg(a, 0, "dot"); Value& q = arg(a, 1, "dot");
        if (!p.vec || !q.vec) throw MxsError("dot: vectors expected");
        return Value::F(p.vec->x*q.vec->x + p.vec->y*q.vec->y + p.vec->z*q.vec->z + p.vec->w*q.vec->w);
    });
    e.def("cross", [](std::vector<Value>& a, std::map<std::string, Value>&, Env&) {
        Value& p = arg(a, 0, "cross"); Value& q = arg(a, 1, "cross");
        if (p.t != Type::Point3 || q.t != Type::Point3) throw MxsError("cross: point3 expected");
        return Value::P3(p.vec->y*q.vec->z - p.vec->z*q.vec->y,
                         p.vec->z*q.vec->x - p.vec->x*q.vec->z,
                         p.vec->x*q.vec->y - p.vec->y*q.vec->x);
    });
}

} // namespace mxs
