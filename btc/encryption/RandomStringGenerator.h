#pragma once

#include <vector>
#include <random>
#include <sstream>
#include <algorithm>

namespace encryption{
    typedef class RandomStringGenerator{
        private:

        public:
            RandomStringGenerator();
            std::vector<char> random_string(int length);
            std::string ascii_string(int length);
    }random_string_generator;
}
