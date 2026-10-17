#pragma once

#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

namespace Aperture {

// Immutable snapshots share pointer blocks as well as their records. Replacing
// one record copies at most 128 references plus the small block directory.
// Mutable builders are confined to the model thread; published lists are const.
template<class T> class SnapshotRecords {
    static constexpr size_t blockSize = 128;
    using Value = std::shared_ptr<const T>;
    using Block = std::array<Value, blockSize>;
    std::vector<std::shared_ptr<Block>> blocks;
    size_t count = 0;

    Block& Writable(size_t block) {
        if (blocks[block].use_count() != 1) blocks[block] = std::make_shared<Block>(*blocks[block]);
        return *blocks[block];
    }
public:
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    const Value& operator[](size_t index) const { return (*blocks[index / blockSize])[index % blockSize]; }
    const Value& front() const { return (*this)[0]; }
    void reserve(size_t size) { blocks.reserve((size + blockSize - 1) / blockSize); }
    void push_back(Value value) {
        if (count % blockSize == 0) blocks.push_back(std::make_shared<Block>());
        Writable(count / blockSize)[count % blockSize] = std::move(value);
        ++count;
    }
    void Replace(size_t index, Value value) {
        if (index >= count) throw std::out_of_range("snapshot record");
        Writable(index / blockSize)[index % blockSize] = std::move(value);
    }
    void Truncate(size_t size) {
        if (size >= count) return;
        if (size % blockSize) {
            auto& tail = Writable(size / blockSize);
            for (size_t i = size % blockSize; i < blockSize; ++i) tail[i].reset();
        }
        blocks.resize((size + blockSize - 1) / blockSize);
        count = size;
    }
};

} // namespace Aperture
