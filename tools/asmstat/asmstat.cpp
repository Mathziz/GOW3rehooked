// asmstat: per-function code statistics and listings of a PE image with its PDB (DbgHelp + Zydis).
//   asmstat <image.dll> <out_dir> [names.txt]   -> out_dir/stats.tsv, out_dir/asm/*.asm for names containing a line of names.txt
//   asmstat diff <old.tsv> <new.tsv> [names.txt] -> per-name change of the selected (or all changed) functions
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

struct Function {
    std::string name;
    u64 rva, size;
};

// Counters per function; order matches Columns.
enum Stat { Bytes, Instr, Jcc, Jmp, Call, ICall, Ret, Vec, Ymm, MemR, MemW, Spill, Frame, Div, Cmov, Lock, Tail, Count };
static const char* Columns[Count] = {"bytes", "instr", "jcc", "jmp", "call", "icall", "ret", "vec",
                                     "ymm", "memr", "memw", "spill", "frame", "div", "cmov", "lock", "tail"};

static std::string Utf8(const wchar_t* s, size_t n) {
    std::string out(WideCharToMultiByte(CP_UTF8, 0, s, (int)n, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, out.data(), (int)out.size(), nullptr, nullptr);
    return out;
}

static std::vector<std::string> Lines(const char* path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        if (!line.empty() && line[0] != '#') out.push_back(line);
    return out;
}

static bool Selected(const std::string& name, const std::vector<std::string>& names) {
    for (const auto& n : names)
        if (name.find(n) != std::string::npos) return true;
    return false;
}

struct Image {
    std::vector<u8> file;
    u64 base = 0, image_size = 0;
    struct Section { u64 rva, size, raw, raw_size; };
    std::vector<Section> sections;

    explicit Image(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(in), {});
        auto* dos = (IMAGE_DOS_HEADER*)file.data();
        auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
        base = nt->OptionalHeader.ImageBase, image_size = nt->OptionalHeader.SizeOfImage;
        auto* s = IMAGE_FIRST_SECTION(nt);
        for (u32 i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            sections.push_back({s[i].VirtualAddress, s[i].Misc.VirtualSize, s[i].PointerToRawData, s[i].SizeOfRawData});
    }
    const u8* At(u64 rva, u64 size) const {
        for (const auto& s : sections)
            if (rva >= s.rva && rva + size <= s.rva + std::min(s.size, s.raw_size)) return file.data() + s.raw + (rva - s.rva);
        return nullptr;
    }
};

struct Context {
    Image* image;
    std::vector<Function> functions; // sorted by rva
    const Function* Find(u64 rva) const {
        auto it = std::upper_bound(functions.begin(), functions.end(), rva, [](u64 a, const Function& f) { return a < f.rva; });
        if (it == functions.begin()) return nullptr;
        --it;
        return rva < it->rva + std::max<u64>(it->size, 1) ? &*it : nullptr;
    }
};

static bool IsVector(ZydisRegister r) {
    return (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM31) || (r >= ZYDIS_REGISTER_YMM0 && r <= ZYDIS_REGISTER_YMM31);
}

// Decodes one function; fills counters and, when listing, the formatted text with callee names and source lines.
static void Analyze(const Context& ctx, const Function& f, u64* stats, std::string* listing, HANDLE process) {
    const u8* code = ctx.image->At(f.rva, f.size);
    if (!code) return;
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    std::string last_line;
    u64 ret_end = f.size;
    stats[Bytes] = f.size;
    for (u64 off = 0; off < f.size;) {
        if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, code + off, f.size - off, &ins, ops))) { ++off; continue; }
        ++stats[Instr];
        const auto m = ins.mnemonic;
        const auto cat = ins.meta.category;
        bool vec = false, ymm = false;
        u64 target = 0;
        for (u32 i = 0; i < ins.operand_count_visible; ++i) {
            const auto& o = ops[i];
            if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && IsVector(o.reg.value)) vec = true, ymm |= o.reg.value >= ZYDIS_REGISTER_YMM0;
            if (o.type == ZYDIS_OPERAND_TYPE_MEMORY && o.mem.type == ZYDIS_MEMOP_TYPE_MEM) {
                if (o.actions & ZYDIS_OPERAND_ACTION_MASK_READ) ++stats[MemR];
                if (o.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) {
                    ++stats[MemW];
                    if (o.mem.base == ZYDIS_REGISTER_RSP) ++stats[Spill];
                }
            }
        }
        stats[Vec] += vec, stats[Ymm] += ymm;
        if (cat == ZYDIS_CATEGORY_COND_BR) ++stats[Jcc];
        else if (cat == ZYDIS_CATEGORY_UNCOND_BR) ++stats[Jmp];
        else if (cat == ZYDIS_CATEGORY_CALL) {
            ++stats[Call];
            if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) target = f.rva + off + ins.length + ops[0].imm.value.s;
            else ++stats[ICall];
        } else if (cat == ZYDIS_CATEGORY_RET) ++stats[Ret], ret_end = off + ins.length;
        else if (cat == ZYDIS_CATEGORY_CMOV) ++stats[Cmov];
        if (m == ZYDIS_MNEMONIC_DIV || m == ZYDIS_MNEMONIC_IDIV) ++stats[Div];
        if ((ins.attributes & ZYDIS_ATTRIB_HAS_LOCK) || m == ZYDIS_MNEMONIC_XCHG && ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) ++stats[Lock];
        if (m == ZYDIS_MNEMONIC_SUB && ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[0].reg.value == ZYDIS_REGISTER_RSP &&
            ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !stats[Frame])
            stats[Frame] = ops[1].imm.value.u;
        if (listing) {
            const u64 va = ctx.image->base + f.rva + off;
            IMAGEHLP_LINE64 line{sizeof(line)};
            DWORD disp = 0;
            if (SymGetLineFromAddr64(process, va, &disp, &line)) {
                std::string where = std::filesystem::path(line.FileName).filename().string() + ":" + std::to_string(line.LineNumber);
                if (where != last_line) *listing += "        ; " + where + "\n", last_line = where;
            }
            char text[256];
            ZydisFormatterFormatInstruction(&formatter, &ins, ops, ins.operand_count_visible, text, sizeof(text), va, nullptr);
            char head[32];
            snprintf(head, sizeof head, "%6llx  ", (unsigned long long)off);
            *listing += head;
            *listing += text;
            // Branch targets inside the function as offsets; calls as names.
            if (cat == ZYDIS_CATEGORY_CALL && target) {
                const auto* callee = ctx.Find(target);
                *listing += "    ; " + (callee ? callee->name : std::string("?"));
            } else if ((cat == ZYDIS_CATEGORY_COND_BR || cat == ZYDIS_CATEGORY_UNCOND_BR) && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                const u64 t = f.rva + off + ins.length + ops[0].imm.value.s;
                char b[64];
                if (t >= f.rva && t < f.rva + f.size) snprintf(b, sizeof b, "    ; -> %llx", (unsigned long long)(t - f.rva));
                else { const auto* g = ctx.Find(t); snprintf(b, sizeof b, "    ; tail "); *listing += b; *listing += g ? g->name : "?"; b[0] = 0; }
                *listing += b;
            }
            *listing += "\n";
        }
        off += ins.length;
    }
    stats[Tail] = f.size - ret_end; // blocks placed after the last return: the cold ones
}

