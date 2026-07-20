// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <unordered_map>
#include <functional>

namespace openxr_api_layer {

    /// Simple LRU-style resource cache with find/insert/evictIf semantics.
    /// Not thread-safe — caller holds the mutex before operations.
    ///
    /// @tparam TKey   Key type (must be hashable via std::hash)
    /// @tparam TValue Value type (stored by value, copied on insert)
    template <typename TKey, typename TValue>
    class ResourceCache {
    public:
        /// Lookup key. Returns iterator to entry, or end() if not found.
        auto find(const TKey& key) { return m_cache.find(key); }
        auto find(const TKey& key) const { return m_cache.find(key); }

        /// Returns end iterator (convenience for find comparisons).
        auto end() { return m_cache.end(); }
        auto end() const { return m_cache.end(); }

        /// Insert or update entry. Returns {iterator, inserted}.
        auto insert(const TKey& key, const TValue& value) {
            return m_cache.emplace(key, value);
        }

        /// Remove entries where predicate(key, value) returns true.
        /// Returns count of evicted entries.
        size_t evictIf(std::function<bool(const TKey&, const TValue&)> predicate) {
            size_t evicted = 0;
            auto it = m_cache.begin();
            while (it != m_cache.end()) {
                if (predicate(it->first, it->second)) {
                    it = m_cache.erase(it);
                    ++evicted;
                } else {
                    ++it;
                }
            }
            return evicted;
        }

        /// Current number of cached entries.
        size_t size() const { return m_cache.size(); }

        /// Remove all entries.
        void clear() { m_cache.clear(); }

    private:
        std::unordered_map<TKey, TValue> m_cache;
    };

} // namespace openxr_api_layer
