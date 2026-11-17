// SPDX-License-Identifier: GPL-2.0-or-later
// A small SPIR-V module writer: ids, deduplicated types and constants, one function, structured control flow. The GCN
// emitter (gcn_spirv.cpp) writes the module directly before its SPIRV-Tools optimization passes.
#pragma once
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>
#include <bit>
#include <compare>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Gcn::Spv {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// A result id: not a number, so a literal never passes for one.
struct Id {
    u32 v = 0;
    constexpr Id() = default;
    constexpr explicit Id(u32 x) : v(x) {}
    constexpr explicit operator u32() const { return v; }
    constexpr explicit operator bool() const { return v != 0; }
    auto operator<=>(const Id&) const = default;
};
// An operand of a type declaration: a literal or an id.
struct W {
    u32 v;
    constexpr W(u32 x) : v(x) {}
    constexpr W(Id i) : v(i.v) {}
};

class Builder {
    u32 bound = 1;  // before `glsl`, which takes the first id
public:
    Builder() = default;
    Id glsl = NewId();  // the GLSL.std.450 import

    Id NewId() { return Id(bound++); }

    // Types and constants are written once per distinct value.
    Id Type(spv::Op op, std::initializer_list<W> ops = {}) {
        std::vector<u32> key{u32(op)};
        for (W w : ops) key.push_back(w.v);
        auto [it, fresh] = tmap.try_emplace(std::move(key), Id());
        if (!fresh) return it->second;
        it->second = NewId();
        Put(types, op, it->second, ops);
        return it->second;
    }
    Id Struct(std::initializer_list<W> members) {  // never shared: decorations are per struct
        const Id id = NewId();
        Put(types, spv::OpTypeStruct, id, members);
        return id;
    }
    Id Const(Id type, u32 v) {
        auto [it, fresh] = cmap.try_emplace({type.v, v}, Id());
        if (fresh) {
            it->second = NewId();
            PutT(types, spv::OpConstant, type, it->second, {v});
        }
        return it->second;
    }
    Id Const64(Id type, u64 v) {
        auto [it, fresh] = cmap.try_emplace({type.v, v | (1ull << 63)}, Id());
        if (fresh) {
            it->second = NewId();
            PutT(types, spv::OpConstant, type, it->second, {u32(v), u32(v >> 32)});
        }
        return it->second;
    }
    Id ConstBool(Id type, bool v) {
        auto [it, fresh] = cmap.try_emplace({type.v, 0x100000000ull | v}, Id());
        if (fresh) {
            it->second = NewId();
            PutT(types, v ? spv::OpConstantTrue : spv::OpConstantFalse, type, it->second, {});
        }
        return it->second;
    }

    // Module-level: a variable outside the function (interface of the entry point), decorations, execution modes.
    Id Global(Id pointer_type, spv::StorageClass sc) {
        const Id id = NewId();
        PutT(gvars, spv::OpVariable, pointer_type, id, {u32(sc)});
        iface.push_back(id);
        return id;
    }
    void Decorate(Id target, spv::Decoration d, std::initializer_list<u32> ops = {}) {
        annot.push_back((u32(ops.size()) + 3) << 16 | spv::OpDecorate);
        annot.push_back(target.v), annot.push_back(d), annot.insert(annot.end(), ops);
    }
    void DecorateMember(Id target, u32 member, spv::Decoration d, std::initializer_list<u32> ops = {}) {
        annot.push_back((u32(ops.size()) + 4) << 16 | spv::OpMemberDecorate);
        annot.push_back(target.v), annot.push_back(member), annot.push_back(d), annot.insert(annot.end(), ops);
    }
    void Capability(spv::Capability c) { caps.insert(c); }
    void Extension(const char* name) { extensions.insert(name); }
    void Mode(spv::ExecutionMode m, std::initializer_list<u32> ops = {}) { modes.push_back({m, std::vector<u32>(ops)}); }

    // A variable of the function (its first block).
    Id Local(Id pointer_type, Id init) {
        const Id id = NewId();
        if (init) PutT(vars, spv::OpVariable, pointer_type, id, {u32(spv::StorageClassFunction), init.v});
        else PutT(vars, spv::OpVariable, pointer_type, id, {u32(spv::StorageClassFunction)});
        return id;
    }

