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

#include <gtest/gtest.h>
#include <logic/resource_cache.h>

using namespace openxr_api_layer;

// ---------------------------------------------------------------------------
// ResourceCache — miss / hit / evictIf semantics
// ---------------------------------------------------------------------------

TEST(ResourceCacheTest, MissReturnsEnd) {
    ResourceCache<uint32_t, std::string> cache;
    EXPECT_EQ(cache.find(42), cache.end());
}

TEST(ResourceCacheTest, HitAfterInsert) {
    ResourceCache<uint32_t, std::string> cache;
    auto [it, inserted] = cache.insert(42, "hello");
    EXPECT_TRUE(inserted);
    EXPECT_EQ(it->first, 42u);
    EXPECT_EQ(it->second, "hello");

    auto hit = cache.find(42);
    EXPECT_NE(hit, cache.end());
    EXPECT_EQ(hit->second, "hello");
}

TEST(ResourceCacheTest, EvictIfRemovesMatchingEntries) {
    ResourceCache<uint32_t, int> cache;
    cache.insert(1, 10);
    cache.insert(2, 20);
    cache.insert(3, 30);

    EXPECT_EQ(cache.size(), 3u);

    // Evict entries where value > 15
    size_t evicted = cache.evictIf([](uint32_t, int val) { return val > 15; });
    EXPECT_EQ(evicted, 2u);
    EXPECT_EQ(cache.size(), 1u);

    // Key 1 remains (value 10 <= 15), keys 2 and 3 are gone
    auto remaining = cache.find(1);
    EXPECT_NE(remaining, cache.end());
    EXPECT_EQ(remaining->second, 10);
    EXPECT_EQ(cache.find(2), cache.end());
    EXPECT_EQ(cache.find(3), cache.end());
}

TEST(ResourceCacheTest, SizeAndClear) {
    ResourceCache<uint32_t, int> cache;
    EXPECT_EQ(cache.size(), 0u);

    cache.insert(1, 100);
    cache.insert(2, 200);
    EXPECT_EQ(cache.size(), 2u);

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.find(1), cache.end());
    EXPECT_EQ(cache.find(2), cache.end());
}
