import os
import sys

class SolidifyCompiler:
    def __init__(self):
        self.symbols = {}
        self.vtables = {}
        self.methods = []
        self.closures = []
        # std-lib 3c-1: distinct String constants. Keyed by literal value ->
        # C variable name (StaticString_<sanitized>). Populated by the
        # registration pass when it sees a ("STRING", value) marker; emitted as
        # fixed-layout String structs (StringVtable + length + inline bytes).
        self.string_consts = {}

    def sanitize_symbol_name(self, name):
        mapping = {
            ':': '_',
            '=': '_eq',
            '+': '_add',
            '-': '_sub',
            '*': '_mul',
            '/': '_div',
            '<': '_lt',
            '>': '_gt',
            '&': '_and',
            '|': '_or',
            '!': '_not',
            '?': '_q',
            '@': '_at',
            '%': '_mod',
        }
        res = []
        for char in name:
            if char.isalnum() or char == '_':
                res.append(char)
            elif char in mapping:
                res.append(mapping[char])
            else:
                res.append(f"_u{ord(char)}")
        return "".join(res)

    def get_symbol_ref(self, name):
        if name not in self.symbols:
            sanitized = self.sanitize_symbol_name(name)
            self.symbols[name] = f"Symbol_{sanitized}"
        return f"(oop)&{self.symbols[name]}"

    def get_string_ref(self, value):
        # std-lib 3c-1: map a String-literal value to a unique StaticString_ C
        # variable. Sanitization alone is not injective (e.g. 'a b' and 'a-b'
        # both sanitize identically), so disambiguate with the registration
        # index to guarantee one struct per distinct value.
        if value not in self.string_consts:
            # Keep the C identifier pure-ASCII: sanitize_symbol_name passes
            # non-ASCII alnum (e.g. 'é') through unchanged, which is not a
            # portable C identifier. Reduce to [A-Za-z0-9_]; the index suffix
            # guarantees uniqueness regardless of any collisions this introduces.
            sanitized = self.sanitize_symbol_name(value)
            sanitized = "".join(c if (c.isascii() and (c.isalnum() or c == "_")) else "_" for c in sanitized)
            self.string_consts[value] = f"StaticString_{sanitized}_{len(self.string_consts)}"
        return f"(oop)&{self.string_consts[value]}"

    def add_vtable(self, name, delegate_name=None, is_nested=0, meta=None):
        # `meta` is the name of this vtable's own metaclass vtable (its header
        # `base.vtable`). When set, sends to the class OBJECT dispatch through
        # it (class-side methods). Defaults to VtableVtable when None.
        self.vtables[name] = {
            'delegate': delegate_name,
            'bindings': [],
            'is_nested': is_nested,
            'meta': meta,
        }

    def add_method(self, vtable_name, selector, num_registers, num_arguments, constants, instructions, access_modifier=0):
        sanitized_sel = self.sanitize_symbol_name(selector)
        method_id = f"Method_{vtable_name}_{sanitized_sel}"
        self.vtables[vtable_name]['bindings'].append((selector, method_id))
        self.methods.append({
            'id': method_id,
            'num_registers': num_registers,
            'num_arguments': num_arguments,
            'constants': constants,
            'instructions': instructions,
            'access_modifier': access_modifier
        })

    def generate_c(self):
        # Pre-pass: register all symbols
        for vt_name, vt_data in self.vtables.items():
            for selector, _ in vt_data['bindings']:
                self.get_symbol_ref(selector)
        for method in self.methods:
            for const in method['constants']:
                if isinstance(const, tuple) and const and const[0] == "STRING":
                    # std-lib 3c-1: String literal -> StaticString_ (NOT a Symbol).
                    self.get_string_ref(const[1])
                elif isinstance(const, str) and not const.startswith("SMI:"):
                    self.get_symbol_ref(const)

        c_code = []
        c_code.append('#include "oevm.h"\n')
        c_code.append('#include <stddef.h>\n')

        # Forward declare root meta vtables
        c_code.append("/* Forward declare root Vtables */")
        c_code.append("extern const struct Vtable Vtable_VtableVtable;")
        c_code.append("extern const struct Vtable Vtable_SymbolVtable;")
        c_code.append("extern const struct Vtable Vtable_StringVtable;")
        c_code.append("extern const struct Vtable Vtable_RawBytesVtable;")
        c_code.append("extern const struct Vtable Vtable_BytecodeClosureVtable;")
        c_code.append("extern const struct Vtable Vtable_PrimitiveClosureVtable;")
        c_code.append("extern const struct Vtable Vtable_BoxedBindingVtable;")
        c_code.append("extern const struct Vtable Vtable_StackVtable;")
        c_code.append("extern const struct Vtable Vtable_ContextVtable;")
        for vt_name in self.vtables:
            c_code.append(f"extern const struct Vtable Vtable_{vt_name};")
        c_code.append("")

        # Global definitions for custom vtables
        c_code.append("/* Global Vtable pointers for custom classes */")
        system_vtables = {"IntegerVtable", "FloatVtable", "LargeIntegerVtable", "MethodContextVtable", "SymbolVtable", "VtableVtable", "PrimitiveClosureVtable", "BytecodeClosureVtable", "BoxedBindingVtable", "StackVtable", "ContextVtable"}
        for vt_name in self.vtables:
            if vt_name not in system_vtables:
                c_code.append(f"oop {vt_name} = NULL;")
        c_code.append("")

        # Generate Symbols
        c_code.append("/* Interned Symbols */")
        for sym_name, sym_var in self.symbols.items():
            len_sym = len(sym_name)
            c_code.append(f'const struct Symbol {sym_var} = {{ {{ (oop)&Vtable_SymbolVtable }}, "{sym_name}", {len_sym} }};')
        c_code.append("")

        # std-lib 3c-1: String constants. Each is a fixed-layout struct matching
        # `struct String` (Object base + length + inline UTF-8 bytes). Bytes are a
        # brace-init so non-printable / multi-byte UTF-8 content is byte-safe, and
        # the layout matches struct String exactly (the VM casts to it). A
        # zero-length string still reserves bytes[1] (a flexible/zero array is not
        # portable C99) but reports length 0.
        c_code.append("/* String literal constants (std-lib 3c-1) */")
        for s_value, str_var in self.string_consts.items():
            raw = s_value.encode("utf-8")
            byte_init = ", ".join(str(b) for b in raw) if raw else "0"
            c_code.append(
                f'static const struct {{ struct Object base; size_t length; uint8_t bytes[{max(len(raw),1)}]; }} '
                f'{str_var} = {{ {{ (oop)&Vtable_StringVtable }}, {len(raw)}, {{ {byte_init} }} }};'
            )
        c_code.append("")

        # Generate Methods instructions & constants
        c_code.append("/* Compiled Method Bytecodes and Constants */")
        for method in self.methods:
            mid = method['id']
            # Instructions array
            inst_list = ", ".join(f"0x{inst:08X}" for inst in method['instructions'])
            c_code.append(f"static const uint32_t {mid}_insts[] = {{ {inst_list} }};")
            
            # Constants array
            const_decls = []
            for const in method['constants']:
                if isinstance(const, tuple) and const and const[0] == "STRING":
                    # std-lib 3c-1: reference the StaticString_ struct emitted above.
                    const_decls.append(self.get_string_ref(const[1]))
                elif isinstance(const, str):
                    if const.startswith("SMI:"):
                        val = int(const.split(":")[1])
                        const_decls.append(f"TAG_SMI({val})")
                    elif const.startswith("LARGEINT:"):
                        # Box the overflowing integer literal as a static const
                        # LargeInteger (sign + uint64 magnitude). Header is the
                        # Vtable STRUCT &Vtable_LargeIntegerVtable, as for floats.
                        ival = int(const.split(":", 1)[1])
                        sign = -1 if ival < 0 else 1
                        mag = abs(ival)
                        li_var = f"{mid}_largeint_{len(const_decls)}"
                        c_code.insert(-1, f"static const struct LargeInteger {li_var} = {{ {{ (oop)&Vtable_LargeIntegerVtable }}, {sign}, {mag}ULL }};")
                        const_decls.append(f"(oop)&{li_var}")
                    else:
                        const_decls.append(self.get_symbol_ref(const))
                elif isinstance(const, int):
                    const_decls.append(f"TAG_SMI({const})")
                elif isinstance(const, float):
                    # Box the float literal as a static const Float. Its header
                    # must be the Vtable STRUCT (&Vtable_FloatVtable), NOT the
                    # global pointer variable `FloatVtable` (&FloatVtable is the
                    # address of the oop, not the vtable) — otherwise the VM does
                    # not recognise the literal as a Float (vtable check fails in
                    # printString, <, =, etc.) and treats it as a bare object.
                    # Matches the Symbol/Closure literals above.
                    float_var = f"{mid}_float_{len(const_decls)}"
                    c_code.insert(-1, f"static const struct Float {float_var} = {{ {{ (oop)&Vtable_FloatVtable }}, {const} }};")
                    const_decls.append(f"(oop)&{float_var}")
                else:
                    const_decls.append("NULL")
            
            const_list = ", ".join(const_decls)
            if const_decls:
                c_code.append(f"static const oop {mid}_consts[] = {{ {const_list} }};")
                const_ptr = f"{mid}_consts"
            else:
                const_ptr = "NULL"
                
            c_code.append(f"static const struct CompiledMethod {mid}_compiled = {{")
            c_code.append(f"    {mid}_insts, {len(method['instructions'])},")
            c_code.append(f"    {method['num_registers']}, {method['num_arguments']},")
            c_code.append(f"    {const_ptr}, {len(method['constants'])},")
            c_code.append(f"    {method.get('access_modifier', 0)}")
            c_code.append("};")
            
            # Closure
            c_code.append(f"static const struct Closure {mid}_closure = {{")
            c_code.append("    { (oop)&Vtable_BytecodeClosureVtable }, NULL, 0,")
            c_code.append(f"    (void *)&{mid}_compiled")
            c_code.append("};\n")

        # Generate Vtable bindings and Vtable structures
        c_code.append("/* Vtables and Bindings */")
        for vt_name, vt_data in self.vtables.items():
            bindings_var = f"Vtable_{vt_name}_bindings"
            if vt_data['bindings']:
                c_code.append(f"static const struct Binding {bindings_var}[] = {{")
                for selector, method_id in vt_data['bindings']:
                    sym_ref = self.get_symbol_ref(selector)
                    c_code.append(f"    {{ {sym_ref}, (oop)&{method_id}_closure }},")
                c_code.append("};")
                bindings_ptr = bindings_var
                bindings_size = len(vt_data['bindings'])
            else:
                bindings_ptr = "NULL"
                bindings_size = 0
                
            delegate_ref = f"(oop)&Vtable_{vt_data['delegate']}" if vt_data['delegate'] else "NULL"
            meta_ref = f"(oop)&Vtable_{vt_data['meta']}" if vt_data.get('meta') else "(oop)&Vtable_VtableVtable"
            c_code.append(f"const struct Vtable Vtable_{vt_name} = {{")
            c_code.append(f"    {{ {meta_ref} }},")
            c_code.append(f"    {delegate_ref}, (struct Binding *){bindings_ptr},")
            c_code.append(f"    {bindings_size}, {bindings_size},")
            c_code.append(f"    {vt_data['is_nested']}")
            c_code.append("};\n")

        # Root Meta Vtables definitions
        c_code.append("/* Root Vtables Definitions */")
        c_code.append("const struct Vtable Vtable_VtableVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        c_code.append("const struct Vtable Vtable_SymbolVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        # std-lib 3c-1: String's root vtable (NULL bindings; behavior is served by
        # the primitive table, exactly like SymbolVtable). Path A needs this DEFINED
        # so StaticString_ constants' (oop)&Vtable_StringVtable headers link, and so
        # `StringVtable` is non-NULL (its prims gate on obj_vtable == StringVtable).
        c_code.append("const struct Vtable Vtable_StringVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        # Milestone B Task 10 fix: RawBytes (Milestone B Task 3) shares String's
        # fixed C struct layout and its at:/at:put:/size prims gate on
        # obj_vtable == RawBytesVtable — same reason StringVtable is hand-rooted
        # above. Without this, Path A binaries (which link this generated file
        # instead of kernel_boot.c) leave RawBytesVtable NULL, so
        # `platform basicNewBytes:` allocates objects with a nil vtable and every
        # byte access DNUs instead of reaching the C bounds/range checks.
        c_code.append("const struct Vtable Vtable_RawBytesVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        c_code.append("const struct Vtable Vtable_BytecodeClosureVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        c_code.append("const struct Vtable Vtable_PrimitiveClosureVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        c_code.append("const struct Vtable Vtable_BoxedBindingVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        c_code.append("const struct Vtable Vtable_StackVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")
        c_code.append("const struct Vtable Vtable_ContextVtable = { { (oop)&Vtable_VtableVtable }, NULL, NULL, 0, 0, 0 };")

        # Write init hook to assign global non-const vtable pointers
        c_code.append("\nvoid init_solidified_vtables(void) {")
        c_code.append("    VtableVtable = (oop)&Vtable_VtableVtable;")
        c_code.append("    SymbolVtable = (oop)&Vtable_SymbolVtable;")
        c_code.append("    StringVtable = (oop)&Vtable_StringVtable;")
        c_code.append("    RawBytesVtable = (oop)&Vtable_RawBytesVtable;")
        c_code.append("    BytecodeClosureVtable = (oop)&Vtable_BytecodeClosureVtable;")
        c_code.append("    PrimitiveClosureVtable = (oop)&Vtable_PrimitiveClosureVtable;")
        c_code.append("    BoxedBindingVtable = (oop)&Vtable_BoxedBindingVtable;")
        c_code.append("    StackVtable = (oop)&Vtable_StackVtable;")
        c_code.append("    ContextVtable = (oop)&Vtable_ContextVtable;")
        for vt_name in self.vtables:
            c_code.append(f"    {vt_name} = (oop)&Vtable_{vt_name};")
        c_code.append("}")

        return "\n".join(c_code)

def main():
    # Example generator script
    compiler = SolidifyCompiler()
    
    # Define system/user classes
    compiler.add_vtable("IntegerVtable")
    compiler.add_vtable("FloatVtable")
    compiler.add_vtable("MethodContextVtable")
    # unwindTo:value: (the NLR mechanism) is retired under the no-NLR model
    # (Milestone A Task 2); this demo generator no longer installs it.
    # Fiber vtables are retired under Milestone A Task 4; this demo generator
    # no longer registers them (the VM still defines the fiber prims/vtables
    # themselves until Task 5, but the compiler no longer emits them).

    # We will define a counter class vtable for testing XIP
    compiler.add_vtable("CounterVtable")
    
    # Add simple increment method:
    # Counter >> increment
    compiler.add_method(
        vtable_name="CounterVtable",
        selector="increment",
        num_registers=4,
        num_arguments=0,
        constants=["SMI:1"],
        instructions=[
            0x0000010C, # OP_LD_SLOT: dest=1, src1=0, src2=0
            0x00000200, # OP_LD_CONST: dest=2, src1=0 (SMI 1)
            0x02010306, # OP_ADD: dest=3, src1=1, src2=2
            0x0000030D, # OP_ST_SLOT: dest=3, src1=0, src2=0
            0x00000304  # OP_RET: dest=3
        ]
    )

    # We will define a recursive runner class vtable for stack walking/reflection tests
    compiler.add_vtable("RecursiveRunnerVtable")
    compiler.add_method(
        vtable_name="RecursiveRunnerVtable",
        selector="runRecurse:",
        num_registers=6,
        num_arguments=1,
        constants=["SMI:0", "SMI:1", "runRecurse:", "inspectStack"],
        instructions=[
            0x0005010B, # OP_JMP_IF_FALSE: dest=1 (n), offset=5
            0x00010200, # OP_LD_CONST: dest=2, src1=1 (SMI 1)
            0x02010307, # OP_SUB: dest=3, src1=1 (n), src2=2 (SMI 1)
            0x00030101, # OP_LD_REG: dest=1, src1=3
            0x00020102, # OP_SEND: dest=1 (argc), src1=2 ("runRecurse:"), src2=0
            0x00000004, # OP_RET: dest=0
            0x00030002, # OP_SEND: dest=0 (argc), src1=3 ("inspectStack"), src2=0
            0x00000004  # OP_RET: dest=0
        ]
    )

    output = compiler.generate_c()
    out_path = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else "src/solidified_image.c"
    with open(out_path, "w") as f:
        f.write(output)
    print(f"Solidified image successfully written to {out_path}")

if __name__ == "__main__":
    main()