static int Diff(const char* old_path, const char* new_path, const char* names_path) {
    auto load = [](const char* path) {
        std::map<std::string, std::vector<long long>> out;
        std::ifstream in(path);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::istringstream s(line);
            std::string name;
            std::getline(s, name, '\t');
            std::vector<long long> v;
            for (std::string cell; std::getline(s, cell, '\t');) v.push_back(std::stoll(cell));
            out[name] = v;
        }
        return out;
    };
    const auto a = load(old_path), b = load(new_path);
    const auto names = names_path ? Lines(names_path) : std::vector<std::string>{};
    // Column 0 is the number of copies; counters follow.
    printf("%-100s %8s %8s %6s %6s %6s %6s %6s %6s\n", "function (copies)", "instr", "d", "jcc", "d", "call", "d", "vec", "d");
    long long totals[2][Count + 1]{};
    std::map<std::string, int> all;
    for (const auto& [n, _] : a) all[n] |= 1;
    for (const auto& [n, _] : b) all[n] |= 2;
    for (const auto& [n, mask] : all) {
        if (!names.empty() && !Selected(n, names)) continue;
        static const std::vector<long long> none(Count + 1, 0);
        const auto& x = mask & 1 ? a.at(n) : none;
        const auto& y = mask & 2 ? b.at(n) : none;
        for (int i = 0; i <= Count; ++i) totals[0][i] += x[i], totals[1][i] += y[i];
        if (x == y) continue;
        std::string shown = n.size() > 92 ? n.substr(0, 92) : n;
        shown += " (" + std::to_string(x[0]) + "->" + std::to_string(y[0]) + ")";
        printf("%-100s %8lld %+8lld %6lld %+6lld %6lld %+6lld %6lld %+6lld\n", shown.c_str(), y[1 + Instr], y[1 + Instr] - x[1 + Instr],
               y[1 + Jcc], y[1 + Jcc] - x[1 + Jcc], y[1 + Call], y[1 + Call] - x[1 + Call], y[1 + Vec], y[1 + Vec] - x[1 + Vec]);
    }
    printf("\ntotal of the %s:", names.empty() ? "image" : "selection");
    for (int i = 0; i < Count; ++i) printf(" %s %lld->%lld", Columns[i], totals[0][1 + i], totals[1][1 + i]);
    printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "diff")) return Diff(argv[2], argv[3], argc > 4 ? argv[4] : nullptr);
    if (argc < 3) {
        fprintf(stderr, "usage: asmstat <image.dll> <out_dir> [names.txt] | asmstat diff <old.tsv> <new.tsv> [names.txt]\n");
        return 2;
    }
    const std::filesystem::path path = std::filesystem::absolute(argv[1]), out = argv[2];
    const auto names = argc > 3 ? Lines(argv[3]) : std::vector<std::string>{};
    Image image(path);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    SymInitializeW(process, path.parent_path().c_str(), FALSE);
    if (!SymLoadModuleExW(process, nullptr, path.c_str(), nullptr, image.base, (DWORD)image.image_size, nullptr, 0)) {
        fprintf(stderr, "cannot load symbols of %s\n", argv[1]);
        return 1;
    }
    Context ctx{&image};
    SymEnumSymbolsW(process, image.base, L"*", [](PSYMBOL_INFOW s, ULONG size, PVOID user) -> BOOL {
        if (s->Tag == 5 /* SymTagFunction */ && size)
            ((Context*)user)->functions.push_back({Utf8(s->Name, wcsnlen(s->Name, s->NameLen)), s->Address - s->ModBase, size});
        return TRUE;
    }, &ctx);
    std::sort(ctx.functions.begin(), ctx.functions.end(), [](auto& a, auto& b) { return a.rva < b.rva; });
    ctx.functions.erase(std::unique(ctx.functions.begin(), ctx.functions.end(), [](auto& a, auto& b) { return a.rva == b.rva; }),
                        ctx.functions.end());

    const size_t n = ctx.functions.size();
    std::vector<u64> stats(n * Count);
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (u32 t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(64)) < n;)
                for (size_t j = i; j < std::min(n, i + 64); ++j) Analyze(ctx, ctx.functions[j], &stats[j * Count], nullptr, nullptr);
        });
    for (auto& t : pool) t.join();

    std::filesystem::create_directories(out / "asm");
    // Copies of one name (template instances, lambdas, duplicated static helpers) are summed.
    std::map<std::string, std::vector<u64>> by_name;
    for (size_t i = 0; i < n; ++i) {
        auto& v = by_name[ctx.functions[i].name];
        if (v.empty()) v.assign(Count + 1, 0);
        ++v[0];
        for (int c = 0; c < Count; ++c) v[1 + c] = c == Frame ? std::max(v[1 + c], stats[i * Count + c]) : v[1 + c] + stats[i * Count + c];
    }
    {
        std::ofstream tsv(out / "stats.tsv", std::ios::binary);
        tsv << "name\tcopies";
        for (auto* c : Columns) tsv << '\t' << c;
        tsv << '\n';
        for (const auto& [name, v] : by_name) {
            tsv << name;
            for (auto x : v) tsv << '\t' << x;
            tsv << '\n';
        }
    }
    // Listings run on this thread: DbgHelp line lookups are not thread-safe.
    size_t listed = 0;
    for (size_t i = 0; i < n && !names.empty(); ++i) {
        const auto& f = ctx.functions[i];
        if (!Selected(f.name, names)) continue;
        u64 s[Count]{};
        std::string text;
        Analyze(ctx, f, s, &text, process);
        std::string file;
        for (char c : f.name.substr(0, 90)) file += isalnum((u8)c) || c == '_' ? c : '_';
        char suffix[32];
        snprintf(suffix, sizeof suffix, "_%llx.asm", (unsigned long long)f.rva);
        std::ofstream asm_file(out / "asm" / (file + suffix), std::ios::binary);
        asm_file << "; " << f.name << "\n;";
        for (int c = 0; c < Count; ++c) asm_file << ' ' << Columns[c] << '=' << s[c];
        asm_file << "\n" << text;
        ++listed;
    }
    printf("%zu functions, %zu names, %zu listed\n", n, by_name.size(), listed);
    return 0;
}
