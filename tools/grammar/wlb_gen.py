"""Generates C++ from a `.wlb` grammar file (docs/plans/grammar-idl.md).

From `harness/grammar/<name>.wlb` it writes `<out>/<name>.wlb.h` and `<out>/<name>.wlb.cc`:
the types (an enum per `words`, a struct per `struct`, a struct per `choice` holding a
std::variant of one struct per arm), and for each struct, choice and words type
`std::optional<T> Parse<T>(std::string_view)` and `std::string Format<T>(const T&)`, plus
`<Name>Grammar(root, env)`: the GBNF for a type and everything it uses. Matching and GBNF
emission are in the hand-written runtime, harness/grammar/wlb_runtime.{h,cc}; the generated
code is tables of sequences and the code that fills and prints the types.

Usage: python3 -I -B tools/grammar/wlb_gen.py <file.wlb> <out_dir>
"""

import pathlib
import re
import shutil
import subprocess
import sys

CPP_KEYWORDS = {"and", "or", "not", "xor", "default", "class", "struct", "int", "double",
                "delete", "new", "this", "true", "false", "switch", "case", "do", "for",
                "while", "if", "else", "return", "operator", "template", "typename", "union",
                "enum", "namespace", "static", "const", "auto", "bool", "char", "float"}

# ---- reading a .wlb file ----------------------------------------------------------------------

TOKEN = re.compile(r'''
    (?P<space>\s+) | (?P<comment>//[^\n]*) |
    (?P<string>"(?:[^"\\]|\\.)*") |
    (?P<range>\.\.) |
    (?P<number>\d+) |
    (?P<ident>[A-Za-z_][A-Za-z0-9_]*) |
    (?P<punct>[{}()?;=,:<>~])
''', re.X)


class WlbError(Exception):
    pass


def tokenize(text, path):
    tokens, pos, line = [], 0, 1
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise WlbError(f"{path}:{line}: unexpected {text[pos]!r}")
        kind = m.lastgroup
        if kind == "string":
            value = bytes(m.group()[1:-1], "utf-8").decode("unicode_escape")
            tokens.append(("string", value, line))
        elif kind not in ("space", "comment"):
            tokens.append((kind, m.group(), line))
        line += m.group().count("\n")
        pos = m.end()
    tokens.append(("end", "", line))
    return tokens


class Reader:
    def __init__(self, tokens, path):
        self.t, self.i, self.path = tokens, 0, path

    def peek(self, k=0):
        return self.t[self.i + k]

    def take(self, kind=None, value=None):
        tok = self.t[self.i]
        if (kind and tok[0] != kind) or (value is not None and tok[1] != value):
            want = value if value is not None else kind
            raise WlbError(f"{self.path}:{tok[2]}: expected {want!r}, got {tok[1]!r}")
        self.i += 1
        return tok[1]

    def at(self, kind, value=None):
        tok = self.t[self.i]
        return tok[0] == kind and (value is None or tok[1] == value)


def read_seq(r, stop):
    """A sequence up to (not including) a `stop` punctuation."""
    elems = []
    while not r.at("punct", stop) and not r.at("end"):
        if r.at("string"):
            elems.append({"kind": "lit", "text": r.take("string")})
        elif r.at("punct", "~"):
            r.take()
            elems.append({"kind": "parseonly", "text": r.take("string")})
        elif r.at("punct", "("):
            r.take()
            body = read_seq(r, ")")
            r.take("punct", ")")
            r.take("punct", "?")
            if len(body) == 1 and body[0]["kind"] == "flag":
                elems.append(body[0])
            else:
                elems.append({"kind": "opt", "body": body})
        else:
            name = r.take("ident")
            r.take("punct", ":")
            if r.at("string"):
                elems.append({"kind": "flag", "name": name, "text": r.take("string")})
            elif r.at("ident", "list"):
                r.take()
                r.take("punct", "<")
                item = r.take("ident")
                r.take("punct", ",")
                sep = r.take("string")
                r.take("punct", ",")
                lo = int(r.take("number"))
                r.take("range")
                hi = int(r.take("number"))
                r.take("punct", ">")
                elems.append({"kind": "list", "name": name, "type": item, "sep": sep,
                              "min": lo, "max": hi})
            else:
                elems.append({"kind": "field", "name": name, "type": r.take("ident")})
    return elems


