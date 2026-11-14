#include "syntax.h"
#include "text_doc.h"
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <unordered_set>

namespace np {

const wchar_t* LangName(Lang l) {
    static const wchar_t* n[L_COUNT] = { L"Plain text", L"C", L"C++", L"C#", L"Java", L"JavaScript", L"TypeScript",
        L"Rust", L"Go", L"Python", L"JSON", L"XML", L"HTML", L"CSS", L"Markdown", L"INI", L"YAML", L"Shell",
        L"PowerShell", L"Batch", L"SQL", L"Lua", L"Assembly", L"BBCode", L"CSV" };
    return l < L_COUNT ? n[l] : L"Plain text";
}

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

Lang LangFromPath(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = Lower(slash == std::wstring::npos ? path : path.substr(slash + 1));
    size_t dot = name.rfind(L'.');
    std::wstring e = dot == std::wstring::npos ? L"" : name.substr(dot + 1);
    if (name == L"makefile" || name == L"dockerfile") return L_SH;
    struct M { const wchar_t* ext; Lang l; };
    static const M map[] = {
        {L"c",L_C},{L"h",L_CPP},{L"cpp",L_CPP},{L"cc",L_CPP},{L"cxx",L_CPP},{L"hpp",L_CPP},{L"hh",L_CPP},{L"hxx",L_CPP},{L"inl",L_CPP},{L"inc",L_CPP},
        {L"cs",L_CS},{L"java",L_JAVA},{L"kt",L_JAVA},{L"js",L_JS},{L"mjs",L_JS},{L"cjs",L_JS},{L"jsx",L_JS},{L"ts",L_TS},{L"tsx",L_TS},
        {L"rs",L_RUST},{L"go",L_GO},{L"py",L_PY},{L"pyw",L_PY},{L"json",L_JSON},{L"jsonc",L_JSON},{L"xml",L_XML},{L"xaml",L_XML},
        {L"vcxproj",L_XML},{L"props",L_XML},{L"targets",L_XML},{L"manifest",L_XML},{L"svg",L_XML},{L"csproj",L_XML},{L"resx",L_XML},
        {L"html",L_HTML},{L"htm",L_HTML},{L"css",L_CSS},{L"scss",L_CSS},{L"md",L_MD},{L"markdown",L_MD},{L"ini",L_INI},{L"cfg",L_INI},
        {L"inf",L_INI},{L"reg",L_INI},{L"toml",L_INI},{L"yml",L_YAML},{L"yaml",L_YAML},{L"sh",L_SH},{L"bash",L_SH},{L"ps1",L_PS},
        {L"psm1",L_PS},{L"bat",L_BAT},{L"cmd",L_BAT},{L"sql",L_SQL},{L"lua",L_LUA},{L"asm",L_ASM},{L"s",L_ASM},{L"nasm",L_ASM},{L"masm",L_ASM},{L"bbcode",L_BBCODE},{L"bb",L_BBCODE},{L"csv",L_CSV},{L"tsv",L_CSV},{L"tab",L_CSV},
    };
    for (auto& m : map) if (e == m.ext) return m.l;
    return L_TEXT;
}
Lang LangFromContent(const std::wstring& l) {
    if (l.rfind(L"#!", 0) == 0) { if (l.find(L"python") != std::wstring::npos) return L_PY; return L_SH; }
    if (l.rfind(L"<?xml", 0) == 0) return L_XML;
    if (l.rfind(L"<!DOCTYPE html", 0) == 0 || l.rfind(L"<html", 0) == 0) return L_HTML;
    return L_TEXT;
}
const wchar_t* LineCommentPrefix(Lang l) {
    switch (l) {
    case L_PY: case L_YAML: case L_SH: case L_PS: return L"# ";
    case L_INI: return L"; "; case L_ASM: return L"; ";
    case L_SQL: case L_LUA: return L"-- ";
    case L_BAT: return L"REM ";
    case L_TEXT: case L_MD: case L_XML: case L_HTML: case L_JSON: case L_CSS: case L_BBCODE: case L_CSV: return nullptr;
    default: return L"// ";
    }
}
bool IsProseTok(Lang lang, Tok t) {
    if (lang == L_TEXT || lang == L_MD || lang == L_BBCODE) return t != T_CODE && t != T_LINK && t != T_TAG && t != T_ATTR;
    return t == T_COMMENT || t == T_STRING;
}

// ------------------------------------------------------------------ keyword tables
using WSet = std::unordered_set<std::wstring>;
static WSet Mk(std::initializer_list<const wchar_t*> l) { WSet s; for (auto* w : l) s.insert(w); return s; }

struct LangDef {
    WSet kw, types;
    const wchar_t* line1 = nullptr; const wchar_t* line2 = nullptr;
    const wchar_t* blockOpen = nullptr; const wchar_t* blockClose = nullptr;
    bool preproc = false, tripleQuote = false, backtick = false, singleQuoteStr = true, caseInsensitive = false;
    bool dollarVar = false;
};

static const LangDef& Def(Lang l) {
    static LangDef d[L_COUNT];
    static bool init = false;
    if (!init) {
        init = true;
        auto cBase = Mk({L"if",L"else",L"for",L"while",L"do",L"switch",L"case",L"default",L"break",L"continue",L"return",L"goto",
            L"sizeof",L"typedef",L"struct",L"union",L"enum",L"static",L"extern",L"const",L"volatile",L"inline",L"register",L"restrict"});
        auto cTypes = Mk({L"void",L"char",L"short",L"int",L"long",L"float",L"double",L"signed",L"unsigned",L"bool",L"_Bool",
            L"size_t",L"int8_t",L"int16_t",L"int32_t",L"int64_t",L"uint8_t",L"uint16_t",L"uint32_t",L"uint64_t",L"uintptr_t",L"intptr_t",
            L"wchar_t",L"BYTE",L"WORD",L"DWORD",L"QWORD",L"BOOL",L"HANDLE",L"HWND",L"LPVOID",L"PVOID",L"ULONG",L"LONG",L"UINT",L"HRESULT",
            L"__int64",L"char8_t",L"char16_t",L"char32_t",L"auto"});
        d[L_C].kw = cBase; d[L_C].types = cTypes;
        d[L_C].line1 = L"//"; d[L_C].blockOpen = L"/*"; d[L_C].blockClose = L"*/"; d[L_C].preproc = true;
        d[L_CPP] = d[L_C];
        for (auto* w : {L"class",L"public",L"private",L"protected",L"virtual",L"override",L"final",L"template",L"typename",L"namespace",
             L"using",L"new",L"delete",L"this",L"operator",L"friend",L"try",L"catch",L"throw",L"noexcept",L"constexpr",L"consteval",
             L"constinit",L"static_cast",L"dynamic_cast",L"reinterpret_cast",L"const_cast",L"nullptr",L"true",L"false",L"explicit",
             L"mutable",L"decltype",L"co_await",L"co_return",L"co_yield",L"concept",L"requires",L"static_assert",L"thread_local",L"alignas",L"alignof",L"export",L"module",L"import"})
            d[L_CPP].kw.insert(w);
        d[L_CPP].types.insert(L"std"); d[L_CPP].types.insert(L"string"); d[L_CPP].types.insert(L"vector");
        d[L_CS].kw = Mk({L"abstract",L"as",L"base",L"break",L"case",L"catch",L"checked",L"class",L"const",L"continue",L"default",L"delegate",
            L"do",L"else",L"enum",L"event",L"explicit",L"extern",L"false",L"finally",L"fixed",L"for",L"foreach",L"goto",L"if",L"implicit",
            L"in",L"interface",L"internal",L"is",L"lock",L"namespace",L"new",L"null",L"operator",L"out",L"override",L"params",L"private",
            L"protected",L"public",L"readonly",L"ref",L"return",L"sealed",L"sizeof",L"stackalloc",L"static",L"struct",L"switch",L"this",
            L"throw",L"true",L"try",L"typeof",L"unchecked",L"unsafe",L"using",L"virtual",L"volatile",L"while",L"async",L"await",L"var",L"record",L"get",L"set",L"init",L"yield"});
        d[L_CS].types = Mk({L"bool",L"byte",L"sbyte",L"char",L"decimal",L"double",L"float",L"int",L"uint",L"long",L"ulong",L"object",L"short",L"ushort",L"string",L"void",L"nint",L"nuint",L"dynamic"});
        d[L_CS].line1 = L"//"; d[L_CS].blockOpen = L"/*"; d[L_CS].blockClose = L"*/"; d[L_CS].preproc = true;
        d[L_JAVA] = d[L_CS]; d[L_JAVA].preproc = false;
        for (auto* w : {L"extends",L"implements",L"package",L"import",L"throws",L"synchronized",L"transient",L"native",L"strictfp",L"fun",L"val",L"when",L"object"}) d[L_JAVA].kw.insert(w);
        d[L_JAVA].types.insert(L"boolean"); d[L_JAVA].types.insert(L"String"); d[L_JAVA].types.insert(L"Integer");
        d[L_JS].kw = Mk({L"break",L"case",L"catch",L"class",L"const",L"continue",L"debugger",L"default",L"delete",L"do",L"else",L"export",
            L"extends",L"finally",L"for",L"function",L"if",L"import",L"in",L"instanceof",L"let",L"new",L"return",L"super",L"switch",L"this",
            L"throw",L"try",L"typeof",L"var",L"void",L"while",L"with",L"yield",L"async",L"await",L"of",L"static",L"get",L"set",L"from",
            L"true",L"false",L"null",L"undefined",L"NaN",L"Infinity"});
        d[L_JS].types = Mk({L"Array",L"Object",L"String",L"Number",L"Boolean",L"Promise",L"Map",L"Set",L"Date",L"RegExp",L"Error",L"JSON",L"Math",L"console",L"window",L"document"});
        d[L_JS].line1 = L"//"; d[L_JS].blockOpen = L"/*"; d[L_JS].blockClose = L"*/"; d[L_JS].backtick = true;
        d[L_TS] = d[L_JS];
        for (auto* w : {L"interface",L"type",L"enum",L"implements",L"namespace",L"declare",L"readonly",L"abstract",L"private",L"public",L"protected",L"as",L"keyof",L"infer",L"is"}) d[L_TS].kw.insert(w);
        for (auto* w : {L"string",L"number",L"boolean",L"any",L"unknown",L"never",L"void",L"object",L"bigint",L"symbol"}) d[L_TS].types.insert(w);
        d[L_RUST].kw = Mk({L"as",L"async",L"await",L"break",L"const",L"continue",L"crate",L"dyn",L"else",L"enum",L"extern",L"false",L"fn",
            L"for",L"if",L"impl",L"in",L"let",L"loop",L"match",L"mod",L"move",L"mut",L"pub",L"ref",L"return",L"self",L"Self",L"static",
            L"struct",L"super",L"trait",L"true",L"type",L"unsafe",L"use",L"where",L"while",L"macro_rules",L"union"});
        d[L_RUST].types = Mk({L"i8",L"i16",L"i32",L"i64",L"i128",L"isize",L"u8",L"u16",L"u32",L"u64",L"u128",L"usize",L"f32",L"f64",
            L"bool",L"char",L"str",L"String",L"Vec",L"Option",L"Result",L"Box",L"Rc",L"Arc",L"Some",L"None",L"Ok",L"Err"});
        d[L_RUST].line1 = L"//"; d[L_RUST].blockOpen = L"/*"; d[L_RUST].blockClose = L"*/"; d[L_RUST].singleQuoteStr = false;
        d[L_GO].kw = Mk({L"break",L"case",L"chan",L"const",L"continue",L"default",L"defer",L"else",L"fallthrough",L"for",L"func",L"go",
            L"goto",L"if",L"import",L"interface",L"map",L"package",L"range",L"return",L"select",L"struct",L"switch",L"type",L"var",L"true",L"false",L"nil",L"iota"});
        d[L_GO].types = Mk({L"bool",L"byte",L"complex64",L"complex128",L"error",L"float32",L"float64",L"int",L"int8",L"int16",L"int32",
            L"int64",L"rune",L"string",L"uint",L"uint8",L"uint16",L"uint32",L"uint64",L"uintptr",L"any"});
        d[L_GO].line1 = L"//"; d[L_GO].blockOpen = L"/*"; d[L_GO].blockClose = L"*/"; d[L_GO].backtick = true;
        d[L_PY].kw = Mk({L"False",L"None",L"True",L"and",L"as",L"assert",L"async",L"await",L"break",L"class",L"continue",L"def",L"del",
            L"elif",L"else",L"except",L"finally",L"for",L"from",L"global",L"if",L"import",L"in",L"is",L"lambda",L"nonlocal",L"not",L"or",
            L"pass",L"raise",L"return",L"try",L"while",L"with",L"yield",L"match",L"case",L"self"});
        d[L_PY].types = Mk({L"int",L"float",L"str",L"bytes",L"list",L"dict",L"set",L"tuple",L"bool",L"object",L"type",L"len",L"print",L"range",L"open",L"super",L"isinstance"});
        d[L_PY].line1 = L"#"; d[L_PY].tripleQuote = true;
        d[L_JSON].kw = Mk({L"true",L"false",L"null"});
        d[L_JSON].line1 = L"//"; d[L_JSON].blockOpen = L"/*"; d[L_JSON].blockClose = L"*/"; d[L_JSON].singleQuoteStr = false;
        d[L_CSS].kw = Mk({L"important",L"media",L"import",L"keyframes",L"font-face",L"supports",L"from",L"to"});
        d[L_CSS].blockOpen = L"/*"; d[L_CSS].blockClose = L"*/";
        d[L_SH].kw = Mk({L"if",L"then",L"else",L"elif",L"fi",L"for",L"while",L"until",L"do",L"done",L"case",L"esac",L"in",L"function",
            L"return",L"exit",L"local",L"export",L"readonly",L"echo",L"cd",L"set",L"unset",L"source",L"true",L"false"});
        d[L_SH].line1 = L"#"; d[L_SH].dollarVar = true;
        d[L_PS].kw = Mk({L"begin",L"break",L"catch",L"class",L"continue",L"data",L"do",L"dynamicparam",L"else",L"elseif",L"end",L"exit",
            L"filter",L"finally",L"for",L"foreach",L"from",L"function",L"if",L"in",L"param",L"process",L"return",L"switch",L"throw",
            L"trap",L"try",L"until",L"using",L"while",L"true",L"false",L"null"});
        d[L_PS].line1 = L"#"; d[L_PS].blockOpen = L"<#"; d[L_PS].blockClose = L"#>"; d[L_PS].caseInsensitive = true; d[L_PS].dollarVar = true;
        d[L_BAT].kw = Mk({L"echo",L"set",L"if",L"else",L"for",L"in",L"do",L"goto",L"call",L"exit",L"not",L"exist",L"defined",L"errorlevel",
            L"setlocal",L"endlocal",L"pushd",L"popd",L"shift",L"start",L"cd",L"copy",L"del",L"move",L"mkdir",L"rmdir",L"equ",L"neq",L"lss",L"leq",L"gtr",L"geq",L"off",L"on"});
        d[L_BAT].line1 = L"::"; d[L_BAT].caseInsensitive = true; d[L_BAT].singleQuoteStr = false;
        d[L_SQL].kw = Mk({L"select",L"from",L"where",L"insert",L"into",L"values",L"update",L"set",L"delete",L"create",L"table",L"drop",
            L"alter",L"add",L"index",L"view",L"join",L"inner",L"left",L"right",L"outer",L"full",L"on",L"as",L"and",L"or",L"not",L"null",
            L"is",L"in",L"like",L"between",L"group",L"by",L"order",L"having",L"limit",L"offset",L"union",L"all",L"distinct",L"case",L"when",
            L"then",L"else",L"end",L"primary",L"key",L"foreign",L"references",L"default",L"exists",L"begin",L"commit",L"rollback",L"asc",L"desc",L"top"});
        d[L_SQL].types = Mk({L"int",L"integer",L"bigint",L"smallint",L"varchar",L"nvarchar",L"char",L"text",L"date",L"datetime",L"float",L"real",L"decimal",L"numeric",L"bit",L"blob",L"boolean"});
        d[L_SQL].line1 = L"--"; d[L_SQL].blockOpen = L"/*"; d[L_SQL].blockClose = L"*/"; d[L_SQL].caseInsensitive = true;
        d[L_LUA].kw = Mk({L"and",L"break",L"do",L"else",L"elseif",L"end",L"false",L"for",L"function",L"goto",L"if",L"in",L"local",L"nil",
            L"not",L"or",L"repeat",L"return",L"then",L"true",L"until",L"while"});
        d[L_LUA].line1 = L"--";
        d[L_ASM].kw = Mk({L"mov",L"movzx",L"movsx",L"lea",L"push",L"pop",L"call",L"ret",L"jmp",L"je",L"jne",L"jz",L"jnz",L"jg",L"jge",L"jl",
            L"jle",L"ja",L"jae",L"jb",L"jbe",L"cmp",L"test",L"add",L"sub",L"imul",L"mul",L"idiv",L"div",L"inc",L"dec",L"and",L"or",L"xor",
            L"not",L"neg",L"shl",L"shr",L"sar",L"rol",L"ror",L"nop",L"int",L"syscall",L"cpuid",L"rdtsc",L"xchg",L"cmov",L"sete",L"setne",
            L"leave",L"enter",L"rep",L"movsb",L"stosb",L"loop",L"section",L"global",L"extern",L"db",L"dw",L"dd",L"dq",L"proc",L"endp",L"segment",L"ends",L"public",L"byte",L"word",L"dword",L"qword",L"ptr"});
        d[L_ASM].types = Mk({L"rax",L"rbx",L"rcx",L"rdx",L"rsi",L"rdi",L"rsp",L"rbp",L"r8",L"r9",L"r10",L"r11",L"r12",L"r13",L"r14",L"r15",
            L"eax",L"ebx",L"ecx",L"edx",L"esi",L"edi",L"esp",L"ebp",L"ax",L"bx",L"cx",L"dx",L"al",L"bl",L"cl",L"dl",L"ah",L"bh",L"ch",L"dh",L"rip",L"eip",
            L"xmm0",L"xmm1",L"xmm2",L"xmm3",L"xmm4",L"xmm5",L"xmm6",L"xmm7",L"cs",L"ds",L"es",L"fs",L"gs",L"ss"});
        d[L_ASM].line1 = L";"; d[L_ASM].line2 = L"#"; d[L_ASM].caseInsensitive = true;
    }
    return d[l < L_COUNT ? l : 0];
}

static bool At(const std::wstring& s, size_t i, const wchar_t* t) {
    if (!t) return false;
    for (size_t k = 0; t[k]; ++k) if (i + k >= s.size() || s[i + k] != t[k]) return false;
    return true;
}
static void Push(std::vector<Span>& o, int s, int len, Tok t) {
    if (len <= 0) return;
    if (!o.empty() && o.back().tok == t && o.back().start + o.back().len == s) { o.back().len += len; return; }
    o.push_back({ s, len, t });
}
static bool IdStart(wchar_t c) { return iswalpha(c) || c == L'_' || c > 0x7F; }
static bool IdChar(wchar_t c) { return iswalnum(c) || c == L'_' || c > 0x7F; }

// generic C-family / scripting lexer. States: 1 block comment, 2 """ , 3 ''' , 4 backtick
static int LexGeneric(Lang lang, const std::wstring& s, int st, std::vector<Span>& o) {
    const LangDef& d = Def(lang);
    size_t i = 0, n = s.size();
    auto contClose = [&](const wchar_t* close, Tok t, int stv) -> bool {
        size_t e = s.find(close, i);
        if (e == std::wstring::npos) { Push(o, (int)i, (int)(n - i), t); i = n; return false; }
        e += wcslen(close); Push(o, (int)i, (int)(e - i), t); i = e; return true;
        (void)stv;
    };
    if (st == 1) { if (!contClose(d.blockClose, T_COMMENT, 1)) return 1; st = 0; }
    else if (st == 2) { if (!contClose(L"\"\"\"", T_STRING, 2)) return 2; st = 0; }
    else if (st == 3) { if (!contClose(L"'''", T_STRING, 3)) return 3; st = 0; }
    else if (st == 4) {
        size_t j = i; bool closed = false;
        while (j < n) { if (s[j] == L'\\') { j += 2; continue; } if (s[j] == L'`') { ++j; closed = true; break; } ++j; }
        j = std::min(j, n); Push(o, (int)i, (int)(j - i), T_STRING); i = j;
        if (!closed) return 4;
    }
    // preprocessor line
    if (d.preproc && i == 0) {
        size_t k = 0; while (k < n && (s[k] == L' ' || s[k] == L'\t')) ++k;
        if (k < n && s[k] == L'#') {
            size_t e = k + 1; while (e < n && (s[e] == L' ' || s[e] == L'\t')) ++e;
            while (e < n && IdChar(s[e])) ++e;
            Push(o, (int)k, (int)(e - k), T_PREPROC); i = e;
            // rest: <include> as string
            size_t lt = s.find(L'<', i);
            if (s.compare(k, std::min<size_t>(e - k, 9), L"#include") == 0 && lt != std::wstring::npos) {
                size_t gt = s.find(L'>', lt); if (gt != std::wstring::npos) { Push(o, (int)i, (int)(lt - i), T_TEXT); Push(o, (int)lt, (int)(gt - lt + 1), T_STRING); i = gt + 1; }
            }
        }
    }
    while (i < n) {
        wchar_t c = s[i];
        if (At(s, i, d.line1) || At(s, i, d.line2)) {
            if (lang == L_SH && c == L'#' && i > 0 && s[i - 1] != L' ' && s[i - 1] != L'\t') { Push(o, (int)i, 1, T_TEXT); ++i; continue; }
            Push(o, (int)i, (int)(n - i), T_COMMENT); return 0;
        }
        if (lang == L_BAT && i == 0) {
            size_t k = 0; while (k < n && s[k] == L' ') ++k;
            if (n - k >= 3 && towlower(s[k]) == L'r' && towlower(s[k + 1]) == L'e' && towlower(s[k + 2]) == L'm' && (n - k == 3 || s[k + 3] == L' ')) { Push(o, 0, (int)n, T_COMMENT); return 0; }
        }
        if (At(s, i, d.blockOpen)) {
            size_t e = s.find(d.blockClose, i + wcslen(d.blockOpen));
            if (e == std::wstring::npos) { Push(o, (int)i, (int)(n - i), T_COMMENT); return 1; }
            e += wcslen(d.blockClose); Push(o, (int)i, (int)(e - i), T_COMMENT); i = e; continue;
        }
        if (d.tripleQuote && (At(s, i, L"\"\"\"") || At(s, i, L"'''"))) {
            const wchar_t* q = s[i] == L'"' ? L"\"\"\"" : L"'''";
            size_t e = s.find(q, i + 3);
            if (e == std::wstring::npos) { Push(o, (int)i, (int)(n - i), T_STRING); return s[i] == L'"' ? 2 : 3; }
            Push(o, (int)i, (int)(e + 3 - i), T_STRING); i = e + 3; continue;
        }
        if (d.backtick && c == L'`') {
            size_t j = i + 1; bool closed = false;
            while (j < n) { if (s[j] == L'\\') { j += 2; continue; } if (s[j] == L'`') { ++j; closed = true; break; } ++j; }
            j = std::min(j, n); Push(o, (int)i, (int)(j - i), T_STRING); i = j;
            if (!closed) return 4;
            continue;
        }
        if (c == L'"' || (c == L'\'' && d.singleQuoteStr) || (c == L'\'' && lang == L_RUST && i + 2 < n && (s[i + 2] == L'\'' || s[i + 1] == L'\\'))) {
            size_t j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == L'\\' && lang != L_BAT) ++j; ++j; }
            j = std::min(j + 1, n);
            Tok t = T_STRING;
            if (lang == L_JSON && c == L'"') {   // key if followed by ':'
                size_t k = j; while (k < n && (s[k] == L' ' || s[k] == L'\t')) ++k;
                if (k < n && s[k] == L':') t = T_KEY;
            }
            Push(o, (int)i, (int)(j - i), t); i = j; continue;
        }
        if (d.dollarVar && c == L'$') {
            size_t j = i + 1;
            if (j < n && s[j] == L'{') { size_t e = s.find(L'}', j); j = e == std::wstring::npos ? n : e + 1; }
            else while (j < n && (IdChar(s[j]) || s[j] == L':')) ++j;
            Push(o, (int)i, (int)(j - i), T_KEY); i = j; continue;
        }
        if (lang == L_BAT && c == L'%') {
            size_t e = s.find(L'%', i + 1);
            size_t j = (e == std::wstring::npos) ? i + 1 : e + 1;
            if (i + 1 < n && iswdigit(s[i + 1])) j = i + 2;
            Push(o, (int)i, (int)(j - i), T_KEY); i = j; continue;
        }
        if (iswdigit(c) || (c == L'.' && i + 1 < n && iswdigit(s[i + 1]))) {
            size_t j = i;
            if (c == L'0' && i + 1 < n && (s[i + 1] == L'x' || s[i + 1] == L'X' || s[i + 1] == L'b' || s[i + 1] == L'B')) j += 2;
            while (j < n && (iswxdigit(s[j]) || s[j] == L'.' || s[j] == L'_' || s[j] == L'\'' ||
                   ((s[j] == L'+' || s[j] == L'-') && (s[j - 1] == L'e' || s[j - 1] == L'E')) || iswalpha(s[j]))) ++j;
            Push(o, (int)i, (int)(j - i), T_NUMBER); i = j; continue;
        }
        if (IdStart(c) || (lang == L_CSS && c == L'-') || (lang == L_CSS && c == L'@')) {
            size_t j = i + 1;
            while (j < n && (IdChar(s[j]) || (lang == L_CSS && s[j] == L'-') || (lang == L_PS && s[j] == L'-'))) ++j;
            std::wstring w = s.substr(i, j - i);
            if (d.caseInsensitive) w = Lower(w);
            Tok t = T_TEXT;
            std::wstring wk = w; if (lang == L_CSS && !wk.empty() && wk[0] == L'@') wk.erase(0, 1);
            if (d.kw.count(wk)) t = T_KEYWORD;
            else if (d.types.count(w)) t = T_TYPE;
            else {
                size_t k = j; while (k < n && s[k] == L' ') ++k;
                if (lang == L_CSS) { if (k < n && s[k] == L':') t = T_KEY; }
                else if (k < n && s[k] == L'(') t = T_FUNC;
                else if (lang == L_RUST && k < n && s[k] == L'!') t = T_FUNC;
                else if (iswupper(w[0]) && lang != L_SQL && lang != L_ASM && lang != L_BAT && w.size() > 1) t = T_TYPE;
            }
            if (lang == L_ASM && j < n && s[j] == L':') t = T_FUNC;
            Push(o, (int)i, (int)(j - i), t); i = j; continue;
        }
        if (wcschr(L"+-*/%=<>!&|^~?:;,.(){}[]@", c) && c) { Push(o, (int)i, 1, T_OPERATOR); ++i; continue; }
        Push(o, (int)i, 1, T_TEXT); ++i;
    }
    return 0;
}