    // The function body. An instruction after a terminator opens an unreachable block first.
    template <class... A> Id Res(spv::Op op, Id type, A... a) {
        Ensure();
        const Id r = NewId();
        Ins(code, op, type, r, a...);
        return r;
    }
    template <class... A> Id EntryRes(spv::Op op, Id type, A... a) {
        const Id r = NewId();
        Ins(entry_code, op, type, r, a...);
        return r;
    }
    template <class... A> void Do(spv::Op op, A... a) {
        Ensure();
        Ins(code, op, a...);
    }
    void Label(Id l) {
        Ins(code, spv::OpLabel, l);
        cur = l, open = true;
    }
    void Br(Id l) {
        Do(spv::OpBranch, l);
        open = false;
    }
    void BrCond(Id c, Id t, Id f) {
        Do(spv::OpBranchConditional, c, t, f);
        open = false;
    }
    void Switch(Id selector, Id dflt, const std::map<u32, Id>& cases) {
        Ensure();
        code.push_back((u32(cases.size()) * 2 + 3) << 16 | spv::OpSwitch), code.push_back(selector.v), code.push_back(dflt.v);
        for (auto [lit, id] : cases) code.push_back(lit), code.push_back(id.v);
        open = false;
    }
    void Return() {
        Do(spv::OpReturn);
        open = false;
    }
    void Kill() {  // discard
        Do(spv::OpTerminateInvocation);
        open = false;
    }
    Id Cur() { return Ensure(), cur; }
    bool Open() const { return open; }
    void Start() {  // the function's entry block: its label is written by Finish, before the variables
        first = NewId(), cur = first, open = true;
    }
    // Closes the function written so far as one the entry function, started over here, runs with OpFunctionCall.
    Id Helper(Id fn_type, Id void_type) {
        const Id f = NewId();
        Ins(funcs, spv::OpFunction, void_type, f, u32(spv::FunctionControlMaskNone), fn_type);
        Ins(funcs, spv::OpLabel, first);
        funcs.insert(funcs.end(), vars.begin(), vars.end()), funcs.insert(funcs.end(), entry_code.begin(), entry_code.end());
        funcs.insert(funcs.end(), code.begin(), code.end());
        Ins(funcs, spv::OpFunctionEnd);
        vars.clear(), entry_code.clear(), code.clear(), Start();
        return f;
    }

    // Assembles the module around the function (type `fn`, label `first`, entry point `main`).
    std::vector<u32> Finish(spv::ExecutionModel model, Id main, Id fn_type, Id void_type) {
        std::vector<u32> m{0x07230203, 0x00010600, 0x0, 0, 0};
        for (u32 c : caps) Ins(m, spv::OpCapability, c);
        for (const auto& name : extensions) {
            const size_t words = (name.size() + 4) / 4, at = m.size();
            m.resize(at + 1 + words, 0), m[at] = u32(1 + words) << 16 | spv::OpExtension;
            std::memcpy(m.data() + at + 1, name.c_str(), name.size());
        }
        {
            u32 w[4]{};  // "GLSL.std.450" and its terminator
            std::memcpy(w, "GLSL.std.450", 13);
            m.push_back((2 + 4) << 16 | spv::OpExtInstImport), m.push_back(glsl.v), m.insert(m.end(), w, w + 4);
        }
        Ins(m, spv::OpMemoryModel, caps.count(spv::CapabilityPhysicalStorageBufferAddresses) ? spv::AddressingModelPhysicalStorageBuffer64 : spv::AddressingModelLogical,
            spv::MemoryModelGLSL450);
        {
            u32 w[2]{0x6E69616D, 0};  // "main"
            m.push_back((u32(iface.size()) + 3 + 2) << 16 | spv::OpEntryPoint), m.push_back(model), m.push_back(main.v), m.insert(m.end(), w, w + 2);
            for (Id i : iface) m.push_back(i.v);
        }
        for (auto& [mode, ops] : modes) {
            m.push_back((u32(ops.size()) + 3) << 16 | spv::OpExecutionMode), m.push_back(main.v), m.push_back(mode), m.insert(m.end(), ops.begin(), ops.end());
        }
        m.insert(m.end(), annot.begin(), annot.end());
        m.insert(m.end(), types.begin(), types.end());
        m.insert(m.end(), gvars.begin(), gvars.end());
        m.insert(m.end(), funcs.begin(), funcs.end());
        Ins(m, spv::OpFunction, void_type, main, u32(spv::FunctionControlMaskNone), fn_type);
        Ins(m, spv::OpLabel, first);
        m.insert(m.end(), vars.begin(), vars.end());
        m.insert(m.end(), entry_code.begin(), entry_code.end());
        m.insert(m.end(), code.begin(), code.end());
        Ins(m, spv::OpFunctionEnd);
        m[3] = bound;
        return m;
    }

