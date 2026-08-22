#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

namespace fc {

// Small LRU cache keyed by Key with hit/miss statistics.
//
// Purpose: the eviction policy core for the frame cache described in
// Module 3 ("frame caching with LRU eviction policy"). Stays dependency
// free so it can be unit tested in seconds on any CI runner.
template <typename Key, typename Value>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return items_.size(); }
    uint64_t hits() const noexcept { return hits_; }
    uint64_t misses() const noexcept { return misses_; }

    // Returns a pointer to the stored value (promoted to most recently
    // used) or nullptr on a miss. The pointer stays valid until the next
    // non-const call on this cache.
    Value *get(const Key &key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        items_.splice(items_.begin(), items_, it->second);
        return &it->second->second;
    }

    // Inserts or updates a key. When full, evicts the least recently
    // used entry first. A zero-capacity cache stores nothing.
    void put(const Key &key, Value value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = std::move(value);
            items_.splice(items_.begin(), items_, it->second);
            return;
        }
        if (capacity_ == 0) {
            return;
        }
        if (items_.size() >= capacity_) {
            index_.erase(items_.back().first);
            items_.pop_back();
        }
        items_.emplace_front(key, std::move(value));
        index_[key] = items_.begin();
    }

    bool erase(const Key &key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        items_.erase(it->second);
        index_.erase(it);
        return true;
    }

    // Drops all entries and resets hit/miss counters.
    void clear() {
        items_.clear();
        index_.clear();
        hits_ = 0;
        misses_ = 0;
    }

private:
    using List = std::list<std::pair<Key, Value>>;

    List items_;
    std::unordered_map<Key, typename List::iterator> index_;
    std::size_t capacity_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};

} // namespace fc