// XML/HTML. States: 1 in comment, 2 inside tag (attributes continue), 3 CDATA
static int LexXml(const std::wstring& s, int st, std::vector<Span>& o) {
    size_t i = 0, n = s.size();
    while (i < n) {
        if (st == 1) {
            size_t e = s.find(L"-->", i);
            if (e == std::wstring::npos) { Push(o, (int)i, (int)(n - i), T_COMMENT); return 1; }
            Push(o, (int)i, (int)(e + 3 - i), T_COMMENT); i = e + 3; st = 0; continue;
        }
        if (st == 3) {
            size_t e = s.find(L"]]>", i);
            if (e == std::wstring::npos) { Push(o, (int)i, (int)(n - i), T_CODE); return 3; }
            Push(o, (int)i, (int)(e + 3 - i), T_CODE); i = e + 3; st = 0; continue;
        }
        if (st == 2) {
            wchar_t c = s[i];
            if (c == L'>' || (c == L'/' && i + 1 < n && s[i + 1] == L'>') || (c == L'?' && i + 1 < n && s[i + 1] == L'>')) {
                size_t l = c == L'>' ? 1 : 2; Push(o, (int)i, (int)l, T_TAG); i += l; st = 0; continue;
            }
            if (c == L'"' || c == L'\'') {
                size_t e = s.find(c, i + 1); e = e == std::wstring::npos ? n : e + 1;
                Push(o, (int)i, (int)(e - i), T_STRING); i = e; continue;
            }
            if (IdStart(c)) { size_t j = i; while (j < n && (IdChar(s[j]) || s[j] == L'-' || s[j] == L':' || s[j] == L'.')) ++j; Push(o, (int)i, (int)(j - i), T_ATTR); i = j; continue; }
            Push(o, (int)i, 1, c == L'=' ? T_OPERATOR : T_TEXT); ++i; continue;
        }
        if (At(s, i, L"<!--")) { st = 1; Push(o, (int)i, 4, T_COMMENT); i += 4; continue; }
        if (At(s, i, L"<![CDATA[")) { st = 3; Push(o, (int)i, 9, T_CODE); i += 9; continue; }
        if (s[i] == L'<') {
            size_t j = i + 1;
            if (j < n && (s[j] == L'/' || s[j] == L'?' || s[j] == L'!')) ++j;
            while (j < n && (IdChar(s[j]) || s[j] == L'-' || s[j] == L':' || s[j] == L'.')) ++j;
            Push(o, (int)i, (int)(j - i), T_TAG); i = j; st = 2; continue;
        }
        if (s[i] == L'&') {
            size_t e = s.find(L';', i);
            if (e != std::wstring::npos && e - i < 10) { Push(o, (int)i, (int)(e + 1 - i), T_NUMBER); i = e + 1; continue; }
        }
        Push(o, (int)i, 1, T_TEXT); ++i;
    }
    return st;
}

