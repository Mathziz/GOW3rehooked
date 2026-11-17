// bytestat <a> <b>: is b's content found inside a (16-byte windows sampled through b), any offset.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <string>
static std::vector<uint8_t> Load(const char* p) { std::vector<uint8_t> v; FILE* f = fopen(p, "rb"); if (!f) return v; fseek(f, 0, SEEK_END); v.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(v.data(), 1, v.size(), f); fclose(f); return v; }
int main(int c, char** v) {
    auto a = Load(v[1]), b = Load(v[2]);
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i + 16 <= a.size(); i += 8) idx.emplace(std::string((char*)a.data() + i, 16), i);
    size_t hit = 0, total = 0; std::map<long long, size_t> deltas;
    for (size_t i = 0; i + 16 <= b.size(); i += 8) {
        ++total;
        auto it = idx.find(std::string((char*)b.data() + i, 16));
        if (it != idx.end()) ++hit, ++deltas[(long long)it->second - (long long)i];
    }
    printf("%s in %s: %zu of %zu windows; deltas:", v[2], v[1], hit, total);
    int n = 0; for (auto& d : deltas) if (d.second > 8 && n++ < 8) printf(" %lld(%zu)", d.first, d.second);
    puts("");
}
