#!/usr/bin/env python3
"""Annotates an Amiga / MC68000 program with the help of a local LLM (Ollama, e.g. Qwen3).

The model never edits code. It works one subroutine at a time and may

  * read the annotated listing, disassemble and read memory,
  * RUN the routine in the emulator (with chosen registers / memory) and look at what changed:
    registers, memory, writes to custom chip / CIA registers, exceptions,
  * give names and comments (labels, routine documentation, variables, data regions).

Everything the model says is stored as names and comments only; the final listing is generated from
the real bytes and verified by assembling it again, so it always describes exactly the same program.

Requires only the Python standard library, a built `m68ktool`, and a running Ollama server with a
tool-capable model (default `qwen3:8b`; use `--model` for another tag).

Example:
    python3 agent/m68k_annotate.py --hunk game --project game.m68kproj.json --out game_annotated.s --lang German
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections import Counter

# --------------------------------------------------------------------------------------------
# talking to the emulator


class ServerError(RuntimeError):
    pass


class M68kServer:
    """`m68ktool serve` as a subprocess: one JSON line in, one JSON line out."""

    def __init__(self, exe, allow_files=True):
        cmd = [exe, "serve"] + (["--allow-files"] if allow_files else [])
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8",
                                     errors="replace", bufsize=1)

    def call(self, **request):
        self.proc.stdin.write(json.dumps(request) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise ServerError("m68ktool serve terminated unexpectedly")
        return json.loads(line)

    def ok(self, **request):
        """Like call(), but raises on {"ok": false}."""
        reply = self.call(**request)
        if not reply.get("ok"):
            raise ServerError(reply.get("error", "unknown error") + (" " + json.dumps(reply["errors"]) if "errors" in reply else ""))
        return reply

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            self.proc.kill()
        finally:
            self.proc.stdout.close()


# --------------------------------------------------------------------------------------------
# talking to Ollama


class OllamaError(RuntimeError):
    pass


class OllamaFormatError(OllamaError):
    """Ollama could not parse what the model wrote as a tool call (usually invalid JSON, e.g. a bare 0x1234 instead of "0x1234")."""


class Ollama:
    retry_delay = 2  # seconds, multiplied by the attempt number
    max_tokens_limit = 16384  # automatic escalation stops here

    def __init__(self, host, model, timeout=900, think=False, num_ctx=32768, temperature=0.7, max_tokens=4096):
        self.url = host.rstrip("/") + "/api/chat"
        self.model, self.timeout, self.think = model, timeout, think
        # Qwen recommends sampling (not greedy / low temperature: that makes it repeat itself endlessly);
        # num_predict caps one answer so that a runaway "thinking" cannot eat the whole context window.
        self.options = {"num_ctx": num_ctx, "temperature": temperature, "top_p": 0.8, "top_k": 20, "presence_penalty": 1.0,
                        "num_predict": max_tokens}

    def capabilities(self):
        """What Ollama says the model can do (e.g. {"completion", "tools", "thinking"}), or None if unknown."""
        try:
            req = urllib.request.Request(self.url.rsplit("/api/", 1)[0] + "/api/show", data=json.dumps({"model": self.model}).encode(),
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=30) as resp:
                caps = json.loads(resp.read()).get("capabilities")
            return set(caps) if isinstance(caps, list) else None
        except Exception:  # noqa: BLE001  (older Ollama versions, unreachable server: assume the model is fine)
            return None

    def chat(self, messages, tools):
        body = {"model": self.model, "messages": messages, "tools": tools, "stream": False, "options": self.options}
        if self.think is not None:
            body["think"] = self.think
        data = json.dumps(body).encode()
        last = None
        for attempt in range(3):
            try:
                req = urllib.request.Request(self.url, data=data, headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    return json.loads(resp.read())
            except urllib.error.HTTPError as e:  # the server answered: do not retry client errors
                detail = e.read().decode(errors="replace")[:500]
                if e.code == 500 and re.search(r"invalid character|unexpected end of JSON|cannot unmarshal|error parsing tool call|invalid JSON", detail, re.I):
                    raise OllamaFormatError(detail) from e  # the model's fault, not the server's: retrying the same request is pointless
                if e.code == 400 and "think" in detail.lower() and "think" in body:
                    # the model has no thinking mode: ask again without the parameter, and remember
                    self.think = None
                    del body["think"]
                    data = json.dumps(body).encode()
                    last = OllamaError(detail)
                    continue
                if 400 <= e.code < 500:
                    raise OllamaError(f"Ollama returned HTTP {e.code}: {detail}") from e
                last = OllamaError(f"Ollama returned HTTP {e.code}: {detail}")
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                last = OllamaError(f"cannot reach Ollama at {self.url}: {e}")
            time.sleep(self.retry_delay * (attempt + 1))
        raise last


def check_server(server):
    """The orchestrator relies on the reverse-engineering commands of the C++ side; an old build would fail in confusing ways."""
    reply = server.call(cmd="help")
    cmds = reply.get("commands", []) if reply.get("ok") else []
    missing = [c for c in ("analyze", "routines", "listing", "ann_routine", "call", "checkpoint") if c not in cmds]
    if missing:
        raise ServerError("your m68ktool is outdated (it lacks: " + ", ".join(missing) + "). Rebuild it:  cmake --build build   (after git pull)")


# --------------------------------------------------------------------------------------------
# tool definitions (what the model sees)


def _fn(name, description, properties, required=()):
    return {"type": "function", "function": {"name": name, "description": description,
                                              "parameters": {"type": "object", "properties": properties, "required": list(required)}}}


ADDR = {"type": "string", "description": 'Address as a hexadecimal string such as "0x001000", or a label name. Plain numbers in JSON are decimal.'}

TOOLS = [
    _fn("get_listing", "Annotated listing of an address range, as currently named and commented.",
        {"from": ADDR, "to": ADDR}, ["from", "to"]),
    _fn("disassemble", "Disassemble `count` instructions starting at an address.",
        {"addr": ADDR, "count": {"type": "integer"}}, ["addr"]),
    _fn("read_memory", "Hex dump of memory.", {"addr": ADDR, "length": {"type": "integer"}}, ["addr"]),
    _fn("run_routine",
        "Run the routine at `addr` in the emulator like a function call and report what it did: registers afterwards, "
        "memory it changed, writes to custom chip / CIA registers, exceptions (e.g. address errors from wild pointers). The machine is "
        "restored afterwards unless keep_state is true. Use this to TEST your hypotheses instead of guessing.",
        {"addr": ADDR,
         "registers": {"type": "object", "description": 'Initial registers, e.g. {"D0": 5, "A0": "0x030000"}'},
         "memory": {"type": "array", "description": 'Memory to set up first: [{"addr": "0x030000", "hex": "48454C4C4F00"}]',
                    "items": {"type": "object", "properties": {"addr": ADDR, "hex": {"type": "string"}}}},
         "max_instructions": {"type": "integer", "description": "default 200000"},
         "keep_state": {"type": "boolean", "description": "keep the changes (then call reset_machine when done)"}},
        ["addr"]),
    _fn("reset_machine", "Restore the machine to the state it had when the analysis started.", {}),
    _fn("name_label", "Give an address (code or data) a meaningful name, e.g. a loop or entry point.",
        {"addr": ADDR, "name": {"type": "string", "description": "snake_case, ASCII, unique"}}, ["addr", "name"]),
    _fn("document_routine", "Name and document the subroutine. Required before finish_routine.",
        {"addr": ADDR, "name": {"type": "string", "description": "snake_case verb phrase, e.g. draw_sprite"},
         "summary": {"type": "string", "description": "what it does, 1-3 sentences"},
         "inputs": {"type": "string", "description": 'registers/memory read, e.g. "A0 = source, D0 = length"'},
         "outputs": {"type": "string", "description": "results and return values"},
         "clobbers": {"type": "string", "description": 'registers destroyed, e.g. "D0 D1 A1"'},
         "confidence": {"type": "string", "enum": ["high", "medium", "low"],
                        "description": "how sure you are: high = verified by running it, medium = plausible from the code, low = a guess"}},
        ["addr", "name", "summary"]),
    _fn("name_variable", "Name a memory location the code reads/writes (variable, flag, pointer, buffer, table).",
        {"addr": ADDR, "name": {"type": "string"}, "size": {"type": "integer", "description": "bytes, default 1"},
         "comment": {"type": "string", "description": "meaning and unit"}}, ["addr", "name"]),
    _fn("mark_data", "Declare a range as data (not code) and how to display it.",
        {"addr": ADDR, "length": {"type": "integer"}, "kind": {"type": "string", "enum": ["bytes", "words", "longs", "text", "pointers"]},
         "name": {"type": "string"}, "comment": {"type": "string"}}, ["addr", "length", "kind"]),
    _fn("add_comment", "Comment for one line of code (appears behind the instruction), or a block comment above it. Use the exact address "
                       "from the right-hand comment of the listing line; an address inside an instruction is refused.",
        {"addr": ADDR, "text": {"type": "string"}, "block": {"type": "boolean"}}, ["addr", "text"]),
    _fn("finish_routine", "Call when the routine is fully documented and commented.", {"summary": {"type": "string"}}),
]

# --------------------------------------------------------------------------------------------
# helpers


def _coerce(value, schema):
    """Converts a model-supplied value to the JSON type the schema asks for (models are sloppy about this)."""
    kind = schema.get("type")
    if kind == "boolean":
        if isinstance(value, str):
            low = value.strip().lower()
            if low in ("true", "yes", "1"):
                return True
            if low in ("false", "no", "0", ""):
                return False
            raise ValueError(f"expected true or false, got {value!r}")
        return bool(value)
    if kind == "integer":
        if isinstance(value, bool):
            raise ValueError("expected a number")
        if isinstance(value, float) and value.is_integer():
            return int(value)
        if isinstance(value, str):
            try:
                return int(value.strip().replace("$", "0x"), 0)
            except ValueError:
                raise ValueError(f"expected a number, got {value!r}") from None
        if isinstance(value, int):
            return value
        raise ValueError(f"expected a number, got {value!r}")
    if kind in ("object", "array"):
        if isinstance(value, str):  # JSON written as a string
            try:
                value = json.loads(value)
            except json.JSONDecodeError:
                raise ValueError(f"expected {kind} (JSON), got text") from None
        if kind == "object" and not isinstance(value, dict) or kind == "array" and not isinstance(value, list):
            raise ValueError(f"expected {kind}")
        return value
    if kind == "string":
        if schema is ADDR and isinstance(value, (int, float)) and not isinstance(value, bool):
            return int(value)  # a JSON number is a decimal address; it must not turn into a "hex" string
        return value if isinstance(value, str) else str(value)
    return value


def prepare_args(tool_name, args):
    """Validates and normalises tool arguments against the tool's schema.

    Returns (clean_args, notes). Unknown arguments are dropped with a note; wrong types are converted when
    that is unambiguous. Raises ValueError with a message the model can act on.
    """
    spec = next((t["function"] for t in TOOLS if t["function"]["name"] == tool_name), None)
    if spec is None:
        raise ValueError(f"unknown tool '{tool_name}'. Available tools: " + ", ".join(t["function"]["name"] for t in TOOLS))
    props = spec["parameters"]["properties"]
    required = spec["parameters"].get("required", [])
    clean, notes = {}, []
    for key, value in args.items():
        if key not in props:
            notes.append(f"ignored unknown argument '{key}' (valid: {', '.join(props) or 'none'})")
            continue
        if value is None:
            continue
        try:
            clean[key] = _coerce(value, props[key])
        except ValueError as e:
            raise ValueError(f"argument '{key}': {e}") from None
    missing = [k for k in required if k not in clean]
    if missing:
        raise ValueError(f"missing required argument(s): {', '.join(missing)}. Valid arguments: {', '.join(props)}")
    return clean, notes


def clean_tool_name(name):
    """'functions.run_routine', 'run_routine()' and 'Run_Routine' all mean run_routine."""
    name = (name or "").strip().rstrip("()").split(".")[-1]
    known = {t["function"]["name"] for t in TOOLS}
    if name in known:
        return name
    return name.lower() if name.lower() in known else name


_TOOL_CALL_TAG = re.compile(r"<tool_call>\s*(.*?)\s*(?:</tool_call>|$)", re.S)


def calls_from_content(content):
    """Some chat templates make the model write the call as text instead of using `tool_calls`.

    Understands <tool_call>{"name": ..., "arguments": {...}}</tool_call> and a bare JSON object (optionally in a
    ```json fence) with the same keys. Returns a list of {"function": {...}} dicts (empty if there is none).
    """
    if not content:
        return []
    candidates = _TOOL_CALL_TAG.findall(content)
    if not candidates:
        fenced = re.findall(r"```(?:json)?\s*(\{.*?\})\s*```", content, re.S)
        candidates = fenced or ([content.strip()] if content.strip().startswith("{") else [])
    calls = []
    for text in candidates:
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            continue
        for item in obj if isinstance(obj, list) else [obj]:
            if isinstance(item, dict) and isinstance(item.get("name"), str) and "arguments" in item:
                calls.append({"function": {"name": item["name"], "arguments": item["arguments"]}})
    return calls


def compact_messages(messages, max_chars):
    """Keeps the conversation within a size budget by shortening the oldest tool results.

    The system prompt, the task and the most recent exchange are never touched.
    """
    def size():
        return sum(len(m.get("content") or "") for m in messages)

    for m in messages[2:-4]:
        if size() <= max_chars:
            break
        if m["role"] == "tool" and len(m["content"]) > 200:
            m["content"] = m["content"][:150] + " ... [older tool output removed to save context]"
    return messages


def norm_addr(value):
    """Normalise an address argument given by the model.

    A bare string of hex digits that contains a letter a-f, or that has 6 or more digits starting with 0
    (\"001000\", \"00DFF180\"), is hexadecimal; other bare digit strings are left alone (decimal).
    """
    if isinstance(value, bool):
        raise ValueError("an address is required")
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if not isinstance(value, str) or not value.strip():
        raise ValueError("an address is required")
    v = value.strip()
    if re.fullmatch(r"[0-9A-Fa-f]{3,8}", v) and (re.search(r"[A-Fa-f]", v) or (len(v) >= 6 and v.startswith("0"))):
        return "0x" + v
    return v


def trim(text, limit):
    return text if len(text) <= limit else text[:limit] + f"\n... [{len(text) - limit} more characters cut]"


def to_int(hex_text):
    return int(hex_text, 16)


VECTORS = {2: "bus error", 3: "address error (odd address or wild pointer)", 4: "illegal instruction", 5: "division by zero", 6: "CHK",
           7: "TRAPV", 8: "privilege violation", 9: "trace", 10: "line A", 11: "line F"}


def vector_name(v):
    if v in VECTORS:
        return VECTORS[v]
    if 32 <= v <= 47:
        return f"TRAP #{v - 32}"
    if 25 <= v <= 31:
        return f"interrupt level {v - 24}"
    return f"vector {v}"


# --------------------------------------------------------------------------------------------
# tool implementations


class Tools:
    def __init__(self, server):
        self.s = server
        self.finished = False
        self.current = None       # address (hex string) of the routine being processed
        self.range = None         # (first, end) address of that routine, end exclusive
        self.finish_warned = False

    def reset_routine(self):
        self.finished = False
        self.finish_warned = False

    def dispatch(self, name, args):
        name = clean_tool_name(name)
        if isinstance(args, str):
            try:
                args = json.loads(args) if args.strip() else {}
            except json.JSONDecodeError:
                return "error: arguments must be a JSON object"
        if not isinstance(args, dict):
            return "error: arguments must be an object"
        try:
            clean, notes = prepare_args(name, args)
            fn = getattr(self, "t_" + name, None)
            if fn is None:
                return f"error: unknown tool '{name}'"
            result = fn(**clean)
            return result + ("\n(" + "; ".join(notes) + ")" if notes else "")
        except TypeError as e:
            return f"error: bad arguments for {name}: {e}"
        except (ServerError, ValueError) as e:
            return f"error: {e}"

    # ---- reading ----
    def t_get_listing(self, **a):
        r = self.s.ok(cmd="listing", verify=False, addresses=True, max_chars=24000, **{"from": norm_addr(a["from"]), "to": norm_addr(a["to"])})
        return r["listing"] + ("\n[listing truncated]" if r.get("truncated") else "")

    def t_disassemble(self, addr, count=16):
        r = self.s.ok(cmd="dis", addr=norm_addr(addr), count=max(1, min(int(count), 200)))
        return "\n".join(f'{l["addr"]}  {l["bytes"]:<12} {l["text"]}' + (f'   <{l["label"]}>' if "label" in l else "") for l in r["lines"])

    def t_read_memory(self, addr, length=64):
        r = self.s.ok(cmd="read", addr=norm_addr(addr), len=max(1, min(int(length), 1024)))
        h = r["hex"]
        base = to_int(r["addr"])
        rows = [f"0x{base + i // 2:06X}: " + " ".join(h[i + j:i + j + 2] for j in range(0, min(32, len(h) - i), 2)) for i in range(0, len(h), 32)]
        return "\n".join(rows) + f'\nascii: {r["ascii"]}'

    # ---- running ----
    def t_run_routine(self, addr, registers=None, memory=None, max_instructions=200000, keep_state=False):
        s = self.s
        s.ok(cmd="rollback", name="baseline")
        for m in memory or []:
            if not isinstance(m, dict) or "addr" not in m or "hex" not in m:
                raise ValueError('each memory entry needs "addr" and "hex"')
            digits = re.sub(r"0x|[\s,]", "", str(m["hex"]), flags=re.I)
            s.ok(cmd="write", addr=norm_addr(m["addr"]), hex=digits)
        regs = {k: (norm_addr(v) if isinstance(v, str) else v) for k, v in (registers or {}).items()}
        s.ok(cmd="io")  # empty the device-write log
        limit = max(1, min(int(max_instructions), 5_000_000))  # a runaway loop must not stall the whole analysis
        reply = s.call(cmd="call", addr=norm_addr(addr), regs=regs, max_instructions=limit)
        if not reply.get("ok"):
            return f"error: {reply.get('error')}"
        r = reply["regs"]
        out = [f'stop: {reply["stop"]}  returned: {reply.get("returned")}  instructions: {reply["instructions"]}']
        out.append("registers after: " + "  ".join(f"D{i}={r[f'D{i}'][2:]}" for i in range(8)))
        out.append("                 " + "  ".join(f"A{i}={r[f'A{i}'][2:]}" for i in range(8)) + f'  flags={r["flags"]}')
        if reply["stop"] != "returned":
            out.append(f'NOTE: it did not return (stop: {reply["stop"]}). For a program entry or a main loop that is normal: the state shown is the '
                       f'state after {reply["instructions"]} instructions. Use a smaller max_instructions to look at an earlier moment.')
        if reply.get("exceptions"):
            out.append(f'EXCEPTIONS: {reply["exceptions"]} (last: {vector_name(reply.get("last_exception", 0))}). A wild or odd pointer in a register '
                       "usually means you did not set up the inputs the routine expects; set registers/memory and run again.")

        sp = to_int(r["A7"])
        diff = s.ok(cmd="diff", name="baseline")
        shown = [x for x in diff["ranges"] if not (sp - 0x80 <= to_int(x["addr"]) < sp + 0x80)]
        if shown:
            out.append(f"memory changed ({len(shown)} ranges; stack area omitted; small ranges first):")
            for x in sorted(shown, key=lambda x: (x["len"], to_int(x["addr"])))[:14]:
                out.append(f'  {x["addr"]} len {x["len"]}: {x["before"][:32]} -> {x["after"][:32]}')
            if len(shown) > 14:
                out.append(f"  ... and {len(shown) - 14} larger ranges (use read_memory to look at them)")
        else:
            out.append("memory changed: nothing (outside the stack)")

        writes = s.ok(cmd="io")["output"]
        if writes:
            lo, hi = (to_int(self.range[0]), to_int(self.range[1])) if self.range else (0, 0)
            own = [w for w in writes if lo <= to_int(w["pc"]) < hi]
            other = [w for w in writes if not lo <= to_int(w["pc"]) < hi]
            out.append(f"writes to hardware / unmapped addresses: {len(writes)} in total - {len(own)} by this routine's own instructions, "
                       f"{len(other)} by the code it calls (do NOT attribute those to this routine's lines):")
            for label, group, cap in (("this routine", own, 10), ("called code", other, 5)):
                for w in group[:cap]:
                    out.append(f'  {w["addr"]} <- {w["value"]} ({w["kind"]}{" " + w["what"] if "what" in w else ""}) by the instruction at {w["pc"]} [{label}]')
                if len(group) > cap:
                    out.append(f"  ... and {len(group) - cap} more from {label}")
            if any(w["kind"] == "unmapped" for w in writes):
                out.append("NOTE: writes to unmapped addresses were dropped; that usually means a pointer register was not set up.")
        if not keep_state:
            s.ok(cmd="rollback", name="baseline")
        else:
            out.append("state kept; call reset_machine to restore.")
        return "\n".join(out)

    def t_reset_machine(self):
        self.s.ok(cmd="rollback", name="baseline")
        return "machine restored"

    # ---- annotating ----
    def t_name_label(self, addr, name):
        self.s.ok(cmd="ann_label", addr=norm_addr(addr), name=name)
        return f"ok: {name}"

    def t_document_routine(self, addr, name, summary, inputs="", outputs="", clobbers="", confidence=""):
        self.s.ok(cmd="ann_routine", addr=norm_addr(addr), name=name, summary=summary, inputs=inputs, outputs=outputs,
                  clobbers=clobbers, confidence=confidence)
        return f"ok: routine {name} documented"

    def t_name_variable(self, addr, name, size=1, comment=""):
        self.s.ok(cmd="ann_variable", addr=norm_addr(addr), name=name, size=int(size), comment=comment)
        return f"ok: variable {name}"

    def t_mark_data(self, addr, length, kind, name="", comment=""):
        self.s.ok(cmd="ann_data", addr=norm_addr(addr), length=int(length), kind=kind, name=name, comment=comment)
        return "ok: data region marked"

    def t_add_comment(self, addr, text, block=False):
        self.s.ok(cmd="ann_comment", addr=norm_addr(addr), text=text, block=bool(block))
        return "ok"

    def completeness_problems(self, rng=None):
        """What is still missing in a routine's listing (names, comments); default: the routine being processed."""
        rng = rng or self.range
        if not rng:
            return []
        r = self.s.ok(cmd="listing", verify=False, max_chars=200000, **{"from": rng[0], "to": rng[1]})
        text = r["listing"]
        problems = []
        unnamed = re.findall(r"^((?:loc|sub|data)_[0-9A-Fa-f]{6}):", text, re.M)
        if unnamed:
            problems.append(f"labels without a real name: {', '.join(unnamed[:8])}{' ...' if len(unnamed) > 8 else ''} (name_label)")
        numeric = sorted({int(a, 16) for a in re.findall(r"\$([0-9A-Fa-f]{4,8})\.[WL]\b", text)})
        numeric = [a for a in numeric if 0x100 <= a < 0xBF0000]  # not vectors, not hardware registers
        if numeric:
            problems.append("memory locations used by number without a name: " + ", ".join(f"${a:06X}" for a in numeric[:8]) +
                            (" ..." if len(numeric) > 8 else "") + " (name_variable, or mark_data for tables)")
        wanted = max(2, r["instructions"] // 10)
        if r.get("user_comments", wanted) < wanted:
            problems.append(f"only {r['user_comments']} comment(s) for {r['instructions']} instructions - add about {wanted} short ones with add_comment "
                            "(explain meaning and purpose of the non-obvious steps)")
        return problems

    def t_finish_routine(self, summary=""):
        still_open = {to_int(r["addr"]) for r in self.s.ok(cmd="routines", undocumented_only=True)["routines"]}
        if self.current is not None and to_int(self.current) in still_open:
            return "error: call document_routine for this routine first (name, summary, inputs, outputs, clobbers)."
        if not self.finish_warned:
            self.finish_warned = True
            problems = self.completeness_problems()
            if problems:
                return ("not finished yet - the listing of this routine is still incomplete:\n- " + "\n- ".join(problems) +
                        "\nFix this with the annotation tools, then call finish_routine again (a second call is accepted as final).")
        self.finished = True
        return "ok: routine finished"


# --------------------------------------------------------------------------------------------
# the annotation workflow

SYSTEM_PROMPT = """You are an expert MC68000 and Commodore Amiga reverse engineer. You document ONE subroutine at a time so that the final
assembler listing reads like the original source code, with meaningful names and helpful comments.

How you work:
0. The listing is in Motorola syntax: source operand first, destination last (MOVE.L D0,(A0)+ copies D0 to the address in A0, then
   increments A0). Numbers with $ are hexadecimal. The right-hand comment of every line starts with the instruction's ADDRESS; copy
   addresses from there and never count bytes yourself. Names and comments only work at those exact addresses.
1. Read the listing of the routine. Writes to custom chip registers appear by name (COLOR00, DMACON, ...). Routines called by this one
   that are already documented appear with their real names. Placeholders (sub_XXXXXX, loc_XXXXXX, data_XXXXXX) await a real name.
2. Form a hypothesis, then TEST it: use run_routine with suitable registers/memory. Compare the registers, memory changes, hardware
   writes and exceptions with what you expected. Do not guess when you can run it.
3. Record what you learned with the annotation tools: document_routine (name, summary, inputs, outputs, clobbers), name_variable for
   every memory location that has a meaning, mark_data for tables/strings/pointer lists, name_label for loops and branch targets,
   add_comment for the non-obvious lines.
4. Call finish_routine when done.

MC68000 / Amiga facts that are easy to get wrong:
- Data registers D0-D7, address registers A0-A7; A7 is the stack pointer and the stack grows DOWNWARDS (-(A7) pushes, (A7)+ pops).
  MOVEQ #n,Dx sign-extends an 8 bit value to 32 bits. Operations on a data register with .B or .W change only the low part of it.
  Writing to an address register with .W (MOVEA.W) sign-extends to 32 bits. LEA label(PC),A0 loads an ADDRESS (it does not read memory).
- BSR / JSR call a subroutine, RTS returns. Registers are not saved automatically: MOVEM.L D0-D3/A0,-(A7) ... MOVEM.L (A7)+,D0-D3/A0
  is the usual save/restore pair. DBRA Dn,label is a counting loop: Dn.w counts DOWN from n to -1, so the body runs n+1 times.
- Instructions and 16/32 bit data must be at EVEN addresses; an odd address raises an address error (vector 3).
- Amiga system calls are JSR -offset(A6): A6 holds the base of a library; the negative offset selects the function (a multiple of 6).
  The base of exec.library is read from the absolute address 4 (MOVEA.L 4.W,A6). Examples in exec.library: -552 OpenLibrary (A1 = name
  string such as "dos.library", D0 = version), -414 CloseLibrary, -198 AllocMem, -210 FreeMem. Say which library an A6 value belongs to
  only if you can see where it came from; otherwise describe the call by its offset and arguments.
- The custom chips live at $DFF000-$DFFFFF. In DMACON ($DFF096), INTENA ($DFF09A), INTREQ ($DFF09C) and ADKCON ($DFF09E) bit 15 selects
  SET (1) or CLEAR (0) for the other bits written; writing $7FFF clears everything. COLOR00 ($DFF180) is the background colour, 12 bit
  $0RGB. The CIAs sit at $BFxxxx. Chip RAM starts at address 0 (512 KB on an A500). The emulator only REMEMBERS what is written to
  the custom chips and CIAs; it does not emulate them, so do not expect values to change by themselves.

Do not finish early. Before finish_routine: give EVERY label a name (loops, branch targets still called loc_/sub_/data_), name every
memory location the code uses (name_variable, with a comment on its meaning), mark tables and strings (mark_data) and add a short comment
(add_comment) to every non-obvious step. finish_routine checks this once and tells you what is missing.

JSON: write every tool argument as valid JSON. Addresses and hex values are STRINGS in quotes ("0x001000"), never bare 0x001000 (that makes
the call fail).

Efficiency: you may and should make SEVERAL tool calls in ONE reply (for example all name_variable and add_comment calls together, then
finish_routine). Every reply costs time, and you only have a limited number of replies per routine.

Be brief. Do not re-derive MC68000 basics and do not repeat yourself. Your FIRST reply must already be a tool call (usually run_routine).
Prefer testing over long reasoning: a run tells you more than a page of thinking.

Naming: snake_case ASCII, unique, describing purpose not address. Routines start with a verb (draw_sprite, wait_for_vblank), variables
are nouns (player_x, lives_left), flags end in a state (sound_enabled). Never reuse register names, mnemonics or hardware register names.
Comments explain WHY and meaning, not what the instruction obviously does; keep them short. Mention units (pixels, bytes, characters)
and valid ranges when you know them. If you are not sure, say so in the comment ("probably ...") instead of inventing details.
Set `confidence` in document_routine honestly: high only if a test you ran confirms it, medium if it is plausible from the code, low if it
is a guess. Another model may check your low/medium routines later - that is fine, a clear "low" is more useful than a confident mistake.
Write all summaries and comments in {lang}.
"""

REFINE_PROMPT = """
THIS IS A REFINEMENT PASS. A faster model has already annotated this routine; its names, comments and documentation appear in the listing.
Treat them as hypotheses of a colleague, not as facts: verify the important claims by running the routine, correct what is wrong (calling
document_routine, name_label, name_variable or add_comment again for the same address overwrites the old text) and close the gaps listed in
the task. Do not rewrite things that are already right. Set `confidence` to high only for what you verified.
"""


class Annotator:
    """Runs one or more passes over the program. Pass 1 explores; later passes refine routines that still have gaps."""

    def __init__(self, server, passes, args, log=None):
        self.s, self.passes, self.a, self.log = server, passes, args, log
        self.llm = passes[0]["llm"]
        self.max_turns = args.max_turns
        self.mode = "explore"
        self.tools = Tools(server)

    said_thinking = False
    chat_ok = False        # becomes True with the first successful answer: errors before that are configuration problems
    model_errors = 0       # consecutive routines that failed because of the model server

    def say(self, text):
        print(text, flush=True)

    def log_event(self, **ev):
        if self.log:
            self.log.write(json.dumps(ev, default=str) + "\n")
            self.log.flush()

    # ---- setup ----
    def setup(self):
        a, s = self.a, self.s
        check_server(s)
        if a.machine == "amiga500":
            s.ok(cmd="machine", type="amiga500", chip_kb=a.chip_kb)
            if a.rom:
                s.ok(cmd="load_rom", file=a.rom)
        else:
            s.ok(cmd="machine", type="generic")
        if a.hunk:
            r = s.ok(cmd="load_file", file=a.hunk, type="hunk", addr=a.hunk_base)
            self.say(f'hunk loaded: {len(r["hunks"])} hunks, entry {r["entry"]}, {len(r["symbols"])} symbols')
        elif a.srec:
            r = s.ok(cmd="load_file", file=a.srec, type="srec")
            self.say(f'S-records loaded: {r["size"]} bytes')
        else:
            s.ok(cmd="load_file", file=a.bin, type="raw", addr=a.org)
        if a.project and os.path.exists(a.project):
            s.ok(cmd="ann_load", file=a.project)
            self.say("project loaded")
        entries = a.entry or ([a.org] if a.bin else [s.ok(cmd="regs")["regs"]["PC"]])
        r = s.ok(cmd="analyze", entries=entries, reset=True)
        self.say(f'analysed {r["instructions"]} instructions, {r["from"]}-{r["to"]}, {r["routines"]} routines')
        s.ok(cmd="checkpoint", name="baseline")

    # ---- one routine ----
    @staticmethod
    def extent(r):
        """(first address, end address exclusive) of a routine as hex strings."""
        return r["start"], r["end"]

    def routine_prompt(self, r, gaps=None):
        addr = r["addr"]
        start, end = self.extent(r)
        listing = self.s.ok(cmd="listing", verify=False, addresses=True, max_chars=14000, **{"from": start, "to": end})
        callees = [f'  {c["name"]} ({c["addr"]}{"" if c["documented"] else ", not documented yet"})' for c in r["callees"]]
        callees += [f"  {x} (outside the analysed code)" for x in r["external_calls"]]
        callees += [f'  library call JSR {c["offset"]}(A6) at {c["addr"]}' for c in r["library_calls"]]
        callers = ", ".join(r["callers"]) or "none (entry point)"
        text = (f'ROUTINE_ADDR={addr}  current name: {r["name"]}  size: {to_int(end) - to_int(start)} bytes  instructions: {r["instructions"]}\n'
                f'Called from: {callers}\nCalls:\n' + ("\n".join(callees) if callees else "  nothing") +
                f"\n\nListing:\n```\n{listing['listing']}\n```\n{'[listing truncated; use get_listing for the rest]' if listing.get('truncated') else ''}\n"
                "Analyse this routine, test it, annotate it and call finish_routine.")
        if gaps:
            text += ("\n\nKnown gaps to close in this pass:\n- " + "\n- ".join(gaps) +
                     "\nVerify the existing documentation as well, and lower or raise its confidence accordingly.")
        return text

    def process_routine(self, r, gaps=None):
        t = self.tools
        t.reset_routine()
        t.current = r["addr"]
        t.range = self.extent(r)
        system = SYSTEM_PROMPT.format(lang=self.a.lang)
        if self.mode == "refine":
            system += REFINE_PROMPT
        messages = [{"role": "system", "content": system},
                    {"role": "user", "content": self.routine_prompt(r, gaps)}]
        nudges = 0
        cut = 0
        bad_json = 0
        repeats = {}
        try:
            for turn in range(self.max_turns):
                if self.max_turns >= 6 and turn == self.max_turns - 3:
                    messages.append({"role": "user", "content": "Only 3 rounds are left for this routine. In your next reply call ALL remaining annotation tools "
                                                                "together (several tool calls in one reply are fine) and finish_routine."})
                compact_messages(messages, max_chars=self.a.num_ctx * 3)
                try:
                    resp = self.llm.chat(messages, TOOLS)
                except OllamaFormatError as e:
                    bad_json += 1
                    self.say(f"  the model wrote an invalid tool call ({bad_json}x): {str(e)[:110]}")
                    self.log_event(routine=r["addr"], turn=turn, invalid_tool_call=str(e)[:300])
                    if bad_json > 4:
                        return False
                    messages.append({"role": "user", "content": "Your last tool call could not be parsed because it was not valid JSON. Write ALL arguments as valid JSON: "
                                                                'addresses and hex values MUST be strings in quotes, e.g. "0x001000" - never a bare 0x001000. Try again.'})
                    continue
                self.chat_ok = True
                msg = resp.get("message", {})
                if msg.get("thinking") and not self.llm.think and not self.said_thinking:
                    self.said_thinking = True
                    self.say("  note: this model thinks even with --think false (a thinking-only model tag). It works, but slowly; "
                             "an -instruct tag is faster. The answer limit grows automatically when it is cut off.")
                self.log_event(routine=r["addr"], turn=turn, message=msg)
                echoed = {k: v for k, v in msg.items() if k in ("content", "tool_calls")}
                echoed["role"] = "assistant"
                messages.append(echoed)
                calls = msg.get("tool_calls") or calls_from_content(msg.get("content"))
                if not calls and resp.get("done_reason") == "length":
                    # the answer hit the token limit (usually: endless thinking) - nothing usable was produced
                    cut += 1
                    messages.pop()  # the empty, cut-off answer carries no information: do not keep it in the context
                    old = self.llm.options["num_predict"]
                    new = min(int(old * 1.5), self.llm.max_tokens_limit)
                    self.llm.options["num_predict"] = new  # a model that needs long thinking gets room for it
                    self.say(f'  answer cut off at the token limit ({cut}x, {old} tokens); '
                             + (f"raising the limit to {new}" if new > old else "limit already at the maximum") +
                             '. Alternatives: a smaller thinking effort, or an "-instruct" model tag')
                    if cut > 5 or (new == old and cut > 2):
                        return False
                    messages.append({"role": "user", "content": "Your last answer was cut off because it was too long. Do NOT deliberate: pick ONE concrete next step "
                                                                "(for example run_routine, or document_routine) and call the tool right now."})
                    continue
                if not calls:
                    nudges += 1
                    if nudges > 2:
                        self.say(f'  model stopped without finishing {r["addr"]}')
                        return False
                    messages.append({"role": "user", "content": "Continue with tool calls. When the routine is documented, call finish_routine."})
                    continue
                for call in calls:
                    fn = call.get("function", {})
                    name, args = clean_tool_name(fn.get("name", "")), fn.get("arguments", {})
                    result = t.dispatch(name, args)
                    key = (name, json.dumps(args, sort_keys=True, default=str))
                    if result.startswith("error"):
                        repeats[key] = repeats.get(key, 0) + 1
                        if repeats[key] >= 3:
                            result += "\nYou have made this same failing call several times. Change your approach."
                    self.log_event(routine=r["addr"], tool=name, args=args, result=trim(result, 2000))
                    messages.append({"role": "tool", "tool_name": name, "content": trim(result, 6000)})
                if t.finished:
                    return True
            self.say(f'  turn limit reached for {r["addr"]}')
            still_open = {to_int(x["addr"]) for x in self.s.ok(cmd="routines", undocumented_only=True)["routines"]}
            if to_int(r["addr"]) not in still_open:
                self.say("  (the routine is documented, only finish_routine was missing: counted as partial; a refinement pass will check it)")
                return "partial"
            return False
        finally:
            t.current = None

    def process_routine_safely(self, r, gaps=None):
        """A server error on one routine must not end a run that may have taken hours: note it, go on with the next one.

        Errors before the first successful answer (wrong host, unknown model) are configuration problems and abort at once;
        three routines in a row failing means the server is really gone."""
        try:
            result = self.process_routine(r, gaps)
            self.model_errors = 0
            return result
        except OllamaError as e:
            if not self.chat_ok:
                raise
            self.model_errors += 1
            self.say(f'  model error on {r["addr"]}: {str(e)[:160]} - skipping this routine ({self.model_errors} in a row)')
            self.log_event(routine=r["addr"], model_error=str(e)[:300])
            if self.model_errors >= 3:
                raise
            return False

    # ---- gaps ----
    def routine_gaps(self, r):
        """Objective list of what is missing or doubtful about a routine's documentation."""
        gaps = [] if r["documented"] else ["the routine is not documented yet (document_routine)"]
        gaps += self.tools.completeness_problems(self.extent(r))
        conf = r.get("confidence", "")
        if conf == "low" or (conf == "medium" and self.a.refine_medium):
            gaps.append(f"the previous model marked its own documentation as confidence {conf}: verify it by running the routine")
        return gaps

    def only_filter(self, routines):
        if self.a.only:
            wanted = {to_int(x) if isinstance(x, str) and x.lower().startswith("0x") else int(x) for x in self.a.only}
            routines = [r for r in routines if to_int(r["addr"]) in wanted]
        return routines

    # ---- passes ----
    def explore_pass(self):
        s, done, tried = self.s, Counter(), set()
        for _ in range(3):  # documenting a routine can reveal that others are still open
            routines = self.only_filter(s.ok(cmd="routines", undocumented_only=True)["routines"])
            routines = [r for r in routines if r["addr"] not in tried]
            if not routines:
                break
            tried |= {r["addr"] for r in routines}
            for i, r in enumerate(routines, 1):
                self.say(f'[{i}/{len(routines)}] {r["name"]} at {r["addr"]} ({to_int(r["end"]) - to_int(r["start"])} bytes)')
                done[self.process_routine_safely(r)] += 1
                self.save()
        return done

    def refine_pass(self):
        s, done = self.s, Counter()
        routines = self.only_filter(s.ok(cmd="routines")["routines"])
        todo = []
        for r in routines:
            gaps = self.routine_gaps(r)
            if gaps or (self.a.refine_all and r["documented"]):
                todo.append((r, gaps))
        self.say(f"{len(todo)} of {len(routines)} routines have gaps")
        for i, (r, gaps) in enumerate(todo, 1):
            self.say(f'[{i}/{len(todo)}] {r["name"]} at {r["addr"]}: ' + "; ".join(g.split(" (")[0] for g in gaps)[:140])
            done[self.process_routine_safely(r, gaps)] += 1
            self.save()
        return done

    def save(self):
        if self.a.project:
            self.s.ok(cmd="ann_save", file=self.a.project)

    def report(self):
        """Remaining gaps after all passes (also written to --report)."""
        routines = self.only_filter(self.s.ok(cmd="routines")["routines"])
        rows = [(r, self.routine_gaps(r)) for r in routines]
        open_rows = [(r, g) for r, g in rows if g]
        low = sum(1 for r, _ in rows if r.get("confidence") == "low")
        self.say(f"gaps left: {len(open_rows)} of {len(rows)} routines" + (f" ({low} with low confidence)" if low else ""))
        if self.a.report:
            lines = ["# Annotation report", "", f"{len(rows) - len(open_rows)} of {len(rows)} routines complete, {len(open_rows)} with gaps.", ""]
            for r, g in open_rows:
                lines.append(f'## {r["name"]} ({r["addr"]}, {to_int(r["end"]) - to_int(r["start"])} bytes, confidence: {r.get("confidence") or "n/a"})')
                lines += [f"- {x}" for x in g]
                lines.append("")
            with open(self.a.report, "w", encoding="utf-8") as f:
                f.write("\n".join(lines))
        return len(open_rows)

    def run(self):
        s = self.s
        for n, p in enumerate(self.passes, 1):
            self.llm, self.mode, self.max_turns = p["llm"], p["mode"], p["max_turns"]
            self.said_thinking = False
            self.say(f'== pass {n}/{len(self.passes)} ({self.mode}): model {self.llm.model}' + (", thinking" if self.llm.think else "") + " ==")
            result = self.explore_pass() if self.mode == "explore" else self.refine_pass()
            self.say(f"pass {n}: {result[True]} routines finished, {result['partial']} partial (documented, not finished), {result[False]} failed")
        listing = s.ok(cmd="listing", verify=True, max_chars=10_000_000)
        if self.a.out:
            with open(self.a.out, "w", encoding="utf-8") as f:
                f.write(listing["listing"])
        self.report()
        self.say("listing " + ("VERIFIED (re-assembles to the identical bytes)" if listing["verified"] else f'NOT verified: {listing.get("problem", "?")}')
                 + (f" -> {self.a.out}" if self.a.out else ""))
        return listing


def make_pass(a, model, mode, think, max_tokens, max_turns):
    """Creates the Ollama client for one pass and adapts it to what the model can do."""
    llm = Ollama(a.host, model, think=False, num_ctx=a.num_ctx, temperature=a.temperature, max_tokens=max_tokens)
    caps = llm.capabilities()
    if caps is not None and "tools" not in caps:
        raise OllamaError(f"model {model} does not support tool calling (capabilities: {', '.join(sorted(caps))})")
    llm.think = (caps is not None and "thinking" in caps) if think == "auto" else (think is True or think == "true")
    return {"llm": llm, "mode": mode, "max_turns": max_turns}


def find_m68ktool(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "build", "m68ktool"), os.path.join(here, "..", "build", "Release", "m68ktool.exe")):
        if os.path.exists(cand):
            return cand
    found = shutil.which("m68ktool")
    if not found:
        sys.exit("m68ktool not found: build the project or pass --m68ktool")
    return found


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--hunk", help="Amiga hunk executable")
    src.add_argument("--bin", help="raw binary (use with --org)")
    src.add_argument("--srec", help="Motorola S-record file")
    p.add_argument("--org", default="0x1000", help="load address of --bin")
    p.add_argument("--hunk-base", default="0x20000", help="load address of --hunk")
    p.add_argument("--machine", choices=["amiga500", "generic"], default="amiga500", help="memory map (amiga500: chip RAM, custom/CIA registers, ROM)")
    p.add_argument("--chip-kb", type=int, default=512, help="chip RAM of the amiga500 machine")
    p.add_argument("--rom", help="Kickstart ROM image (amiga500 only; your own copy, it is not part of this project)")
    p.add_argument("--entry", action="append", help='entry point (repeatable), default: the start of the program, e.g. "0x20000"')
    p.add_argument("--project", help="annotation file (JSON); loaded if present, saved after every routine")
    p.add_argument("--out", help="write the final annotated listing here")
    p.add_argument("--only", action="append", help="process only this routine address (repeatable)")
    p.add_argument("--model", default="qwen3:8b-instruct", help="Ollama model tag for pass 1 (needs tool support)")
    p.add_argument("--refine-model", action="append", default=[], metavar="MODEL",
                   help="model for one more pass that checks and completes routines that still have gaps (repeatable; e.g. a slower thinking model)")
    p.add_argument("--refine-think", choices=["auto", "true", "false"], default="auto", help="thinking mode in refinement passes (auto: if the model has it)")
    p.add_argument("--refine-max-tokens", type=int, default=12000, help="answer limit in refinement passes (they may think a lot)")
    p.add_argument("--refine-max-turns", type=int, default=25, help="tool-call rounds per routine in refinement passes")
    p.add_argument("--refine-all", action="store_true", help="let the refinement passes review every documented routine, not only those with gaps")
    p.add_argument("--refine-medium", action="store_true", help="also revisit routines the model itself rated medium confidence")
    p.add_argument("--report", help="write the remaining gaps (per routine) to this Markdown file")
    p.add_argument("--host", default=os.environ.get("OLLAMA_HOST", "http://localhost:11434"))
    p.add_argument("--think", choices=["true", "false"], default="false",
                   help="the model's thinking mode (default false: tool calling works better and it cannot run away; ignored by models without it)")
    p.add_argument("--num-ctx", type=int, default=32768, help="context window in tokens (listing + conversation)")
    p.add_argument("--max-tokens", type=int, default=4096, help="limit for ONE model answer (with --think true use more, e.g. 12000)")
    p.add_argument("--temperature", type=float, default=0.7, help="do not go low: Qwen models repeat themselves endlessly at low temperature")
    p.add_argument("--lang", default="English", help='language of summaries and comments, e.g. "German"')
    p.add_argument("--max-turns", type=int, default=20, help="model replies per routine (several tool calls fit in one reply)")
    p.add_argument("--log", help="write every model message and tool result to this JSON-lines file")
    p.add_argument("--m68ktool", help="path to the m68ktool executable")
    a = p.parse_args(argv)
    if not a.host.startswith("http"):
        a.host = "http://" + a.host

    server = M68kServer(find_m68ktool(a.m68ktool))
    log = open(a.log, "a", encoding="utf-8") if a.log else None
    try:
        passes = [make_pass(a, a.model, "explore", think=a.think == "true", max_tokens=a.max_tokens, max_turns=a.max_turns)]
        for model in a.refine_model:
            passes.append(make_pass(a, model, "refine", think=a.refine_think, max_tokens=a.refine_max_tokens, max_turns=a.refine_max_turns))
        worker = Annotator(server, passes, a, log)
        worker.setup()
        listing = worker.run()
        return 0 if listing["verified"] else 1
    except (ServerError, OllamaError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finally:
        server.close()
        if log:
            log.close()


if __name__ == "__main__":
    sys.exit(main())