// Markdown. State 1: inside ``` fence.
static void MdInline(const std::wstring& s, size_t i, size_t n, std::vector<Span>& o, Tok base) {
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'`') {
            size_t e = s.find(L'`', i + 1);
            if (e != std::wstring::npos && e < n) { Push(o, (int)i, (int)(e + 1 - i), T_CODE); i = e + 1; continue; }
        }
        if ((c == L'*' || c == L'_') && i + 1 < n && s[i + 1] == c) {
            std::wstring m(2, c); size_t e = s.find(m, i + 2);
            if (e != std::wstring::npos && e < n && e > i + 2) { Push(o, (int)i, (int)(e + 2 - i), T_STRONG); i = e + 2; continue; }
        }
        if ((c == L'*' || c == L'_') && i + 1 < n && s[i + 1] != L' ') {
            size_t e = s.find(c, i + 1);
            if (e != std::wstring::npos && e < n && e > i + 1 && (c == L'*' || i == 0 || !IdChar(s[i - 1]))) { Push(o, (int)i, (int)(e + 1 - i), T_EMPH); i = e + 1; continue; }
        }
        if (c == L'[' || (c == L'!' && i + 1 < n && s[i + 1] == L'[')) {
            size_t rb = s.find(L"](", i);
            if (rb != std::wstring::npos && rb < n) { size_t rp = s.find(L')', rb); if (rp != std::wstring::npos && rp < n) { Push(o, (int)i, (int)(rp + 1 - i), T_LINK); i = rp + 1; continue; } }
        }
        if (At(s, i, L"http://") || At(s, i, L"https://")) {
            size_t j = i; while (j < n && !iswspace(s[j]) && s[j] != L')' && s[j] != L'>') ++j;
            Push(o, (int)i, (int)(j - i), T_LINK); i = j; continue;
        }
        Push(o, (int)i, 1, base); ++i;
    }
}
static int LexMd(const std::wstring& s, int st, std::vector<Span>& o) {
    size_t n = s.size(), k = 0;
    while (k < n && s[k] == L' ') ++k;
    if (At(s, k, L"```") || At(s, k, L"~~~")) { Push(o, 0, (int)n, T_CODE); return st == 1 ? 0 : 1; }
    if (st == 1) { Push(o, 0, (int)n, T_CODE); return 1; }
    if (k >= 4) { Push(o, 0, (int)n, T_CODE); return 0; }
    if (k < n && s[k] == L'#') { Push(o, 0, (int)n, T_HEADING); return 0; }
    if (k < n && s[k] == L'>') { Push(o, 0, (int)k + 1, T_QUOTE); MdInline(s, k + 1, n, o, T_QUOTE); return 0; }
    {   // hr
        size_t c = 0; wchar_t m = 0; bool ok = n > k;
        for (size_t j = k; j < n; ++j) { if (s[j] == L' ') continue; if (!m && (s[j] == L'-' || s[j] == L'*' || s[j] == L'_')) m = s[j]; if (s[j] != m) { ok = false; break; } ++c; }
        if (ok && c >= 3) { Push(o, 0, (int)n, T_OPERATOR); return 0; }
    }
    size_t j = k;
    if (j < n && (s[j] == L'-' || s[j] == L'*' || s[j] == L'+') && j + 1 < n && s[j + 1] == L' ') j += 2;
    else { while (j < n && iswdigit(s[j])) ++j; if (j > k && j < n && (s[j] == L'.' || s[j] == L')') && j + 1 < n && s[j + 1] == L' ') j += 2; else j = k; }
    if (j > k) { if (At(s, j, L"[ ] ") || At(s, j, L"[x] ") || At(s, j, L"[X] ")) j += 4; Push(o, 0, (int)j, T_LIST); }
    else Push(o, 0, (int)k, T_TEXT);
    MdInline(s, j, n, o, T_TEXT);
    return 0;
}
static int LexIni(const std::wstring& s, std::vector<Span>& o, bool yaml) {
    size_t n = s.size(), k = 0;
    while (k < n && (s[k] == L' ' || s[k] == L'\t')) ++k;
    if (k < n && (s[k] == L'#' || (!yaml && s[k] == L';'))) { Push(o, 0, (int)n, T_COMMENT); return 0; }
    if (!yaml && k < n && s[k] == L'[') { Push(o, 0, (int)n, T_HEADING); return 0; }
    if (yaml && At(s, k, L"---")) { Push(o, 0, (int)n, T_OPERATOR); return 0; }
    size_t sep = yaml ? s.find(L':', k) : s.find_first_of(L"=:", k);
    size_t start = k;
    if (yaml && k < n && s[k] == L'-') { Push(o, (int)k, 1, T_OPERATOR); start = k + 1; }
    if (sep != std::wstring::npos && (!yaml || sep + 1 == n || s[sep + 1] == L' ')) {
        Push(o, 0, (int)start, T_TEXT);
        Push(o, (int)start, (int)(sep - start), T_KEY); Push(o, (int)sep, 1, T_OPERATOR);
        size_t v = sep + 1;
        size_t hash = s.find(L" #", v);
        size_t vend = (yaml && hash != std::wstring::npos) ? hash : n;
        std::wstring val = s.substr(v, vend - v);
        size_t a = val.find_first_not_of(L" \t");
        Tok t = T_STRING;
        if (a != std::wstring::npos) {
            std::wstring tv = Lower(val.substr(a));
            while (!tv.empty() && tv.back() == L' ') tv.pop_back();
            if (tv == L"true" || tv == L"false" || tv == L"yes" || tv == L"no" || tv == L"null" || tv == L"on" || tv == L"off") t = T_KEYWORD;
            else if (!tv.empty() && (iswdigit(tv[0]) || tv[0] == L'-' || tv[0] == L'.') && tv.find_first_not_of(L"0123456789.-+exabcdf_") == std::wstring::npos) t = T_NUMBER;
        }
        Push(o, (int)v, (int)(vend - v), t);
        if (vend < n) Push(o, (int)vend, (int)(n - vend), T_COMMENT);
        return 0;
    }
    Push(o, 0, (int)n, yaml ? T_STRING : T_TEXT);
    return 0;
}

