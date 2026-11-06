// The groups of checks run by dva_tests (main.cpp).
#pragma once

#include <string>

void core_tests(const std::string& golden_path);
void theme_tests();
void fuzz_tests();
void dsp_tests();
void play_tests();