def read_wlb(path):
    r = Reader(tokenize(pathlib.Path(path).read_text(), path), path)
    types, env = {}, []
    while not r.at("end"):
        what = r.take("ident")
        if what == "env":
            r.take("ident")
            r.take("punct", "{")
            while not r.at("punct", "}"):
                field = r.take("ident")
                r.take("punct", ":")
                kind = r.take("ident")
                if kind != "names":
                    raise WlbError(f"{path}: env field {field}: only `names` so far")
                r.take("punct", ";")
                env.append(field)
            r.take("punct", "}")
            continue
        name = r.take("ident")
        if name in types:
            raise WlbError(f"{path}: {name} defined twice")
        if what == "words":
            r.take("punct", "{")
            words = []
            while not r.at("punct", "}"):
                spelling = r.take("string") if r.at("string") else r.take("ident")
                aliases = []
                while r.at("punct", "~"):
                    r.take()
                    aliases.append(r.take("string"))
                words.append({"spelling": spelling, "aliases": aliases})
                if not r.at("punct", "}"):
                    r.take("punct", ",")
            r.take("punct", "}")
            types[name] = {"kind": "words", "words": words}
        elif what in ("int", "num"):
            lo = int(r.take("number"))
            r.take("range")
            hi = int(r.take("number"))
            decimals = 0
            if what == "num" and r.at("number"):
                decimals = int(r.take("number"))
                r.take("ident", "dp")
            types[name] = {"kind": what, "lo": lo, "hi": hi, "decimals": decimals}
        elif what == "name":
            r.take("ident", "from")
            source = r.take("ident")
            if source not in env:
                raise WlbError(f"{path}: name {name}: no env list {source}")
            types[name] = {"kind": "name", "env": source}
        elif what == "struct":
            r.take("punct", "=")
            seq = read_seq(r, ";")
            r.take("punct", ";")
            types[name] = {"kind": "struct", "seq": seq}
        elif what == "choice":
            r.take("punct", "{")
            arms = []
            while not r.at("punct", "}"):
                arm = r.take("ident")
                r.take("punct", "=")
                seq = read_seq(r, ";")
                r.take("punct", ";")
                arms.append({"name": arm, "seq": seq})
            r.take("punct", "}")
            types[name] = {"kind": "choice", "arms": arms}
        else:
            raise WlbError(f"{path}: unknown declaration {what!r}")
    check(types, path)
    return types, env


def fields(seq, optional=False):
    """Every field, flag and list of a sequence, with whether it sits in an optional."""
    for e in seq:
        if e["kind"] in ("field", "flag", "list"):
            yield e, optional
        elif e["kind"] == "opt":
            yield from fields(e["body"], True)


def check(types, path):
    for name, t in types.items():
        seqs = [t["seq"]] if t["kind"] == "struct" else (
            [a["seq"] for a in t["arms"]] if t["kind"] == "choice" else [])
        for seq in seqs:
            seen = set()
            for e, _ in fields(seq):
                if e["kind"] != "flag" and e["type"] not in types:
                    raise WlbError(f"{path}: {name}: unknown type {e['type']}")
                if e["name"] in seen:
                    raise WlbError(f"{path}: {name}: field {e['name']} twice")
                seen.add(e["name"])
    # No type may use itself, directly or not (no recursion yet: no Box<T>).
    def deps(name):
        t = types[name]
        seqs = [t["seq"]] if t["kind"] == "struct" else (
            [a["seq"] for a in t["arms"]] if t["kind"] == "choice" else [])
        return {e["type"] for s in seqs for e, _ in fields(s) if e["kind"] != "flag"}
    for name in types:
        stack, seen = list(deps(name)), set()
        while stack:
            d = stack.pop()
            if d == name:
                raise WlbError(f"{path}: {name} uses itself")
            if d not in seen:
                seen.add(d)
                stack.extend(deps(d))