// BBCode: [tag], [tag=value], [/tag]; state 1 inside [code]/[noparse]
static int LexBB(const std::wstring& s, int st, std::vector<Span>& o) {
    size_t i = 0, n = s.size();
    while (i < n) {
        if (st == 1) {
            size_t e = std::wstring::npos;
            for (size_t k = i; k < n; ++k) if (s[k] == L'[' && (s.compare(k, 7, L"[/code]") == 0 || s.compare(k, 10, L"[/noparse]") == 0)) { e = k; break; }
            if (e == std::wstring::npos) { Push(o, (int)i, (int)(n - i), T_CODE); return 1; }
            Push(o, (int)i, (int)(e - i), T_CODE); i = e; st = 0; continue;
        }
        if (s[i] == L'[') {
            size_t e = s.find(L']', i);
            size_t j = i + 1; if (j < n && s[j] == L'/') ++j;
            size_t n0 = j; while (j < n && (iswalnum(s[j]) || s[j] == L'*')) ++j;
            if (e != std::wstring::npos && j > n0 && (j == e || s[j] == L'=' || s[j] == L' ')) {
                Push(o, (int)i, (int)(j - i), T_TAG);
                if (j < e) { Push(o, (int)j, 1, T_OPERATOR); Push(o, (int)j + 1, (int)(e - j - 1), T_STRING); }
                Push(o, (int)e, 1, T_TAG);
                std::wstring name = Lower(s.substr(n0, j - n0));
                if (s[i + 1] != L'/' && (name == L"code" || name == L"noparse")) st = 1;
                i = e + 1; continue;
            }
        }
        if (At(s, i, L"http://") || At(s, i, L"https://")) { size_t j = i; while (j < n && !iswspace(s[j]) && s[j] != L'[') ++j; Push(o, (int)i, (int)(j - i), T_LINK); i = j; continue; }
        Push(o, (int)i, 1, T_TEXT); ++i;
    }
    return st;
}

