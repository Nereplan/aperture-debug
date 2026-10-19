#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <map>
#include <string>
#include <string_view>
enum class FontType { Regular };
struct Localization {
    static Localization& Instance() { static Localization value; return value; }
    bool IsRtl() const { return false; }
};
namespace FitMemo {
struct Key {
    std::wstring value;
    FontType font = FontType::Regular;
    int widthBucket = 0;
    bool rtl = false;
};
// Copy-free probe: hits must not copy the title into a full Key every frame.
struct KeyProbe {
    std::wstring_view value;
    FontType font = FontType::Regular;
    int widthBucket = 0;
    bool rtl = false;
};
struct KeyLess {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const {
        if (a.font != b.font) return a.font < b.font;
        if (a.widthBucket != b.widthBucket) return a.widthBucket < b.widthBucket;
        if (a.rtl != b.rtl) return a.rtl < b.rtl;
        return a.value < b.value;
    }
};
struct Entry { std::wstring fitted; std::list<Key>::iterator lruIt; };
constexpr size_t kFitCacheLimit = 512;
inline int Bucket(float maxWidth) { return (int)std::lround(maxWidth * 0.5f); }
} // namespace FitMemo

template <typename R>
static std::wstring FitSingleLineMemo(R& r, std::map<FitMemo::Key, FitMemo::Entry, FitMemo::KeyLess>& cache,
                                      std::list<FitMemo::Key>& lru, const std::wstring& value, FontType font, float maxWidth) {
    if (maxWidth <= 1.0f || value.empty() || r.MeasureString(value, font).width <= maxWidth) return value;
    const bool rtl = Localization::Instance().IsRtl();
    const FitMemo::KeyProbe probe{ std::wstring_view(value), font, FitMemo::Bucket(maxWidth), rtl };
    auto it = cache.find(probe);
    if (it != cache.end()) {
        // Single cached measure verifies the hit; a font change that widens
        // text falls through to a fresh binary search below.
        if (r.MeasureString(it->second.fitted, font).width <= maxWidth) {
            lru.splice(lru.begin(), lru, it->second.lruIt);
            it->second.lruIt = lru.begin();
            return it->second.fitted;
        }
        lru.erase(it->second.lruIt);
        cache.erase(it);
    }
    const std::wstring ellipsis = L"\x2026";
    if (r.MeasureString(ellipsis, font).width > maxWidth) return L"";
    size_t lo = 0;
    size_t hi = value.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        std::wstring candidate = value.substr(0, mid) + ellipsis;
        if (r.MeasureString(candidate, font).width <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    std::wstring fitted = value.substr(0, lo) + ellipsis;
    while (cache.size() >= FitMemo::kFitCacheLimit && !lru.empty()) { cache.erase(lru.back()); lru.pop_back(); }
    lru.push_front(FitMemo::Key{ std::wstring(value), font, probe.widthBucket, rtl });
    const FitMemo::Key& key = lru.front();
    FitMemo::Entry entry;
    entry.fitted = fitted;
    entry.lruIt = lru.begin();
    cache.emplace(key, std::move(entry));
    return fitted;
}

struct Measure {
    float glyphWidth = 1;
    struct Result { float width; };
    Result MeasureString(const std::wstring& value, FontType) const { return {value.size() * glyphWidth}; }
};
int main() {
    Measure renderer;
    std::map<FitMemo::Key, FitMemo::Entry, FitMemo::KeyLess> cache;
    std::list<FitMemo::Key> lru;
    for (int i = 0; i < 2000; ++i) {
        const auto title = L"A long title whose width needs fitting " + std::to_wstring(i);
        for (const float width : {1.0f, 2.0f}) {
            renderer.glyphWidth = width;
            const auto fitted = FitSingleLineMemo(renderer, cache, lru, title, FontType::Regular, 20);
            if (cache.size() != lru.size() || cache.size() > FitMemo::kFitCacheLimit || fitted.size() * width > 20) {
                std::printf("FAIL cache=%zu lru=%zu iteration=%d\n", cache.size(), lru.size(), i);
                return 1;
            }
        }
    }
    std::printf("PASS 2000 font-width invalidations: cache=%zu, lru=%zu\n",cache.size(),lru.size());
}
