"""Generates C++ from a `.wlb` grammar file (docs/plans/grammar-idl.md).

From `harness/grammar/<name>.wlb` it writes `<out>/<name>.wlb.h` and `<out>/<name>.wlb.cc`:
the types (an enum per `words`, a struct per `struct`, a struct per `choice` holding a
std::variant of one struct per arm), and for each struct, choice and words type
`std::optional<T> Parse<T>(std::string_view)` and `std::string Format<T>(const T&)`, plus
`<Name>Grammar(root, env)`: the GBNF for a type and everything it uses. Matching and GBNF
emission are in the hand-written runtime, harness/grammar/wlb_runtime.{h,cc}; the generated
code is tables of sequences and the code that fills and prints the types.

Usage: python3 -I -B tools/grammar/wlb_gen.py <file.wlb> <out_dir>
       python3 -I -B tools/grammar/wlb_gen.py --rules <file.wlb> <RULES.md> [--fix]

With --rules it checks (or with --fix rewrites) the reply form in RULES.md: the lines between
the paragraph starting "Reply in exactly this form" (up to its ":\n") and "\n\nExample:" are
one per documented line of the file's `lines` type, in its order. It fails when the anchors
are missing and when an optional line's key is not named (in backquotes) in that paragraph.
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
    (?P<punct>[{}()?;=,:<>~*@])
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


def read_hook(r):
    """`@parse(Fn)` after a declaration, or None."""
    if r.at("punct", "@") and r.peek(1)[1] == "parse":
        r.take()
        r.take("ident", "parse")
        r.take("punct", "(")
        fn = r.take("ident")
        r.take("punct", ")")
        return fn
    return None


def read_line(r, path):
    """One line of a `lines` block:
    key[?|*N] [<field:Type ["Label"]>]: field:Type|field:list<...> [default "x"] [lenient]
    [@normalize(Fn)] [@needs(list)] ["description"];"""
    line = {"key": r.take("ident"), "mode": "required", "max": 1, "key_field": None,
            "default": None, "lenient": False, "normalize": None, "needs": None,
            "doc": None}
    if r.at("punct", "?"):
        r.take()
        line["mode"] = "optional"
    elif r.at("punct", "*"):
        r.take()
        line["mode"] = "repeat"
        line["max"] = int(r.take("number"))
    if r.at("punct", "<"):
        r.take()
        name = r.take("ident")
        r.take("punct", ":")
        line["key_field"] = {"name": name, "type": r.take("ident"), "label": None}
        if r.at("string"):
            line["key_field"]["label"] = r.take("string")  # What RULES.md shows: <Label>.
        r.take("punct", ">")
    r.take("punct", ":")
    value = read_seq_one(r)
    if value["kind"] not in ("field", "list"):
        raise WlbError(f"{path}: line {line['key']}: the value must be one field")
    line["value"] = value
    while not r.at("punct", ";"):
        if r.at("ident", "default"):
            r.take()
            line["default"] = r.take("string")
        elif r.at("ident", "lenient"):
            r.take()
            line["lenient"] = True
        elif r.at("punct", "@"):
            r.take()
            what = r.take("ident")
            r.take("punct", "(")
            line[{"normalize": "normalize", "needs": "needs"}[what]] = r.take("ident")
            r.take("punct", ")")
        elif r.at("string"):
            line["doc"] = r.take("string")
        else:
            tok = r.peek()
            raise WlbError(f"{path}:{tok[2]}: unexpected {tok[1]!r} in line {line['key']}")
    r.take("punct", ";")
    return line


def read_seq_one(r):
    """One field (`name:Type` or `name:list<...>`) of a line."""
    elems = []
    name = r.take("ident")
    r.take("punct", ":")
    if r.at("ident", "list"):
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
        return {"kind": "list", "name": name, "type": item, "sep": sep, "min": lo, "max": hi}
    return {"kind": "field", "name": name, "type": r.take("ident")}


def read_wlb(path, imported=False):
    r = Reader(tokenize(pathlib.Path(path).read_text(), path), path)
    types, env, imports = {}, {}, []
    while not r.at("end"):
        what = r.take("ident")
        if what == "import":
            other = pathlib.Path(path).parent / r.take("string")
            other_types, other_env, _ = read_wlb(other, imported=True)
            for name, t in other_types.items():
                types[name] = dict(t, imported=t.get("imported") or other.stem)
            for field, kind in other_env.items():
                env.setdefault(field, kind)
            imports.append(other.stem)
            continue
        if what == "env":
            r.take("ident")
            r.take("punct", "{")
            while not r.at("punct", "}"):
                field = r.take("ident")
                r.take("punct", ":")
                kind = r.take("ident")
                if kind not in ("names", "count"):
                    raise WlbError(f"{path}: env field {field}: `names` or `count`")
                r.take("punct", ";")
                env[field] = kind
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
            hi_env = None
            if r.at("ident"):
                hi_env = r.take("ident")  # A count the call passes in.
                if env.get(hi_env) != "count":
                    raise WlbError(f"{path}: {name}: no env count {hi_env}")
                hi = 99999
            else:
                hi = int(r.take("number"))
            decimals = 0
            if what == "num" and r.at("number"):
                decimals = int(r.take("number"))
                r.take("ident", "dp")
            types[name] = {"kind": what, "lo": lo, "hi": hi, "hi_env": hi_env,
                           "decimals": decimals, "hook": read_hook(r)}
        elif what == "name":
            r.take("ident", "from")
            source = r.take("ident")
            if env.get(source) != "names":
                raise WlbError(f"{path}: name {name}: no env list {source}")
            types[name] = {"kind": "name", "env": source, "hook": read_hook(r)}
        elif what == "text":
            r.take("ident", "max")
            max_bytes = int(r.take("number"))
            forbid = ""
            if r.at("ident", "no"):
                r.take()
                forbid = r.take("string")
            source = None
            if r.at("ident", "from"):
                r.take()
                source = r.take("ident")
                if env.get(source) != "names":
                    raise WlbError(f"{path}: text {name}: no env list {source}")
            types[name] = {"kind": "text", "max": max_bytes, "forbid": forbid,
                           "env": source}
        elif what == "lines":
            r.take("punct", "{")
            lines = []
            while not r.at("punct", "}"):
                lines.append(read_line(r, path))
            r.take("punct", "}")
            types[name] = {"kind": "lines", "lines": lines}
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
    if not imported:
        check(types, path)
    return types, env, imports


def fields(seq, optional=False):
    """Every field, flag and list of a sequence, with whether it sits in an optional."""
    for e in seq:
        if e["kind"] in ("field", "flag", "list"):
            yield e, optional
        elif e["kind"] == "opt":
            yield from fields(e["body"], True)


def check(types, path):
    for name, t in types.items():
        if t["kind"] == "lines":
            for line in t["lines"]:
                for e in [line["value"]] + ([line["key_field"]] if line["key_field"] else []):
                    if e["type"] not in types:
                        raise WlbError(f"{path}: {name}.{line['key']}: unknown type "
                                       f"{e['type']}")
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


def gbnf_range(lo, hi):
    """GBNF for exactly the whole numbers lo..hi (no leading zeros)."""
    if hi - lo + 1 <= 40:
        return " | ".join(f'"{n}"' for n in range(lo, hi + 1))
    alts = []
    for digits in range(len(str(lo)), len(str(hi)) + 1):
        first = 0 if digits == 1 else 10 ** (digits - 1)
        last = 10 ** digits - 1
        a, b = max(lo, first), min(hi, last)
        if a > b:
            continue
        if (a, b) == (first, last):
            head = "[0-9]" if digits == 1 else "[1-9]"
            alts.append(head + " [0-9]" * (digits - 1))
        elif digits == 1:
            alts.append(f"[{a}-{b}]")
        elif b - a + 1 <= 200:
            alts += [f'"{n}"' for n in range(a, b + 1)]
        else:
            raise WlbError(f"range {lo}..{hi}: can't write {a}..{b} in GBNF yet")
    return " | ".join(alts)


def gbnf_class(forbid):
    """`[^...]`: any character but these (GBNF escapes)."""
    out = "[^"
    for c in forbid:
        if c == "\n":
            out += "\\n"
        elif c in "]\\^-":
            out += "\\" + c
        else:
            out += c
    return out + "]"


class Gen:
    def __init__(self, types, env, stem, wlb_path, imports=()):
        self.types, self.env, self.stem, self.wlb_path = types, env, stem, wlb_path
        self.imports = list(imports)
        self.title = stem[:1].upper() + stem[1:]
        self.ns = "::harness::grammar::"

    def own(self, name):
        return not self.types[name].get("imported")

    def rule(self, type_name):
        stem = self.types[type_name].get("imported") or self.stem
        return f"{stem}-{type_name}"

    def hooks(self):
        """The hand-written functions this file names (grammar/<stem>_hooks.h)."""
        out = []
        for name, t in self.types.items():
            if self.own(name) and t.get("hook"):
                out.append(t["hook"])
            if t["kind"] == "lines" and self.own(name):
                out += [l["normalize"] for l in t["lines"] if l["normalize"]]
        return out

    def bounded(self, name, seen=None):
        """Whether values of `name` hold a number bounded by an env count."""
        t = self.types[name]
        if t["kind"] == "int":
            return bool(t.get("hi_env"))
        seen = seen or set()
        if name in seen:
            return False
        seen = seen | {name}
        seqs = [t["seq"]] if t["kind"] == "struct" else (
            [a["seq"] for a in t["arms"]] if t["kind"] == "choice" else [])
        return any(self.bounded(e["type"], seen) for s in seqs for e, _ in fields(s)
                   if e["kind"] != "flag")

    def scalar(self, type_name):
        """The C++ type of a field of `type_name`."""
        k = self.types[type_name]["kind"]
        return {"int": "int", "num": "double", "name": "std::string",
                "text": "std::string"}.get(k, self.ns + type_name)

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
        return {"int": " = 0", "num": " = 0", "words": " = {}"}.get(k, "")

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
             '#include "grammar/wlb_runtime.h"']
        h += [f'#include "grammar/{imp}.wlb.h"' for imp in self.imports]
        h += ["", "namespace harness::grammar {", ""]
        for name in order_types(self.types):
            t = self.types[name]
            if not self.own(name):
                continue
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
        for name, t in self.types.items():
            if t["kind"] == "lines" and self.own(name):
                h += self.lines_header(name, t)
        for name in order_types(self.types):
            if not self.own(name):
                continue
            if self.types[name]["kind"] in ("words", "struct", "choice"):
                h += [f"std::optional<{name}> Parse{name}(std::string_view text);",
                      "// The same, preferring the names `env` offered where a name could",
                      "// end in more than one place.",
                      f"std::optional<{name}> Parse{name}(std::string_view text, "
                      "const rt::Env& env);",
                      f"std::string Format{name}(const {name}& value);"]
            if self.types[name]["kind"] in ("words", "struct", "choice"):
                h.append(f"// For tests: a random valid value (round trips, GBNF checks).")
                h.append(f"{name} Random{name}(rt::RandomSource* source);")
            if self.types[name]["kind"] in ("struct", "choice"):
                h.append(f"// Every `name` value in `value`, in order (what its GBNF needs).")
                h.append(f"void CollectNames(const {name}& value, "
                         "std::vector<std::string>* out);")
        envs = ", ".join(self.env)
        h += ["",
              f"// The GBNF for `root` (a type of this file) and everything it uses; env:",
              f"// {envs}. \"\" when `root` needs a list that is empty.",
              f"std::string {self.title}Grammar(std::string_view root, const rt::Env& env);",
              "// This file's GBNF rules (for a file that imports it).",
              f"const std::vector<const rt::Rule*>& {self.title}Rules();",
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
            t = self.types[e["type"]]
            names = (f", \"{t['env']}\"" if t["kind"] == "name" and t.get("env") else "")
            return (f"rt::Field(&Is{e['type']}, \"{self.rule(e['type'])}\", "
                    f"{ids[e['name']]}{names})")
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
                        f"Parse{e['type']}(c.span, &{out}->{m}.back(), env);")
            elif opt:
                body = f"{out}->{m}.emplace(); Parse{e['type']}(c.span, &*{out}->{m}, env);"
            else:
                body = f"Parse{e['type']}(c.span, &{out}->{m}, env);"
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

    def random_seq(self, v, seq, indent, opt=False):
        """Code giving `v`'s fields of `seq` random values; an optional group is
        present or absent as a whole."""
        lines = []
        for e in seq:
            k = e["kind"]
            if k == "flag":
                lines.append(f"{indent}{v}.{member(e['name'])} = source->Coin();")
            elif k == "field":
                value = f"Random{e['type']}(source)"
                lines.append(f"{indent}{v}.{member(e['name'])} = {value};")
            elif k == "list":
                m = f"{v}.{member(e['name'])}"
                lines.append(
                    f"{indent}for (int i = 0, n = {e['min']} + source->Below("
                    f"{e['max'] - e['min'] + 1}); i < n; ++i) {{ {m}.push_back("
                    f"Random{e['type']}(source)); }}")
            elif k == "opt":
                lines.append(f"{indent}if (source->Coin()) {{")
                lines += self.random_seq(v, e["body"], indent + "  ", True)
                lines.append(f"{indent}}}")
        return lines

    def rule_spec(self, rule_name):
        return {"name": f"\"{rule_name}\"", "alts": "{}", "raw": "\"\"", "env": "\"\"",
                "valid": "nullptr", "count": "\"\"", "count_from": "1", "needs": "\"\""}

    # -- lines --

    def line_member_type(self, line):
        v = line["value"]
        item = self.scalar(v["type"])
        if v["kind"] == "list" or line["mode"] == "repeat":
            return f"std::vector<{item}>"
        return item

    def lines_header(self, name, t):
        h = [f"struct {name} {{"]
        for line in t["lines"]:
            if line["key_field"]:
                kf = line["key_field"]
                h.append(f"  {self.scalar(kf['type'])} {member(kf['name'])};")
            mt = self.line_member_type(line)
            init = " = 0" if mt == "int" else ""
            h.append(f"  {mt} {member(line['value']['name'])}{init};")
        h += [f"  bool operator==(const {name}&) const = default;", "};", "",
              "// Reads a reply: lines in any order, keys in any case, unknown lines",
              "// ignored; a bad optional line is dropped, a bad required one (or a",
              "// missing one) fails it. `env` holds the counts (option numbers).",
              f"std::optional<{name}> Parse{name}(std::string_view text, const rt::Env& env);"]
        first = t["lines"][0]
        h += ["// Reads the first line alone, as soon as it has streamed in.",
              f"std::optional<{self.line_member_type(first)}> Parse{name}FirstLine(",
              "    std::string_view line, const rt::Env& env);",
              f"std::string Format{name}(const {name}& value);",
              "// For tests: a random valid reply (round trips, GBNF checks).",
              f"{name} Random{name}(rt::RandomSource* source);", ""]
        return h

    def parse_into(self, type_name, src, dst):
        """A C++ condition: `src` parses as `type_name` into `dst`."""
        if self.own(type_name):
            return f"Parse{type_name}({src}, &{dst}, &env)"
        return f"rt::Assign(Parse{type_name}({src}, env), &{dst})"

    def append_value(self, type_name, v):
        if self.own(type_name):
            return f"Append{type_name}({v}, &out);"
        return f"out.append(Format{type_name}({v}));"

    def line_fn(self, name, line):
        return f"Parse{name}Line{line['key'][:1].upper() + line['key'][1:]}"

    def lines_source(self, name, t):
        c, rules = [], []
        for line in t["lines"]:
            v = line["value"]
            item = self.scalar(v["type"])
            fn = self.line_fn(name, line)
            out_t = self.line_member_type(line) if line["mode"] != "repeat" else item
            within = (f" && Within{v['type']}(x, env)" if self.bounded(v["type"]) else "")
            c.append(f"bool {fn}(std::string_view raw, const rt::Env& env, {out_t}* out) {{")
            c.append("  (void)env;")
            if line["normalize"]:
                c.append(f"  const std::string value = hooks::{line['normalize']}(raw);")
            else:
                c.append("  const std::string value(raw);")
            if v["kind"] == "list":
                sep = v["sep"].strip() or v["sep"]
                c.append(f"  std::vector<{item}> items;")
                c.append(f"  for (std::string_view piece : rt::SplitItems(value, "
                         f"{cpp_string(sep)})) {{")
                c.append(f"    {item} x;")
                if line["lenient"]:
                    c += [f"    if (!({self.parse_into(v['type'], 'piece', 'x')}{within})) "
                          "continue;  // A bad item is dropped.",
                          "    if (std::find(items.begin(), items.end(), x) == items.end()) "
                          "items.push_back(std::move(x));", "  }"]
                else:
                    c += [f"    if (!({self.parse_into(v['type'], 'piece', 'x')}{within})) "
                          "return false;", "    items.push_back(std::move(x));", "  }",
                          f"  if (items.size() < {v['min']} || items.size() > {v['max']}) "
                          "return false;"]
                c += ["  *out = std::move(items);", "  return true;", "}", ""]
            else:
                c += [f"  {item} x;",
                      f"  if (!({self.parse_into(v['type'], 'value', 'x')}{within})) "
                      "return false;", "  *out = std::move(x);", "  return true;", "}", ""]
        # GBNF: a rule per line, then the reply.
        root_seq = []
        for i, line in enumerate(t["lines"]):
            v = line["value"]
            key = line["key"]
            rule_name = f"{self.stem}-line-{key}"
            fn = f"RuleLine{key[:1].upper() + key[1:]}"
            head = "" if i == 0 else "\n"
            nodes = []
            if line["key_field"]:
                kf = line["key_field"]
                nodes += [f"rt::Lit({cpp_string(head + key + ' ')})",
                          f"rt::Field(nullptr, \"{self.rule(kf['type'])}\", 0)",
                          "rt::Lit(\": \")"]
            else:
                nodes.append(f"rt::Lit({cpp_string(head + key + ': ')})")
            if v["kind"] == "list":
                value_node = (f"rt::List(nullptr, \"{self.rule(v['type'])}\", "
                              f"{cpp_string(v['sep'])}, 0, {v['min']}, {v['max']})")
            else:
                value_node = f"rt::Field(nullptr, \"{self.rule(v['type'])}\", 0)"
            if line["default"] is not None and line["mode"] == "required":
                # Always written: the default, or a value.
                vfn, vrule = fn + "Value", rule_name + "-value"
                spec = self.rule_spec(vrule)
                spec["alts"] = (f"{{rt::Seq{{rt::Lit({cpp_string(line['default'])})}}, "
                                f"rt::Seq{{{value_node}}}}}")
                c += self.rule_fn(vfn, spec)
                rules.append(vfn)
                value_node = f"rt::Field(nullptr, \"{vrule}\", 0)"
            nodes.append(value_node)
            spec = self.rule_spec(rule_name)
            spec["alts"] = f"{{rt::Seq{{{', '.join(nodes)}}}}}"
            if line["needs"]:
                spec["needs"] = f"\"{line['needs']}\""
            c += self.rule_fn(fn, spec)
            rules.append(fn)
            ref = f"rt::Field(nullptr, \"{rule_name}\", 0)"
            if line["mode"] == "optional":
                root_seq.append(f"rt::Opt({{{ref}}})")
            elif line["mode"] == "repeat":
                root_seq.append(f"rt::List(nullptr, \"{rule_name}\", \"\", 0, 0, "
                                f"{line['max']})")
            else:
                root_seq.append(ref)
        spec = self.rule_spec(self.rule(name))
        spec["alts"] = f"{{rt::Seq{{{', '.join(root_seq)}}}}}"
        c += self.rule_fn(f"Rule{name}", spec)
        rules.append(f"Rule{name}")
        return c, rules

    def rule_fn(self, fn, spec):
        return [f"const rt::Rule& {fn}() {{",
                "  // Leaked on purpose: no exit-time destructor.",
                "  static const rt::Rule* const rule = new rt::Rule{" +
                ", ".join(f".{k} = {v}" for k, v in spec.items()) + "};",
                "  return *rule;", "}", ""]

    def lines_public(self, name, t):
        c = [f"std::optional<{name}> Parse{name}(std::string_view text, const rt::Env& env) {{",
             f"  {name} result;"]
        required = [l for l in t["lines"] if l["mode"] == "required" and l["default"] is None]
        for l in required:
            c.append(f"  bool seen_{l['key']} = false;")
        c += ["  for (std::string_view line : rt::SplitLines(text)) {",
              "    std::string_view key, raw;",
              "    if (!rt::SplitLine(line, &key, &raw)) continue;"]
        for line in t["lines"]:
            v = line["value"]
            m = member(v["name"])
            fn = self.line_fn(name, line)
            fails = line["mode"] == "required"  # Required (or always-written) lines.
            on_bad = "return std::nullopt;" if fails else "continue;"
            if line["key_field"]:
                kf = line["key_field"]
                c += [f"    if (std::string_view field; rt::KeyWithField(key, "
                      f"{cpp_string(line['key'])}, &field)) {{",
                      f"      {self.scalar(kf['type'])} key_value;",
                      f"      if (!{self.parse_into(kf['type'], 'field', 'key_value')}) "
                      f"{on_bad}"]
            else:
                c.append(f"    if (rt::EqualsIgnoreCase(key, {cpp_string(line['key'])})) {{")
            if line["default"] is not None:
                c.append(f"      if (rt::EqualsIgnoreCase(raw, {cpp_string(line['default'])})) "
                         f"{{ result.{m} = {{}}; continue; }}")
            if line["mode"] == "repeat":
                c += [f"      {self.scalar(v['type'])} value;",
                      f"      if (!{fn}(raw, env, &value)) continue;",
                      f"      if (result.{m}.size() < {line['max']}) "
                      f"result.{m}.push_back(std::move(value));"]
            else:
                c += [f"      {self.line_member_type(line)} value;",
                      f"      if (!{fn}(raw, env, &value)) {on_bad}",
                      f"      result.{m} = std::move(value);"]
            if line["key_field"]:
                c.append(f"      result.{member(line['key_field']['name'])} = "
                         "std::move(key_value);")
            if fails and line["default"] is None:
                c.append(f"      seen_{line['key']} = true;")
            c += ["      continue;", "    }"]
        c.append("  }")
        if required:
            cond = " || ".join(f"!seen_{l['key']}" for l in required)
            c.append(f"  if ({cond}) return std::nullopt;")
        c += ["  return result;", "}", ""]
        first = t["lines"][0]
        c += [f"std::optional<{self.line_member_type(first)}> Parse{name}FirstLine(",
              "    std::string_view line, const rt::Env& env) {",
              "  line = rt::Trim(line);",
              "  std::string_view key, raw;",
              "  if (line.find('\\n') != line.npos || !rt::SplitLine(line, &key, &raw) ||",
              f"      !rt::EqualsIgnoreCase(key, {cpp_string(first['key'])})) {{",
              "    return std::nullopt;", "  }",
              f"  {self.line_member_type(first)} value;",
              f"  if (!{self.line_fn(name, first)}(raw, env, &value)) return std::nullopt;",
              "  return value;", "}", ""]
        # The printer.
        c += [f"std::string Format{name}(const {name}& value) {{", "  std::string out;"]
        for i, line in enumerate(t["lines"]):
            v = line["value"]
            m = f"value.{member(v['name'])}"
            key = line["key"]
            head = "" if i == 0 else "\n"
            if line["key_field"]:
                kf = f"value.{member(line['key_field']['name'])}"
                start = [f"out.append({cpp_string(head + key + ' ')});",
                         self.append_value(line["key_field"]["type"], kf),
                         "out.append(\": \");"]
            else:
                start = [f"out.append({cpp_string(head + key + ': ')});"]
            if v["kind"] == "list":
                body = [f"for (size_t i = 0; i < {m}.size(); ++i) {{",
                        f"  if (i > 0) out.append({cpp_string(v['sep'])});",
                        "  " + self.append_value(v["type"], f"{m}[i]"), "}"]
                empty = f"{m}.empty()"
            else:
                body = [self.append_value(v["type"], m)]
                empty = f"{m}.empty()" if self.scalar(v["type"]) == "std::string" or \
                    line["mode"] == "repeat" else "false"
            if line["mode"] == "repeat":
                c += [f"  for (const auto& item : {m}) {{"] + \
                     ["    " + x for x in start] + \
                     ["    " + self.append_value(v["type"], "item"), "  }"]
            elif line["mode"] == "optional":
                c += [f"  if (!({empty})) {{"] + ["    " + x for x in start + body] + ["  }"]
            elif line["default"] is not None:
                c += ["  " + x for x in start] + \
                     [f"  if ({empty}) out.append({cpp_string(line['default'])});",
                      "  else {"] + ["    " + x for x in body] + ["  }"]
            else:
                c += ["  " + x for x in start + body]
        c += ["  return out;", "}", ""]
        # Random replies (tests): every required line, each optional one or not,
        # up to its count of a repeated one.
        c += [f"{name} Random{name}(rt::RandomSource* source) {{", f"  {name} v;"]
        for line in t["lines"]:
            v = line["value"]
            m = f"v.{member(v['name'])}"
            body = []
            if line["key_field"]:
                kf = line["key_field"]
                body.append(f"v.{member(kf['name'])} = Random{kf['type']}(source);")
            if line["mode"] == "repeat":
                body.append(f"for (int i = 0, n = source->Below({line['max'] + 1}); i < n; "
                            f"++i) {{ {m}.push_back(Random{v['type']}(source)); }}")
            elif v["kind"] == "list":
                # A lenient list drops repeats when read, so it gets none.
                push = (f"if (auto x = Random{v['type']}(source); std::find({m}.begin(), "
                        f"{m}.end(), x) == {m}.end()) {m}.push_back(x);"
                        if line["lenient"] else f"{m}.push_back(Random{v['type']}(source));")
                body.append(f"for (int i = 0, n = {v['min']} + source->Below("
                            f"{v['max'] - v['min'] + 1}); i < n; ++i) {{ {push} }}")
            else:
                body.append(f"{m} = Random{v['type']}(source);")
            if line["mode"] == "optional":
                c += ["  if (source->Coin()) {"] + ["    " + x for x in body] + ["  }"]
            else:
                c += ["  " + x for x in body]
        c += ["  return v;", "}", ""]
        return c

    def within(self, v, seq):
        """Statements returning false when a bounded field of `seq` is out of range."""
        out = []
        for e, opt in fields(seq):
            if e["kind"] == "flag" or not self.bounded(e["type"]):
                continue
            m = f"{v}.{member(e['name'])}"
            fn = f"Within{e['type']}"
            if e["kind"] == "list":
                out.append(f"for (const auto& item : {m}) if (!{fn}(item, env)) return false;")
            elif opt:
                out.append(f"if ({m} && !{fn}(*{m}, env)) return false;")
            else:
                out.append(f"if (!{fn}({m}, env)) return false;")
        return out

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
             "#include <algorithm>", "#include <cstddef>", "#include <optional>",
             "#include <string>",
             "#include <string_view>", "#include <utility>", "#include <variant>",
             "#include <vector>", "", '#include "base/strings/string_number_conversions.h"',
             '#include "grammar/wlb_runtime.h"']
        if self.hooks():
            c.append(f'#include "grammar/{self.stem}_hooks.h"')
        c += [f'#include "grammar/{imp}.wlb.h"' for imp in self.imports]
        c += ["", "namespace harness::grammar {", "namespace {", ""]
        order = [n for n in order if self.own(n)]
        # Declarations first: the types use each other.
        for name in order:
            if self.types[name]["kind"] == "lines":
                continue
            cpp = self.scalar(name) if self.types[name]["kind"] in (
                "int", "num", "name", "text") else name
            c += [f"bool Parse{name}(std::string_view s, {cpp}* out, const rt::Env* env);",
                  f"[[maybe_unused]] bool Is{name}(std::string_view s, const rt::Env* env) "
                  f"{{ return Parse{name}(s, nullptr, env); }}",
                  f"void Append{name}(const {cpp}& v, std::string* out);"]
        c.append("")
        for name in order:
            t = self.types[name]
            k = t["kind"]
            if k == "words":
                c += [f"bool Parse{name}(std::string_view s, {name}* out, const rt::Env* env) {{", "  (void)env;"]
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
            elif k == "int" and t.get("hook"):
                c += [f"bool Parse{name}(std::string_view s, int* out, const rt::Env* env) {{", "  (void)env;",
                      "  int value = 0;",
                      f"  if (!hooks::{t['hook']}(s, &value) || value < {t['lo']} || "
                      f"value > {t['hi']}) return false;",
                      "  if (out) *out = value;", "  return true;", "}", "",
                      f"void Append{name}(const int& v, std::string* out) {{",
                      "  out->append(base::NumberToString(v));", "}", ""]
            elif k == "int":
                c += [f"bool Parse{name}(std::string_view s, int* out, const rt::Env* env) {{", "  (void)env;",
                      f"  return rt::ParseIntIn(s, {t['lo']}, {t['hi']}, out);", "}", "",
                      f"void Append{name}(const int& v, std::string* out) {{",
                      "  out->append(base::NumberToString(v));", "}", ""]
            elif k == "num":
                c += [f"bool Parse{name}(std::string_view s, double* out, const rt::Env* env) {{", "  (void)env;",
                      f"  return rt::ParseNumIn(s, {t['lo']}, {t['hi']}, {t['decimals']}, "
                      "out);", "}", "",
                      f"void Append{name}(const double& v, std::string* out) {{",
                      "  out->append(rt::FormatNum(v));", "}", ""]
            elif k == "name" and t.get("hook"):
                c += [f"bool Parse{name}(std::string_view s, std::string* out, const rt::Env* env) {{", "  (void)env;",
                      "  std::string value;",
                      f"  if (!hooks::{t['hook']}(s, &value)) return false;",
                      f"  if (!rt::IsOffered(env, \"{t['env']}\", value)) return false;",
                      "  if (out) *out = std::move(value);", "  return true;", "}", "",
                      f"void Append{name}(const std::string& v, std::string* out) {{",
                      "  out->append(v);", "}", ""]
            elif k == "text":
                # Free text is cut to `max` (never splitting a character); text from
                # an env list must match a name exactly, so it is refused instead.
                cut = ("  if (s.size() > {m}) return false;\n  std::string value(s);"
                       if t["env"] else
                       "  std::string value = rt::Cut(s, {m});").format(m=t["max"])
                c += [f"bool Parse{name}(std::string_view s, std::string* out, const rt::Env* env) {{", "  (void)env;",
                      "  s = rt::Trim(s);"] + cut.split("\n") + [
                      "  if (value.empty()) return false;",
                      "  if (out) *out = std::move(value);", "  return true;", "}", "",
                      f"void Append{name}(const std::string& v, std::string* out) {{",
                      "  out->append(v);", "}", ""]
            elif k == "name":
                c += [f"bool Parse{name}(std::string_view s, std::string* out, const rt::Env* env) {{", "  (void)env;",
                      "  if (!rt::IsName(s)) return false;",
                      f"  if (!rt::IsOffered(env, \"{t['env']}\", s)) return false;",
                      "  if (out) *out = std::string(s);", "  return true;", "}", "",
                      f"void Append{name}(const std::string& v, std::string* out) {{",
                      "  out->append(v);", "}", ""]
            elif k == "struct":
                ids = self.ids(t["seq"])
                c += self.seq_table(f"Seq{name}", t["seq"], ids)
                c += [f"bool Parse{name}(std::string_view s, {name}* out, const rt::Env* env) {{", "  (void)env;",
                      "  std::vector<rt::Capture> caps;",
                      f"  if (!rt::MatchSeq(Seq{name}(), s, &caps, env)) return false;",
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
                c += [f"bool Parse{name}(std::string_view s, {name}* out, const rt::Env* env) {{", "  (void)env;",
                      "  std::vector<rt::Capture> caps;"]
                for arm in t["arms"]:
                    a = f"{name}::{arm['name']}"
                    c += [f"  if (rt::MatchSeq(Seq{name}{arm['name']}(), s, &caps, env)) {{",
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
        # Random values of the scalar types (tests).
        for name in order:
            t = self.types[name]
            if t["kind"] == "int":
                span = 9 if t.get("hi_env") else t["hi"] - t["lo"] + 1
                c += [f"[[maybe_unused]] int Random{name}(rt::RandomSource* source) {{",
                      f"  return {t['lo']} + source->Below({span});", "}", ""]
            elif t["kind"] == "num":
                c += [f"[[maybe_unused]] double Random{name}(rt::RandomSource* source) {{",
                      f"  return source->Num({t['lo']}, {t['hi']}, {t['decimals']});", "}",
                      ""]
            elif t["kind"] in ("name", "text"):
                c += [f"[[maybe_unused]] std::string Random{name}(rt::RandomSource* source) {{",
                      "  return source->Name();", "}", ""]
        # Count bounds (`1..options`): checked after parsing, against the call.
        for name in order:
            t = self.types[name]
            if t["kind"] == "lines" or not self.bounded(name):
                continue
            cpp = self.scalar(name)
            c.append(f"bool Within{name}(const {cpp}& v, const rt::Env& env) {{")
            if t["kind"] == "int":
                c.append(f"  return v <= rt::CountOf(env, \"{t['hi_env']}\");")
            else:
                seqs = [("v", t["seq"])] if t["kind"] == "struct" else []
                checks = []
                if t["kind"] == "struct":
                    checks += self.within("v", t["seq"])
                else:
                    for arm in t["arms"]:
                        body = self.within("(*arm)", arm["seq"])
                        if body:
                            a = f"{name}::{arm['name']}"
                            checks.append(f"if (const {a}* arm = std::get_if<{a}>(&v.value)) "
                                          f"{{ {' '.join(body)} }}")
                c += ["  " + x for x in checks] + ["  return true;"]
            c += ["}", ""]
        # GBNF rules.
        rules = []
        for name in order:
            t = self.types[name]
            k = t["kind"]
            if k == "lines":
                continue
            fn = f"Rule{name}"
            rules.append(fn)
            c += [f"const rt::Rule& {fn}() {{",
                  "  // Leaked on purpose: no exit-time destructor.",
                  "  static const rt::Rule* const rule = new rt::Rule{"]
            spec = self.rule_spec(self.rule(name))
            if k == "words":
                raw = " | ".join(cpp_string(w["spelling"]).replace('"', '\\"')
                                 for w in t["words"])
                spec["raw"] = f"\"{raw}\""
            elif k in ("int", "num") and t.get("hi_env"):
                spec["count"] = f"\"{t['hi_env']}\""
                spec["count_from"] = str(t["lo"])
            elif k in ("int", "num"):
                raw = gbnf_range(t["lo"], t["hi"])
                if t["decimals"]:
                    raw = f"({raw})" + (' ("." [0-9])?' if t["decimals"] == 1 else
                                        f' ("." [0-9]{{1,{t["decimals"]}}})?')
                spec["raw"] = cpp_string(raw)
            elif k == "name":
                spec["env"] = f"\"{t['env']}\""
                if t.get("hook"):
                    spec["valid"] = f"&Is{name}"
            elif k == "text" and t["env"]:
                spec["env"] = f"\"{t['env']}\""
                spec["valid"] = f"&Is{name}"
            elif k == "text":
                spec["raw"] = cpp_string(f"{gbnf_class(t['forbid'] or '\n')}{{1,{t['max']}}}")
            elif k == "struct":
                spec["alts"] = f"{{Seq{name}()}}"
            elif k == "choice":
                alts = ", ".join(f"Seq{name}{a['name']}()" for a in t["arms"])
                spec["alts"] = f"{{{alts}}}"
            c[-1] += ", ".join(f".{k} = {v}" for k, v in spec.items()) + "};"
            c += ["  return *rule;", "}", ""]
        for name in order:
            if self.types[name]["kind"] == "lines":
                lines_code, line_rules = self.lines_source(name, self.types[name])
                c += lines_code
                rules += line_rules
        c += ["}  // namespace", ""]
        for name in order:
            if self.types[name]["kind"] == "lines":
                c += self.lines_public(name, self.types[name])
        for name in order:
            if self.types[name]["kind"] in ("words", "struct", "choice"):
                c += [f"std::optional<{name}> Parse{name}(std::string_view text) {{",
                      f"  {name} value;",
                      f"  if (!Parse{name}(text, &value, nullptr)) return std::nullopt;",
                      "  return value;", "}", "",
                      f"std::optional<{name}> Parse{name}(std::string_view text, "
                      "const rt::Env& env) {",
                      f"  {name} value;",
                      f"  if (!Parse{name}(text, &value, &env)) return std::nullopt;",
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
            c += ["  (void)value;", "  (void)out;", "}", ""]
        for name in order:
            t = self.types[name]
            if t["kind"] == "words":
                c += [f"{name} Random{name}(rt::RandomSource* source) {{",
                      f"  return static_cast<{name}>(source->Below({len(t['words'])}));",
                      "}", ""]
            elif t["kind"] == "struct":
                c += [f"{name} Random{name}(rt::RandomSource* source) {{", f"  {name} v;"]
                c += self.random_seq("v", t["seq"], "  ")
                c += ["  return v;", "}", ""]
            elif t["kind"] == "choice":
                c += [f"{name} Random{name}(rt::RandomSource* source) {{", f"  {name} v;",
                      f"  switch (source->Below({len(t['arms'])})) {{"]
                for i, arm in enumerate(t["arms"]):
                    a = f"{name}::{arm['name']}"
                    c += [f"    case {i}: {{", f"      {a} arm;"]
                    c += self.random_seq("arm", arm["seq"], "      ")
                    c += ["      v.value = std::move(arm);", "      break;", "    }"]
                c += ["    default: break;", "  }", "  return v;", "}", ""]
        rule_list = ", ".join(f"&{r}()" for r in rules)
        imported = "".join(
            f"  rules.insert(rules.end(), {imp[:1].upper() + imp[1:]}Rules().begin(), "
            f"{imp[:1].upper() + imp[1:]}Rules().end());\n" for imp in self.imports)
        c += [f"std::string {self.title}RuleName(std::string_view type) {{",
              f"  return std::string(\"{self.stem}-\") + std::string(type);", "}", "",
              f"const std::vector<const rt::Rule*>& {self.title}Rules() {{",
              "  // Leaked on purpose: no exit-time destructor.",
              f"  static const std::vector<const rt::Rule*>* const rules =",
              f"      new std::vector<const rt::Rule*>{{{rule_list}}};",
              "  return *rules;", "}", "",
              f"std::string {self.title}Grammar(std::string_view root, const rt::Env& env) {{",
              f"  std::vector<const rt::Rule*> rules = {self.title}Rules();",
              imported,
              f"  return rt::EmitGbnf(rules, {self.title}RuleName(root), env);", "}", "",
              "}  // namespace harness::grammar", ""]
        return "\n".join(c)


INTRO = "Reply in exactly this form"
END = "\n\nExample:"


def reply_form(types):
    """RULES.md's reply form: `key: <description>` per documented line of the `lines` type."""
    roots = [t for t in types.values() if t["kind"] == "lines" and not t.get("imported")]
    if len(roots) != 1:
        raise WlbError(f"--rules needs exactly one `lines` type, found {len(roots)}")
    out, optional = [], []
    for line in roots[0]["lines"]:
        if line["doc"] is None:
            continue
        key = line["key"]
        if line["key_field"]:
            kf = line["key_field"]
            key += f" <{kf['label'] or kf['type']}>"
        out.append(f"{key}: {line['doc']}")
        if line["mode"] in ("optional", "repeat"):
            optional.append(line["key"])
    return out, optional