    template <class... A> static void Ins(std::vector<u32>& v, spv::Op op, A... a) {
        v.push_back((u32(sizeof...(A)) + 1) << 16 | op);
        (v.push_back(u32(a)), ...);
    }

private:
    void Ensure() {
        if (!open) Label(NewId());
    }
    static void Put(std::vector<u32>& v, spv::Op op, Id id, std::initializer_list<W> ops) {  // type declaration: result id first
        v.push_back((u32(ops.size()) + 2) << 16 | op), v.push_back(id.v);
        for (W w : ops) v.push_back(w.v);
    }
    static void PutT(std::vector<u32>& v, spv::Op op, Id type, Id id, std::initializer_list<u32> ops) {  // typed: type, result id
        v.push_back((u32(ops.size()) + 3) << 16 | op), v.push_back(type.v), v.push_back(id.v), v.insert(v.end(), ops);
    }
    std::set<u32> caps;
    std::set<std::string> extensions;
    std::vector<u32> types, gvars, annot, vars, entry_code, code, funcs;
    Id first;
    std::vector<Id> iface;
    std::vector<std::pair<spv::ExecutionMode, std::vector<u32>>> modes;
    std::map<std::vector<u32>, Id> tmap;
    std::map<std::pair<u64, u64>, Id> cmap;
    Id cur;
    bool open = false;
};

// Typed constants and instruction helpers shared by the module writers (the GCN emitter and the fixed shaders).
struct Ops {
    using s32 = std::int32_t;
    Builder b;
    const Id tVoid = b.Type(spv::OpTypeVoid), tBool = b.Type(spv::OpTypeBool), tU32 = b.Type(spv::OpTypeInt, {32, 0}), tI32 = b.Type(spv::OpTypeInt, {32, 1}),
             tF32 = b.Type(spv::OpTypeFloat, {32}), tU64 = b.Type(spv::OpTypeInt, {64, 0}), tI64 = b.Type(spv::OpTypeInt, {64, 1});
    Id Vec(Id c, u32 n) { return b.Type(spv::OpTypeVector, {c, n}); }
    const Id tV2F = Vec(tF32, 2), tV3F = Vec(tF32, 3), tV4F = Vec(tF32, 4), tV2U = Vec(tU32, 2), tV4U = Vec(tU32, 4), tV2I = Vec(tI32, 2), tV3I = Vec(tI32, 3),
             tV4I = Vec(tI32, 4), tV2B = Vec(tBool, 2);
    Id Ptr(spv::StorageClass sc, Id t) { return b.Type(spv::OpTypePointer, {u32(sc), t}); }
    const Id pFnU = Ptr(spv::StorageClassFunction, tU32), pFnB = Ptr(spv::StorageClassFunction, tBool);
    Id CU(u32 v) { return b.Const(tU32, v); }
    Id CI(s32 v) { return b.Const(tI32, u32(v)); }
    Id CFb(u32 bits) { return b.Const(tF32, bits); }
    Id CF(float f) { return CFb(std::bit_cast<u32>(f)); }
    Id CU64(u64 v) { return b.Const64(tU64, v); }
    Id CB(bool v) { return b.ConstBool(tBool, v); }
    Id Zero(int cls) { return cls == 0 ? CF(0.f) : cls == 1 ? CU(0) : CI(0); }
    Id One(int cls) { return cls == 0 ? CF(1.f) : cls == 1 ? CU(1) : CI(1); }
    Id V4(int cls) { return cls == 0 ? tV4F : cls == 1 ? tV4U : tV4I; }
    Id Sc(int cls) { return cls == 0 ? tF32 : cls == 1 ? tU32 : tI32; }