// CSV "rainbow": each column gets its own colour; state = in a quoted field spanning lines.
static int LexCsv(const std::wstring& s, int st, std::vector<Span>& o) {
    static const Tok rot[6] = { T_KEYWORD, T_STRING, T_NUMBER, T_TYPE, T_FUNC, T_ATTR };
    // delimiter guess per line: the most frequent of , ; tab |
    int cnt[4] = {}; const wchar_t dl[4] = { L',', L';', L'\t', L'|' };
    for (wchar_t c : s) for (int k = 0; k < 4; ++k) if (c == dl[k]) ++cnt[k];
    int bk = 0; for (int k = 1; k < 4; ++k) if (cnt[k] > cnt[bk]) bk = k;
    wchar_t d = dl[bk];
    bool q = st != 0; int col = 0, start = 0;
    for (int i = 0; i <= (int)s.size(); ++i) {
        if (i == (int)s.size() || (!q && s[i] == d)) {
            Push(o, start, i - start, rot[col % 6]);
            if (i < (int)s.size()) Push(o, i, 1, T_OPERATOR);
            ++col; start = i + 1; continue;
        }
        if (s[i] == L'"') q = !q;
    }
    return q ? 1 : 0;
}

int LexLine(Lang lang, const std::wstring& line, int st, std::vector<Span>& out) {
    out.clear();
    if (line.size() > 20000) { Push(out, 0, (int)line.size(), T_TEXT); return st; }   // pathological lines
    switch (lang) {
    case L_TEXT: Push(out, 0, (int)line.size(), T_TEXT); return 0;
    case L_XML: case L_HTML: return LexXml(line, st, out);
    case L_MD: return LexMd(line, st, out);
    case L_BBCODE: return LexBB(line, st, out);
    case L_CSV: return LexCsv(line, st, out);
    case L_INI: return LexIni(line, out, false);
    case L_YAML: return LexIni(line, out, true);
    default: return LexGeneric(lang, line, st, out);
    }
}

