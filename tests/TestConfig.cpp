// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"
#include <gtest/gtest.h>
#include "logic/config.h"

namespace openxr_api_layer {

    TEST(ConfigParserTest, ClampsOutOfRange) {
        FoveationConfig cfg;
        cfg.ParseConfigurationStatement("peripheral_multiplier=0.0", 1, true, "", "");
        EXPECT_FLOAT_EQ(cfg.m_peripheralPixelDensity, 0.1f); // clamped to min
    }

    TEST(ConfigParserTest, SectionAppExactMatch) {
        FoveationConfig cfg;
        // Should not activate for "OtherGame"
        EXPECT_FALSE(cfg.ParseConfigurationStatement("[app:exact:OtherGame]", 1, true, "DCS", ""));
        // Should activate for "DCS"
        EXPECT_TRUE(cfg.ParseConfigurationStatement("[app:exact:DCS]", 2, false, "DCS", ""));
    }

    TEST(ConfigParserTest, IgnoresComments) {
        FoveationConfig cfg;
        // Should return true (active state unchanged) and not crash
        EXPECT_TRUE(cfg.ParseConfigurationStatement("# a comment", 1, true, "", ""));
        EXPECT_TRUE(cfg.ParseConfigurationStatement("// another", 2, true, "", ""));
    }

    TEST(ConfigParserTest, MalformedSectionHeaderDoesNotCrash) {
        FoveationConfig cfg;
        EXPECT_NO_THROW(cfg.ParseConfigurationStatement("[]", 1, true, "", ""));
        EXPECT_NO_THROW(cfg.ParseConfigurationStatement("[a]", 2, true, "", ""));
    }

} // namespace openxr_api_layer