    // Instructions.
    template <class... A> Id R(spv::Op op, Id t, A... x) { return b.Res(op, t, x...); }
    template <class... A> Id E(Id t, GLSLstd450 f, A... x) { return b.Res(spv::OpExtInst, t, b.glsl, u32(f), x...); }
    Id Bc(Id t, Id v) { return R(spv::OpBitcast, t, v); }
    Id Uf(Id f) { return Bc(tU32, f); }  // floatBitsToUint
    Id Fu(Id u) { return Bc(tF32, u); }  // uintBitsToFloat
    Id Iu(Id u) { return Bc(tI32, u); }
    Id Ui(Id i) { return Bc(tU32, i); }
    Id Sel(Id t, Id c, Id x, Id y) { return R(spv::OpSelect, t, c, x, y); }
    Id SelU(Id c, Id x, Id y) { return Sel(tU32, c, x, y); }
    Id SelF(Id c, Id x, Id y) { return Sel(tF32, c, x, y); }
    Id Add(Id x, Id y) { return R(spv::OpIAdd, tU32, x, y); }
    Id Sub(Id x, Id y) { return R(spv::OpISub, tU32, x, y); }
    Id Mul(Id x, Id y) { return R(spv::OpIMul, tU32, x, y); }
    Id And(Id x, Id y) { return R(spv::OpBitwiseAnd, tU32, x, y); }
    Id Or(Id x, Id y) { return R(spv::OpBitwiseOr, tU32, x, y); }
    Id Xor(Id x, Id y) { return R(spv::OpBitwiseXor, tU32, x, y); }
    Id Not(Id x) { return R(spv::OpNot, tU32, x); }
    Id Shl(Id x, Id y) { return R(spv::OpShiftLeftLogical, tU32, x, y); }
    Id Shr(Id x, Id y) { return R(spv::OpShiftRightLogical, tU32, x, y); }
    Id Sar(Id x, Id y) { return Ui(R(spv::OpShiftRightArithmetic, tI32, Iu(x), y)); }
    Id Shl(Id x, u32 n) { return Shl(x, CU(n)); }
    Id Shr(Id x, u32 n) { return Shr(x, CU(n)); }
    Id And(Id x, u32 n) { return And(x, CU(n)); }
    Id Bool(spv::Op op, Id x, Id y) { return R(op, tBool, x, y); }
    Id Eq(Id x, u32 n) { return Bool(spv::OpIEqual, x, CU(n)); }
    Id Ne(Id x, u32 n) { return Bool(spv::OpINotEqual, x, CU(n)); }
    Id LAnd(Id x, Id y) { return Bool(spv::OpLogicalAnd, x, y); }
    Id LOr(Id x, Id y) { return Bool(spv::OpLogicalOr, x, y); }
    Id LNot(Id x) { return R(spv::OpLogicalNot, tBool, x); }
    Id FAdd(Id x, Id y) { return R(spv::OpFAdd, tF32, x, y); }
    Id FSub(Id x, Id y) { return R(spv::OpFSub, tF32, x, y); }
    Id FMul(Id x, Id y) { return R(spv::OpFMul, tF32, x, y); }
    Id FDiv(Id x, Id y) { return R(spv::OpFDiv, tF32, x, y); }
    Id FNeg(Id x) { return R(spv::OpFNegate, tF32, x); }
    Id FAbs(Id x) { return E(tF32, GLSLstd450FAbs, x); }
    Id FLt(Id x, Id y) { return Bool(spv::OpFOrdLessThan, x, y); }
    Id FGe(Id x, Id y) { return Bool(spv::OpFOrdGreaterThanEqual, x, y); }
    Id FGt(Id x, Id y) { return Bool(spv::OpFOrdGreaterThan, x, y); }
    Id U64(Id lo, Id hi) { return R(spv::OpBitwiseOr, tU64, R(spv::OpUConvert, tU64, lo), R(spv::OpShiftLeftLogical, tU64, R(spv::OpUConvert, tU64, hi), CU(32))); }
    Id Lo(Id v) { return R(spv::OpUConvert, tU32, v); }
    Id Hi(Id v) { return R(spv::OpUConvert, tU32, R(spv::OpShiftRightLogical, tU64, v, CU(32))); }
    Id W64(Id v) { return R(spv::OpUConvert, tU64, v); }
    Id Ptr64(Id lo, Id hi) { return U64(lo, And(hi, 0xFFFFu)); }  // PTR(lo, hi)
    Id Extract(Id t, Id v, u32 i) { return R(spv::OpCompositeExtract, t, v, i); }

    Id Var(Id ptr, Id init) { return b.Local(ptr, init); }
    Id Ld(Id var, Id t) { return R(spv::OpLoad, t, var); }
    Id LdU(Id var) { return Ld(var, tU32); }
    void St(Id var, Id v) { b.Do(spv::OpStore, var, v); }

    struct IfCtx {
        Id merge, els;
    };
    IfCtx IfBegin(Id cond, bool with_else = false) {
        const Id merge = b.NewId(), then = b.NewId(), els = with_else ? b.NewId() : merge;
        b.Do(spv::OpSelectionMerge, merge, 0u);
        b.BrCond(cond, then, els);
        b.Label(then);
        return {merge, els};
    }
    void IfElse(const IfCtx& c) {
        if (b.Open()) b.Br(c.merge);
        b.Label(c.els);
    }
    void IfEnd(const IfCtx& c) {
        if (b.Open()) b.Br(c.merge);
        b.Label(c.merge);
    }
    // cond ? f() : zero, where f has effects the untaken side must not have (guest loads).
    template <class F> Id Guard(Id t, Id cond, Id zero, F&& f) {
        const Id from = b.Cur(), then = b.NewId(), merge = b.NewId();
        b.Do(spv::OpSelectionMerge, merge, 0u);
        b.BrCond(cond, then, merge);
        b.Label(then);
        const Id v = f();
        const Id last = b.Cur();
        b.Br(merge);
        b.Label(merge);
        return R(spv::OpPhi, t, v, last, zero, from);
    }
};
} // namespace Gcn::Spv