int LexCache::StateAt(const TextDoc& doc, Lang lang, int line) {
    if ((int)states_.size() < doc.LineCount() + 1) states_.resize(doc.LineCount() + 1, 0);
    if (validTo_ <= 0) { states_[0] = 0; validTo_ = 0; }
    for (int i = validTo_; i < line && i < doc.LineCount(); ++i) {
        states_[i + 1] = LexLine(lang, doc.Line(i), states_[i], tmp_);
        validTo_ = i + 1;
    }
    return states_[std::min(line, validTo_)];
}

// ------------------------------------------------------------------ folding
static int Indent(const std::wstring& s, bool& blank) {
    int w = 0; size_t i = 0;
    for (; i < s.size(); ++i) { if (s[i] == L' ') ++w; else if (s[i] == L'\t') w = (w / 4 + 1) * 4; else break; }
    blank = i == s.size();
    return w;
}
void ComputeFolds(const TextDoc& doc, Lang lang, LexCache& lex, std::vector<FoldRegion>& out) {
    out.clear();
    int n = doc.LineCount();
    if (n > 200000) return;
    bool braces = !(lang == L_CSV || lang == L_BBCODE || lang == L_PY || lang == L_YAML || lang == L_MD || lang == L_TEXT || lang == L_XML || lang == L_HTML || lang == L_INI || lang == L_BAT || lang == L_ASM || lang == L_SQL);
    if (lang == L_MD) {
        std::vector<std::pair<int, int>> stack;   // (level, line)
        bool fence = false;
        for (int i = 0; i < n; ++i) {
            const std::wstring& s = doc.Line(i);
            if (s.rfind(L"```", 0) == 0) fence = !fence;
            if (fence || s.empty() || s[0] != L'#') continue;
            int lvl = 0; while (lvl < (int)s.size() && s[lvl] == L'#') ++lvl;
            while (!stack.empty() && stack.back().first >= lvl) { int st = stack.back().second; stack.pop_back(); int e = i - 1; while (e > st && doc.Line(e).empty()) --e; if (e > st) out.push_back({ st, e }); }
            stack.push_back({ lvl, i });
        }
        while (!stack.empty()) { int st = stack.back().second; stack.pop_back(); int e = n - 1; while (e > st && doc.Line(e).empty()) --e; if (e > st) out.push_back({ st, e }); }
    } else if (braces) {
        std::vector<int> stack; std::vector<Span> sp;
        int state = lex.StateAt(doc, lang, 0);
        for (int i = 0; i < n; ++i) {
            const std::wstring& s = doc.Line(i);
            state = LexLine(lang, s, state, sp);
            for (auto& x : sp) {
                if (x.tok != T_OPERATOR) continue;
                for (int k = x.start; k < x.start + x.len; ++k) {
                    wchar_t c = s[k];
                    if (c == L'{' || c == L'[') stack.push_back(i);
                    else if ((c == L'}' || c == L']') && !stack.empty()) {
                        int st = stack.back(); stack.pop_back();
                        // fold "}" line too unless "} else {" style continuation
                        int end = i;
                        if (end > st) out.push_back({ st, end });
                    }
                }
            }
        }
        // a region whose header is a lone "{" line: move header up one line (K&R vs Allman)
        for (auto& r : out) {
            const std::wstring& h = doc.Line(r.start);
            size_t a = h.find_first_not_of(L" \t");
            if (a != std::wstring::npos && h[a] == L'{' && r.start > 0) r.start -= 1;
        }
    } else if (lang != L_TEXT && lang != L_INI && lang != L_BAT) {
        // indentation based
        std::vector<int> ind(n); std::vector<char> blank(n);
        for (int i = 0; i < n; ++i) { bool b; ind[i] = Indent(doc.Line(i), b); blank[i] = b; }
        std::vector<std::pair<int, int>> stack;  // (indent, line)
        int lastNonBlank = -1;
        for (int i = 0; i < n; ++i) {
            if (blank[i]) continue;
            while (!stack.empty() && ind[i] <= stack.back().first) {
                int st = stack.back().second; stack.pop_back();
                if (lastNonBlank > st) out.push_back({ st, lastNonBlank });
            }
            if (lastNonBlank >= 0 && ind[i] > ind[lastNonBlank]) stack.push_back({ ind[lastNonBlank], lastNonBlank });
            lastNonBlank = i;
        }
        while (!stack.empty()) { int st = stack.back().second; stack.pop_back(); if (lastNonBlank > st) out.push_back({ st, lastNonBlank }); }
    } else if (lang == L_INI) {
        int sec = -1;
        for (int i = 0; i < n; ++i) {
            const std::wstring& s = doc.Line(i);
            if (!s.empty() && s[0] == L'[') { if (sec >= 0 && i - 1 > sec) out.push_back({ sec, i - 1 }); sec = i; }
        }
        if (sec >= 0 && n - 1 > sec) out.push_back({ sec, n - 1 });
    }
    // dedupe by start (keep largest), sort
    std::sort(out.begin(), out.end(), [](const FoldRegion& a, const FoldRegion& b) { return a.start < b.start || (a.start == b.start && a.end > b.end); });
    std::vector<FoldRegion> u;
    for (auto& r : out) if (u.empty() || u.back().start != r.start) u.push_back(r);
    out.swap(u);
}

} // namespace np
