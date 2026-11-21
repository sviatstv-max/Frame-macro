// Build & run:  g++ -std=c++20 -I src tests/test_macro.cpp -o test_macro && ./test_macro
#include <cassert>
#include <cstdio>

#include "macro.hpp"

using namespace fm;

int main() {
    // bucket boundaries
    int const cases[][2] = {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {5, 4}, {6, 4}, {7, 5}, {8, 5},
                            {9, 6}, {12, 6}, {13, 7}, {16, 7}, {17, 8}, {100000, 8}};
    for (auto const& c : cases) assert(bucketOf(c[0]) == c[1]);
    assert(bucketOf(0) == -1);

    // gaps: 10->11 =1, 11->13 =2, 13->13 skipped, 13->30 =17
    Macro m;
    m.inputs = {{10, 1, true, true}, {11, 1, false, true}, {13, 1, true, true}, {13, 1, true, false}, {30, 1, false, true}};
    auto all = GapStats::build(m.inputs, m.inputs.size(), false);
    assert(all.n[0] == 1 && all.n[1] == 1 && all.n[8] == 1);
    uint32_t total = 0;
    for (auto v : all.n) total += v;
    assert(total == 3);

    // presses only: frames 10, 13, 13 -> one gap of 3
    auto presses = GapStats::build(m.inputs, m.inputs.size(), true);
    assert(presses.n[2] == 1);

    // a partial build only sees the first `count` inputs
    auto firstTwo = GapStats::build(m.inputs, 2, false);
    assert(firstTwo.n[0] == 1 && firstTwo.n[1] == 0);

    // round trip
    Macro back;
    assert(Macro::parse(m.serialize(), back) && back.inputs == m.inputs);
    assert(Macro::parse("framemacro 1\r\n5 1 1 1\r\n", back) && back.inputs.size() == 1);  // CRLF is fine

    // garbage is rejected and leaves `back` alone
    Macro keep = back;
    for (char const* bad : {"nope", "", "framemacro 2\n", "framemacro 1\n5 9 1 1\n", "framemacro 1\n5 1 2 1\n",
                            "framemacro 1\n5 1 1\n", "framemacro 1\n5 1 1 1 7\n", "framemacro 1\n-5 1 1 1\n",
                            "framemacro 1\n5 1 1 1\n4 1 0 1\n", "framemacro 1\nx y z w\n"}) {
        assert(!Macro::parse(bad, back));
        assert(back.inputs == keep.inputs);
    }

    // truncation / lookup
    assert(m.lowerBound(0) == 0 && m.lowerBound(13) == 2 && m.lowerBound(31) == 5);
    assert(m.length() == 30);
    m.truncateFrom(13);
    assert(m.inputs.size() == 2 && m.length() == 11);
    m.truncateFrom(0);
    assert(m.inputs.empty() && m.length() == 0);

    std::puts("macro tests passed");
}