def order_types(types):
    """Types in an order where each comes after the types it holds."""
    out, done = [], set()

    def visit(name):
        if name in done:
            return
        done.add(name)
        t = types[name]
        seqs = [t["seq"]] if t["kind"] == "struct" else (
            [a["seq"] for a in t["arms"]] if t["kind"] == "choice" else [])
        for s in seqs:
            for e, _ in fields(s):
                if e["kind"] != "flag":
                    visit(e["type"])
        out.append(name)

    for name in types:
        visit(name)
    return out

# ---- writing C++ -------------------------------------------------------------------------------


def cpp_string(text):
    out = '"'
    for c in text:
        out += {'"': '\\"', "\\": "\\\\", "\n": "\\n"}.get(c, c)
    return out + '"'


def member(name):
    return name + "_" if name in CPP_KEYWORDS else name


def enumerator(spelling):
    words = re.findall(r"[A-Za-z0-9]+", spelling)
    if not words:
        words = {"<": ["Less"], ">": ["Greater"]}.get(spelling, ["Sym"])
    return "k" + "".join(w[:1].upper() + w[1:] for w in words)


class Gen:
    def __init__(self, types, env, stem, wlb_path):
        self.types, self.env, self.stem, self.wlb_path = types, env, stem, wlb_path
        self.title = stem[:1].upper() + stem[1:]
        self.ns = "::harness::grammar::"

    def rule(self, type_name):
        return f"{self.stem}-{type_name}"

    def scalar(self, type_name):
        """The C++ type of a field of `type_name`."""
        k = self.types[type_name]["kind"]
        return {"int": "int", "num": "double", "name": "std::string"}.get(
            k, self.ns + type_name)

    def field_type(self, e, optional):
        if e["kind"] == "flag":
            return "bool"
        if e["kind"] == "list":
            return f"std::vector<{self.scalar(e['type'])}>"
        t = self.scalar(e["type"])
        return f"std::optional<{t}>" if optional else t

    def default(self, e, optional):
        if e["kind"] == "flag":
            return " = false"
        if e["kind"] == "list" or optional:
            return ""
        k = self.types[e["type"]]["kind"]
        return {"int": " = 0", "num": " = 0", "name": "", "words": " = {}"}.get(k, "")

    # -- header --

    def struct_body(self, name, seq, indent):
        lines = []
        for e, opt in fields(seq):
            lines.append(f"{indent}{self.field_type(e, opt)} {member(e['name'])}"
                         f"{self.default(e, opt)};")
        lines.append(f"{indent}bool operator==(const {name}&) const = default;")
        return lines

    def header(self):
        guard = f"HARNESS_GRAMMAR_{self.stem.upper()}_WLB_H_"
        h = [f"// Generated by tools/grammar/wlb_gen.py from {self.wlb_path}. DO NOT EDIT.",
             "", f"#ifndef {guard}", f"#define {guard}", "",
             "#include <optional>", "#include <string>", "#include <string_view>",
             "#include <variant>", "#include <vector>", "",
             '#include "grammar/wlb_runtime.h"', "", "namespace harness::grammar {", ""]
        for name in order_types(self.types):
            t = self.types[name]
            if t["kind"] == "words":
                names = ", ".join(enumerator(w["spelling"]) for w in t["words"])
                h += [f"enum class {name} {{ {names} }};", ""]
            elif t["kind"] == "struct":
                h += [f"struct {name} {{"] + self.struct_body(name, t["seq"], "  ") + ["};", ""]
            elif t["kind"] == "choice":
                # Arms are defined at namespace scope and named inside the choice
                # (`Thing::Who` is `ThingWho`): a nested struct's default member
                # initializers can't be used while the outer struct is incomplete.
                for arm in t["arms"]:
                    full = f"{name}{arm['name']}"
                    h.append(f"struct {full} {{")
                    h += self.struct_body(full, arm["seq"], "  ")
                    h += ["};", ""]
                h.append(f"struct {name} {{")
                for arm in t["arms"]:
                    h.append(f"  using {arm['name']} = {name}{arm['name']};")
                arms = ", ".join(f"{name}{a['name']}" for a in t["arms"])
                h += [f"  std::variant<{arms}> value;",
                      f"  bool operator==(const {name}&) const = default;", "};", ""]
        for name in order_types(self.types):
            if self.types[name]["kind"] in ("words", "struct", "choice"):
                h += [f"std::optional<{name}> Parse{name}(std::string_view text);",
                      f"std::string Format{name}(const {name}& value);"]
            if self.types[name]["kind"] in ("struct", "choice"):
                h.append(f"// Every `name` value in `value`, in order (what its GBNF needs).")
                h.append(f"void CollectNames(const {name}& value, "
                         "std::vector<std::string>* out);")
        envs = ", ".join(self.env)
        h += ["",
              f"// The GBNF for `root` (a type of this file) and everything it uses; env lists:",
              f"// {envs}. \"\" when `root` needs a list that is empty.",
              f"std::string {self.title}Grammar(std::string_view root, const rt::Env& env);",
              f"// The GBNF rule name of a type: \"{self.stem}-<Type>\".",
              f"std::string {self.title}RuleName(std::string_view type);",
              "", "}  // namespace harness::grammar", "", f"#endif  // {guard}", ""]
        return "\n".join(h)

    # -- source --

    def node(self, e, ids):
        k = e["kind"]
        if k == "lit":
            return f"rt::Lit({cpp_string(e['text'])})"
        if k == "parseonly":
            return f"rt::ParseOnly({cpp_string(e['text'])})"
        if k == "flag":
            return f"rt::Flag({cpp_string(e['text'])}, {ids[e['name']]})"
        if k == "field":
            return (f"rt::Field(&Is{e['type']}, \"{self.rule(e['type'])}\", "
                    f"{ids[e['name']]})")
        if k == "list":
            return (f"rt::List(&Is{e['type']}, \"{self.rule(e['type'])}\", "
                    f"{cpp_string(e['sep'])}, {ids[e['name']]}, {e['min']}, {e['max']})")
        body = ", ".join(self.node(b, ids) for b in e["body"])
        return f"rt::Opt({{{body}}})"

    def seq_table(self, fn, seq, ids):
        nodes = ",\n      ".join(self.node(e, ids) for e in seq)
        return [f"const rt::Seq& {fn}() {{",
                "  // Leaked on purpose: no exit-time destructor.",
                f"  static const rt::Seq* const seq = new rt::Seq{{\n      {nodes}}};",
                "  return *seq;", "}", ""]

    def fill(self, out, seq, ids):
        """Code filling `out->...` from `caps`."""
        lines = ["  for (const rt::Capture& c : caps) {", "    switch (c.id) {"]
        for e, opt in fields(seq):
            m, i = member(e["name"]), ids[e["name"]]
            if e["kind"] == "flag":
                body = f"{out}->{m} = true;"
            elif e["kind"] == "list":
                body = (f"{out}->{m}.emplace_back(); "
                        f"Parse{e['type']}(c.span, &{out}->{m}.back());")
            elif opt:
                body = f"{out}->{m}.emplace(); Parse{e['type']}(c.span, &*{out}->{m});"
            else:
                body = f"Parse{e['type']}(c.span, &{out}->{m});"
            lines.append(f"      case {i}: {body} break;")
        lines += ["      default: break;", "    }", "  }"]
        return lines

    def ids(self, seq):
        return {e["name"]: n for n, (e, _) in enumerate(fields(seq))}

    def present(self, v, seq):
        conds = []
        for e, _ in fields(seq):
            m = member(e["name"])
            conds.append(f"{v}.{m}" if e["kind"] == "flag" else (
                f"!{v}.{m}.empty()" if e["kind"] == "list" else f"{v}.{m}.has_value()"))
        return " || ".join(conds) or "false"

    def append_seq(self, v, seq, indent, in_opt=False):
        lines = []
        for e in seq:
            k = e["kind"]
            if k == "lit":
                lines.append(f"{indent}out->append({cpp_string(e['text'])});")
            elif k == "flag":
                lines.append(f"{indent}if ({v}.{member(e['name'])}) "
                             f"out->append({cpp_string(e['text'])});")
            elif k == "field":
                deref = "*" if in_opt else ""
                lines.append(f"{indent}Append{e['type']}({deref}{v}.{member(e['name'])}, out);")
            elif k == "list":
                m = member(e["name"])
                lines += [f"{indent}for (size_t i = 0; i < {v}.{m}.size(); ++i) {{",
                          f"{indent}  if (i > 0) out->append({cpp_string(e['sep'])});",
                          f"{indent}  Append{e['type']}({v}.{m}[i], out);", f"{indent}}}"]
            elif k == "opt":
                lines.append(f"{indent}if ({self.present(v, e['body'])}) {{")
                lines += self.append_seq(v, e["body"], indent + "  ", True)
                lines.append(f"{indent}}}")
        return lines

    def collect(self, v, seq):
        lines = []
        for e, opt in fields(seq):
            if e["kind"] == "flag":
                continue
            k = self.types[e["type"]]["kind"]
            if k not in ("name", "struct", "choice"):
                continue
            m = f"{v}.{member(e['name'])}"
            add = (lambda x: f"out->push_back({x});") if k == "name" else \
                (lambda x: f"CollectNames({x}, out);")
            if e["kind"] == "list":
                lines.append(f"  for (const auto& item : {m}) {add('item')}")
            elif opt:
                lines.append(f"  if ({m}) {add('*' + m)}")
            else:
                lines.append(f"  {add(m)}")
        return lines

    def source(self):
        order = order_types(self.types)
        c = [f"// Generated by tools/grammar/wlb_gen.py from {self.wlb_path}. DO NOT EDIT.",
             "", f'#include "grammar/{self.stem}.wlb.h"', "",
             "#include <cstddef>", "#include <optional>", "#include <string>",
             "#include <string_view>", "#include <utility>", "#include <variant>",
             "#include <vector>", "", '#include "base/strings/string_number_conversions.h"',
             '#include "grammar/wlb_runtime.h"', "", "namespace harness::grammar {",
             "namespace {", ""]
        # Declarations first: the types use each other.
        for name in order:
            cpp = self.scalar(name) if self.types[name]["kind"] in ("int", "num", "name") \
                else name
            c += [f"bool Parse{name}(std::string_view s, {cpp}* out);",
                  f"[[maybe_unused]] bool Is{name}(std::string_view s) {{ return Parse{name}(s, nullptr); }}",
                  f"void Append{name}(const {cpp}& v, std::string* out);"]
        c.append("")
        for name in order:
            t = self.types[name]
            k = t["kind"]
            if k == "words":
                c.append(f"bool Parse{name}(std::string_view s, {name}* out) {{")
                for w in t["words"]:
                    spellings = [w["spelling"]] + w["aliases"]
                    test = " || ".join(f"rt::EqualsIgnoreCase(s, {cpp_string(x)})"
                                       for x in spellings)
                    c += [f"  if ({test}) {{",
                          f"    if (out) *out = {name}::{enumerator(w['spelling'])};",
                          "    return true;", "  }"]
                c += ["  return false;", "}", "",
                      f"void Append{name}(const {name}& v, std::string* out) {{",
                      "  switch (v) {"]
                for w in t["words"]:
                    c.append(f"    case {name}::{enumerator(w['spelling'])}: "
                             f"out->append({cpp_string(w['spelling'])}); return;")
                c += ["  }", "}", ""]
            elif k == "int":
                c += [f"bool Parse{name}(std::string_view s, int* out) {{",
                      f"  return rt::ParseIntIn(s, {t['lo']}, {t['hi']}, out);", "}", "",
                      f"void Append{name}(const int& v, std::string* out) {{",
                      "  out->append(base::NumberToString(v));", "}", ""]
            elif k == "num":
                c += [f"bool Parse{name}(std::string_view s, double* out) {{",
                      f"  return rt::ParseNumIn(s, {t['lo']}, {t['hi']}, {t['decimals']}, "
                      "out);", "}", "",
                      f"void Append{name}(const double& v, std::string* out) {{",
                      "  out->append(rt::FormatNum(v));", "}", ""]
            elif k == "name":
                c += [f"bool Parse{name}(std::string_view s, std::string* out) {{",
                      "  if (!rt::IsName(s)) return false;",
                      "  if (out) *out = std::string(s);", "  return true;", "}", "",
                      f"void Append{name}(const std::string& v, std::string* out) {{",
                      "  out->append(v);", "}", ""]
            elif k == "struct":
                ids = self.ids(t["seq"])
                c += self.seq_table(f"Seq{name}", t["seq"], ids)
                c += [f"bool Parse{name}(std::string_view s, {name}* out) {{",
                      "  std::vector<rt::Capture> caps;",
                      f"  if (!rt::MatchSeq(Seq{name}(), s, &caps)) return false;",
                      "  if (!out) return true;", f"  *out = {name}();"]
                c += self.fill("out", t["seq"], ids)
                c += ["  return true;", "}", "",
                      f"void Append{name}(const {name}& v, std::string* out) {{"]
                c += self.append_seq("v", t["seq"], "  ")
                c += ["}", ""]
            elif k == "choice":
                for arm in t["arms"]:
                    c += self.seq_table(f"Seq{name}{arm['name']}", arm["seq"],
                                        self.ids(arm["seq"]))
                c += [f"bool Parse{name}(std::string_view s, {name}* out) {{",
                      "  std::vector<rt::Capture> caps;"]
                for arm in t["arms"]:
                    a = f"{name}::{arm['name']}"
                    c += [f"  if (rt::MatchSeq(Seq{name}{arm['name']}(), s, &caps)) {{",
                          "    if (!out) return true;", f"    {a} arm;"]
                    c += ["  " + line for line in self.fill("(&arm)", arm["seq"],
                                                            self.ids(arm["seq"]))]
                    c += ["    out->value = std::move(arm);", "    return true;", "  }"]
                c += ["  return false;", "}", "",
                      f"void Append{name}(const {name}& v, std::string* out) {{"]
                for arm in t["arms"]:
                    a = f"{name}::{arm['name']}"
                    if any(True for _ in fields(arm["seq"])):
                        c.append(f"  if (const {a}* arm = std::get_if<{a}>(&v.value)) {{")
                    else:
                        c.append(f"  if (std::holds_alternative<{a}>(v.value)) {{")
                    c += self.append_seq("(*arm)", arm["seq"], "    ")
                    c += ["    return;", "  }"]
                c += ["}", ""]
        # GBNF rules.
        rules = []
        for name in order:
            t = self.types[name]
            k = t["kind"]
            fn = f"Rule{name}"
            rules.append(fn)
            c += [f"const rt::Rule& {fn}() {{",
                  "  // Leaked on purpose: no exit-time destructor.",
                  "  static const rt::Rule* const rule = new rt::Rule{"]
            spec = {"name": f"\"{self.rule(name)}\"", "alts": "{}", "raw": "\"\"",
                    "env": "\"\""}
            if k == "words":
                raw = " | ".join(cpp_string(w["spelling"]).replace('"', '\\"')
                                 for w in t["words"])
                spec["raw"] = f"\"{raw}\""
            elif k in ("int", "num"):
                digits = len(str(t["hi"]))
                raw = "[0-9]" + " [0-9]?" * (digits - 1)
                if t["decimals"]:
                    raw += ' ("." [0-9])?' if t["decimals"] == 1 else \
                        f' ("." [0-9]{{1,{t["decimals"]}}})?'
                spec["raw"] = cpp_string(raw)
            elif k == "name":
                spec["env"] = f"\"{t['env']}\""
            elif k == "struct":
                spec["alts"] = f"{{Seq{name}()}}"
            elif k == "choice":
                alts = ", ".join(f"Seq{name}{a['name']}()" for a in t["arms"])
                spec["alts"] = f"{{{alts}}}"
            c[-1] += ", ".join(f".{k} = {v}" for k, v in spec.items()) + "};"
            c += ["  return *rule;", "}", ""]
        c += ["}  // namespace", ""]
        for name in order:
            if self.types[name]["kind"] in ("words", "struct", "choice"):
                c += [f"std::optional<{name}> Parse{name}(std::string_view text) {{",
                      f"  {name} value;",
                      f"  if (!Parse{name}(text, &value)) return std::nullopt;",
                      "  return value;", "}", "",
                      f"std::string Format{name}(const {name}& value) {{",
                      "  std::string out;", f"  Append{name}(value, &out);", "  return out;",
                      "}", ""]
        for name in order:
            t = self.types[name]
            if t["kind"] not in ("struct", "choice"):
                continue
            c.append(f"void CollectNames(const {name}& value, "
                     "std::vector<std::string>* out) {")
            seqs = [("value", t["seq"])] if t["kind"] == "struct" else []
            if t["kind"] == "choice":
                for arm in t["arms"]:
                    body = self.collect("(*arm)", arm["seq"])
                    if body:  # Only arms that hold names.
                        a = f"{name}::{arm['name']}"
                        c.append(f"  if (const {a}* arm = std::get_if<{a}>(&value.value)) {{")
                        c += ["  " + l for l in body]
                        c.append("  }")
            else:
                c += self.collect("value", t["seq"])
            c += ["  (void)out;", "}", ""]
        rule_list = ", ".join(f"&{r}()" for r in rules)
        c += [f"std::string {self.title}RuleName(std::string_view type) {{",
              f"  return std::string(\"{self.stem}-\") + std::string(type);", "}", "",
              f"std::string {self.title}Grammar(std::string_view root, const rt::Env& env) {{",
              f"  const std::vector<const rt::Rule*> rules = {{{rule_list}}};",
              f"  return rt::EmitGbnf(rules, {self.title}RuleName(root), env);", "}", "",
              "}  // namespace harness::grammar", ""]
        return "\n".join(c)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    wlb_path, out_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    try:
        types, env = read_wlb(wlb_path)
    except WlbError as e:
        print(f"wlb_gen: {e}", file=sys.stderr)
        return 1
    stem = wlb_path.stem
    root = pathlib.Path(__file__).resolve().parent.parent.parent
    try:
        shown = wlb_path.resolve().relative_to(root)
    except ValueError:
        shown = wlb_path.name
    gen = Gen(types, env, stem, shown)
    out_dir.mkdir(parents=True, exist_ok=True)
    outputs = {out_dir / f"{stem}.wlb.h": gen.header(), out_dir / f"{stem}.wlb.cc": gen.source()}
    fmt = shutil.which("clang-format")
    for path, text in outputs.items():
        if fmt:
            text = subprocess.run([fmt, f"--assume-filename={path.name}",
                                   f"--style=file:{root / '.clang-format'}"],
                                  input=text, capture_output=True, text=True,
                                  check=True).stdout
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