def check_rules(wlb_path, rules_path, fix):
    types, _, _ = read_wlb(wlb_path)
    form, optional = reply_form(types)
    text = rules_path.read_text()
    start = text.find(INTRO)
    if start < 0:
        raise WlbError(f"{rules_path}: no paragraph starting {INTRO!r} (reworded?)")
    body = text.find(":\n", start)
    end = text.find(END, body if body >= 0 else start)
    if body < 0 or end < 0:
        raise WlbError(f"{rules_path}: the reply form after {INTRO!r} has no ':\\n' "
                       f"or no {END.strip()!r} after it (reworded?)")
    intro = text[start:body]
    missing = [k for k in optional if f"`{k}" not in intro]
    if missing:
        raise WlbError(f"{rules_path}: the reply form's intro doesn't say when to write "
                       f"{', '.join(missing)} (name each optional key in backquotes)")
    want = "\n".join(form)
    have = text[body + 2:end]
    if have == want:
        return 0
    if fix:
        rules_path.write_text(text[:body + 2] + want + text[end:])
        print(f"wlb_gen: rewrote the reply form in {rules_path}")
        return 0
    print(f"wlb_gen: {rules_path}'s reply form differs from {wlb_path} "
          f"(tools/wlb check rules --fix). Expected:\n{want}\nFound:\n{have}", file=sys.stderr)
    return 1


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--rules":
        args = sys.argv[2:]
        fix = "--fix" in args
        args = [a for a in args if a != "--fix"]
        if len(args) != 2:
            print(__doc__)
            return 2
        try:
            return check_rules(pathlib.Path(args[0]), pathlib.Path(args[1]), fix)
        except (WlbError, OSError) as e:
            print(f"wlb_gen: {e}", file=sys.stderr)
            return 1
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    wlb_path, out_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    try:
        types, env, imports = read_wlb(wlb_path)
    except WlbError as e:
        print(f"wlb_gen: {e}", file=sys.stderr)
        return 1
    stem = wlb_path.stem
    root = pathlib.Path(__file__).resolve().parent.parent.parent
    try:
        shown = wlb_path.resolve().relative_to(root)
    except ValueError:
        shown = wlb_path.name
    gen = Gen(types, env, stem, shown, imports)
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
