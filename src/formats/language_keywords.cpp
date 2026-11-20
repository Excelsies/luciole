#include "formats/language_keywords.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace luciole::formats {
  namespace {
    template <std::size_t N> consteval std::array<std::string_view, N> sorted(std::array<std::string_view, N> words) {
      std::ranges::sort(words);
      return words;
    }

    constexpr auto kC = sorted(
        std::to_array<std::string_view>({
            "#define", "#elif",  "#else",  "#endif",   "#error",   "#if",      "#ifdef", "#ifndef",  "#include",
            "#pragma", "#undef", "auto",   "break",    "case",     "char",     "const",  "continue", "default",
            "do",      "double", "else",   "enum",     "extern",   "float",    "for",    "goto",     "if",
            "inline",  "int",    "long",   "register", "restrict", "return",   "short",  "signed",   "sizeof",
            "static",  "struct", "switch", "typedef",  "union",    "unsigned", "void",   "volatile", "while",
        })
    );

    constexpr auto kCpp = sorted(
        std::to_array<std::string_view>({
            "#define",      "#elif",    "#else",    "#endif",    "#error",     "#if",          "#ifdef",
            "#ifndef",      "#include", "#pragma",  "#undef",    "alignas",    "alignof",      "auto",
            "bool",         "break",    "case",     "catch",     "char",       "class",        "co_await",
            "co_return",    "co_yield", "concept",  "const",     "const_cast", "consteval",    "constexpr",
            "constinit",    "continue", "decltype", "default",   "delete",     "do",           "double",
            "dynamic_cast", "else",     "enum",     "explicit",  "export",     "extern",       "float",
            "for",          "friend",   "goto",     "if",        "import",     "inline",       "int",
            "long",         "module",   "mutable",  "namespace", "new",        "noexcept",     "nullptr",
            "operator",     "override", "private",  "protected", "public",     "register",     "reinterpret_cast",
            "requires",     "return",   "short",    "signed",    "sizeof",     "static",       "static_assert",
            "static_cast",  "struct",   "switch",   "template",  "this",       "thread_local", "throw",
            "try",          "typedef",  "typeid",   "typename",  "union",      "unsigned",     "using",
            "virtual",      "void",     "volatile", "while",
        })
    );

    constexpr auto kCSharp = sorted(
        std::to_array<std::string_view>({
            "abstract",  "as",       "async",     "await",   "base",     "bool",       "break",    "byte",
            "case",      "catch",    "char",      "checked", "class",    "const",      "continue", "decimal",
            "default",   "delegate", "do",        "double",  "else",     "enum",       "event",    "explicit",
            "extern",    "finally",  "fixed",     "float",   "for",      "foreach",    "get",      "goto",
            "if",        "implicit", "in",        "init",    "int",      "interface",  "internal", "is",
            "lock",      "long",     "namespace", "new",     "object",   "operator",   "out",      "override",
            "params",    "private",  "protected", "public",  "readonly", "record",     "ref",      "return",
            "sbyte",     "sealed",   "set",       "short",   "sizeof",   "stackalloc", "static",   "string",
            "struct",    "switch",   "this",      "throw",   "try",      "typeof",     "uint",     "ulong",
            "unchecked", "unsafe",   "ushort",    "using",   "var",      "virtual",    "void",     "volatile",
            "while",     "yield",
        })
    );

    constexpr auto kJava = sorted(
        std::to_array<std::string_view>({
            "abstract",  "assert",     "boolean",   "break",     "byte",         "case",   "catch",  "char",
            "class",     "const",      "continue",  "default",   "do",           "double", "else",   "enum",
            "extends",   "final",      "finally",   "float",     "for",          "goto",   "if",     "implements",
            "import",    "instanceof", "int",       "interface", "long",         "native", "new",    "package",
            "permits",   "private",    "protected", "public",    "record",       "return", "sealed", "short",
            "static",    "strictfp",   "super",     "switch",    "synchronized", "this",   "throw",  "throws",
            "transient", "try",        "var",       "void",      "volatile",     "while",  "yield",
        })
    );

    constexpr auto kKotlin = sorted(
        std::to_array<std::string_view>({
            "abstract",  "as",       "break",  "by",       "class",  "companion", "constructor", "continue", "data",
            "do",        "else",     "enum",   "for",      "fun",    "if",        "import",      "in",       "init",
            "interface", "internal", "is",     "lateinit", "object", "open",      "override",    "package",  "private",
            "protected", "public",   "return", "sealed",   "super",  "suspend",   "this",        "throw",    "try",
            "typealias", "val",      "var",    "when",     "while",
        })
    );

    constexpr auto kEcmaScript = sorted(
        std::to_array<std::string_view>({
            "async",    "await",    "break",  "case",   "catch",  "class",      "const",   "continue",
            "debugger", "default",  "delete", "do",     "else",   "export",     "extends", "finally",
            "for",      "function", "if",     "import", "in",     "instanceof", "let",     "new",
            "of",       "return",   "static", "super",  "switch", "this",       "throw",   "try",
            "typeof",   "var",      "void",   "while",  "with",   "yield",
        })
    );

    constexpr auto kTypeScript = sorted(
        std::to_array<std::string_view>({
            "abstract",  "any",        "as",     "async",    "await",      "boolean",   "break",     "case",
            "catch",     "class",      "const",  "continue", "debugger",   "declare",   "default",   "delete",
            "do",        "else",       "enum",   "export",   "extends",    "finally",   "for",       "function",
            "if",        "implements", "import", "in",       "instanceof", "interface", "keyof",     "let",
            "namespace", "never",      "new",    "number",   "of",         "private",   "protected", "public",
            "readonly",  "return",     "static", "string",   "super",      "switch",    "this",      "throw",
            "try",       "type",       "typeof", "unknown",  "var",        "void",      "while",     "with",
            "yield",
        })
    );

    constexpr auto kRust = sorted(
        std::to_array<std::string_view>({
            "Self",   "as",     "async",  "await", "break", "const",  "continue", "crate",  "dyn",
            "else",   "enum",   "extern", "fn",    "for",   "if",     "impl",     "in",     "let",
            "loop",   "match",  "mod",    "move",  "mut",   "pub",    "ref",      "return", "self",
            "static", "struct", "super",  "trait", "type",  "unsafe", "use",      "where",  "while",
        })
    );

    constexpr auto kGo = sorted(
        std::to_array<std::string_view>({
            "break", "case",   "chan",   "const",  "continue", "default", "defer",     "else", "fallthrough",
            "for",   "func",   "go",     "goto",   "if",       "import",  "interface", "map",  "package",
            "range", "return", "select", "struct", "switch",   "type",    "var",
        })
    );

    constexpr auto kSwift = sorted(
        std::to_array<std::string_view>({
            "Self",      "actor",       "as",          "associatedtype",
            "async",     "await",       "break",       "case",
            "catch",     "class",       "continue",    "default",
            "defer",     "do",          "else",        "enum",
            "extension", "fallthrough", "fileprivate", "for",
            "func",      "guard",       "if",          "import",
            "in",        "init",        "inout",       "internal",
            "is",        "let",         "open",        "operator",
            "private",   "protocol",    "public",      "repeat",
            "return",    "self",        "static",      "struct",
            "subscript", "super",       "switch",      "throw",
            "throws",    "try",         "typealias",   "var",
            "where",     "while",
        })
    );

    constexpr auto kZig = sorted(
        std::to_array<std::string_view>({
            "addrspace",      "align",       "allowzero", "and",      "anyframe",
            "anytype",        "asm",         "async",     "await",    "break",
            "callconv",       "catch",       "comptime",  "const",    "continue",
            "defer",          "else",        "enum",      "errdefer", "error",
            "export",         "extern",      "fn",        "for",      "if",
            "inline",         "linksection", "noalias",   "noinline", "nosuspend",
            "opaque",         "or",          "orelse",    "packed",   "pub",
            "resume",         "return",      "struct",    "suspend",  "switch",
            "test",           "threadlocal", "try",       "union",    "unreachable",
            "usingnamespace", "var",         "volatile",  "while",
        })
    );

    constexpr auto kPhp = sorted(
        std::to_array<std::string_view>({
            "abstract",  "and",      "array",    "as",        "break",        "case",    "catch",      "class",
            "clone",     "const",    "continue", "declare",   "default",      "do",      "echo",       "else",
            "elseif",    "empty",    "enum",     "extends",   "final",        "finally", "fn",         "for",
            "foreach",   "function", "global",   "if",        "implements",   "include", "instanceof", "interface",
            "isset",     "list",     "match",    "namespace", "new",          "or",      "print",      "private",
            "protected", "public",   "readonly", "require",   "require_once", "return",  "static",     "switch",
            "throw",     "trait",    "try",      "unset",     "use",          "var",     "while",      "yield",
        })
    );

    constexpr auto kPython = sorted(
        std::to_array<std::string_view>({
            "and",  "as",     "assert", "async", "await",  "break",   "case",     "class", "continue",
            "def",  "del",    "elif",   "else",  "except", "finally", "for",      "from",  "global",
            "if",   "import", "in",     "is",    "lambda", "match",   "nonlocal", "not",   "or",
            "pass", "raise",  "return", "try",   "while",  "with",    "yield",
        })
    );

    constexpr auto kRuby = sorted(
        std::to_array<std::string_view>({
            "BEGIN", "END",   "alias",  "and",   "begin",  "break", "case",   "class", "def",
            "do",    "else",  "elsif",  "end",   "ensure", "for",   "if",     "in",    "module",
            "next",  "not",   "or",     "redo",  "rescue", "retry", "return", "self",  "super",
            "then",  "undef", "unless", "until", "when",   "while", "yield",
        })
    );

    constexpr auto kLua = sorted(
        std::to_array<std::string_view>({
            "and", "break", "do",  "else", "elseif", "end",    "for",  "function", "goto",  "if",
            "in",  "local", "not", "or",   "repeat", "return", "then", "until",    "while",
        })
    );

    constexpr auto kShell = sorted(
        std::to_array<std::string_view>({
            "case",     "declare", "do", "done",  "elif",     "else",   "esac",   "export", "fi",    "for",
            "function", "if",      "in", "local", "readonly", "return", "select", "then",   "until", "while",
        })
    );

    constexpr auto kFish = sorted(
        std::to_array<std::string_view>({
            "and",
            "begin",
            "break",
            "case",
            "continue",
            "else",
            "end",
            "for",
            "function",
            "if",
            "in",
            "not",
            "or",
            "return",
            "set",
            "switch",
            "while",
        })
    );

    constexpr auto kNix = sorted(
        std::to_array<std::string_view>({
            "assert",
            "else",
            "if",
            "import",
            "in",
            "inherit",
            "let",
            "or",
            "rec",
            "then",
            "with",
        })
    );

    constexpr auto kHaskell = sorted(
        std::to_array<std::string_view>({
            "as",       "case",    "class",  "data",    "default", "deriving",  "do",    "else",   "family",
            "forall",   "foreign", "hiding", "if",      "import",  "in",        "infix", "infixl", "infixr",
            "instance", "let",     "module", "newtype", "of",      "qualified", "then",  "type",   "where",
        })
    );

    constexpr auto kSql = sorted(
        std::to_array<std::string_view>({
            "add",        "alter",      "and",    "as",      "asc",     "between", "by",    "case",     "check",
            "column",     "constraint", "create", "cross",   "default", "delete",  "desc",  "distinct", "drop",
            "else",       "end",        "exists", "foreign", "from",    "full",    "group", "having",   "if",
            "in",         "index",      "inner",  "insert",  "into",    "is",      "join",  "key",      "left",
            "like",       "limit",      "not",    "offset",  "on",      "or",      "order", "outer",    "primary",
            "references", "right",      "select", "set",     "table",   "then",    "union", "unique",   "update",
            "values",     "view",       "when",   "where",   "with",
        })
    );

    constexpr auto kMeson = sorted(
        std::to_array<std::string_view>({
            "and",
            "break",
            "continue",
            "elif",
            "else",
            "endforeach",
            "endif",
            "foreach",
            "if",
            "in",
            "not",
            "or",
        })
    );

    constexpr auto kCMake = sorted(
        std::to_array<std::string_view>({
            "break",    "continue", "else",    "elseif",   "endforeach", "endfunction", "endif",
            "endmacro", "endwhile", "foreach", "function", "if",         "include",     "macro",
            "option",   "project",  "return",  "set",      "unset",      "while",
        })
    );

    constexpr auto kMake = sorted(
        std::to_array<std::string_view>({
            "define",
            "else",
            "endef",
            "endif",
            "export",
            "ifdef",
            "ifeq",
            "ifndef",
            "ifneq",
            "include",
            "override",
            "unexport",
            "vpath",
        })
    );

    constexpr auto kDockerfile = sorted(
        std::to_array<std::string_view>({
            "ADD",   "ARG",        "AS",      "CMD", "COPY",  "ENTRYPOINT", "ENV",  "EXPOSE", "FROM",    "HEALTHCHECK",
            "LABEL", "MAINTAINER", "ONBUILD", "RUN", "SHELL", "STOPSIGNAL", "USER", "VOLUME", "WORKDIR",
        })
    );

    constexpr auto kCss = sorted(
        std::to_array<std::string_view>({
            "!default", "!important", "@charset", "@container", "@each",    "@else",      "@extend", "@for",
            "@forward", "@function",  "@if",      "@import",    "@include", "@keyframes", "@layer",  "@media",
            "@mixin",   "@page",      "@return",  "@supports",  "@use",     "@while",
        })
    );
  } // namespace

  KeywordSet keywords(Language language) {
    switch (language) {
    case Language::C:
      return {.words = kC};
    case Language::Cpp:
      return {.words = kCpp};
    case Language::CSharp:
      return {.words = kCSharp};
    case Language::Java:
      return {.words = kJava};
    case Language::Kotlin:
      return {.words = kKotlin};
    case Language::EcmaScript:
      return {.words = kEcmaScript};
    case Language::TypeScript:
      return {.words = kTypeScript};
    case Language::Rust:
      return {.words = kRust};
    case Language::Go:
      return {.words = kGo};
    case Language::Swift:
      return {.words = kSwift};
    case Language::Zig:
      return {.words = kZig};
    case Language::Php:
      return {.words = kPhp};
    case Language::Python:
      return {.words = kPython};
    case Language::Ruby:
      return {.words = kRuby};
    case Language::Lua:
      return {.words = kLua};
    case Language::Shell:
      return {.words = kShell};
    case Language::Fish:
      return {.words = kFish};
    case Language::Nix:
      return {.words = kNix};
    case Language::Haskell:
      return {.words = kHaskell};
    case Language::Sql:
      return {.words = kSql, .caseless = true};
    case Language::Meson:
      return {.words = kMeson};
    case Language::CMake:
      return {.words = kCMake, .caseless = true};
    case Language::Make:
      return {.words = kMake};
    case Language::Dockerfile:
      return {.words = kDockerfile};
    case Language::Css:
    case Language::Scss:
      return {.words = kCss};
    default:
      return {};
    }
  }
} // namespace luciole::formats
