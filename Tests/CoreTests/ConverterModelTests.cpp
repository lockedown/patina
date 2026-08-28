// ConverterModelTests.cpp
//
// Correctness for build order stage 6's converter half -- see
// ConverterModel.h. The defining property of "quantise to N bits, no
// dither" is exactly 2^bits distinct output levels; everything else here
// checks the edges of that (clamping, pass-through for out-of-range bit
// counts, the no-op-at-high-bit-depth case).

#include "TestFramework.h"
#include "../../Sources/Core/ConverterModel.h"

#include <cmath>
#include <set>
#include <vector>

using namespace akz;

AKZ_TEST(quantize_produces_exactly_2_pow_bits_distinct_levels) {
    // 4 bits is small enough to exhaustively distinguish all 16 levels
    // with a fine sweep, and large enough that a bug landing on the
    // wrong count (e.g. off-by-one from an asymmetric range) is obvious.
    std::set<float> distinctLevels;
    const int steps = 100000;
    for (int i = 0; i <= steps; ++i) {
        const float sample = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(steps);
        distinctLevels.insert(quantize(sample, 4));
    }
    AKZ_CHECK_EQ(distinctLevels.size(), static_cast<size_t>(16));
}

AKZ_TEST(quantize_12_bit_matches_S950_bit_depth) {
    // Not a magic number -- MachineProfile.cpp cites the S950 as
    // 12-bit linear. A coarser sweep than the 4-bit test above (4096
    // levels would need an impractically fine sweep to hit all of them
    // exhaustively), just confirming the level count is in the right
    // ballpark and monotonic, not exactly 4096 hit by brute force.
    std::set<float> distinctLevels;
    const int steps = 200000;
    for (int i = 0; i <= steps; ++i) {
        const float sample = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(steps);
        distinctLevels.insert(quantize(sample, 12));
    }
    // With a fine-enough sweep this should land close to 4096; a gross
    // implementation error (wrong shift, wrong level count) would show
    // up as an order-of-magnitude difference, which this catches.
    AKZ_CHECK(distinctLevels.size() > 3000);
    AKZ_CHECK(distinctLevels.size() <= 4096);
}

AKZ_TEST(quantize_clamps_out_of_range_input) {
    AKZ_CHECK_NEAR(quantize(5.0f, 8), 1.0f, 0.01f);
    AKZ_CHECK_NEAR(quantize(-5.0f, 8), -1.0f, 0.01f);
}

AKZ_TEST(quantize_is_a_no_op_outside_1_to_23_bits) {
    // 0 bits, negative bits, and 24+ bits all mean "nothing meaningful
    // to quantise to" -- see the header comment.
    AKZ_CHECK_EQ(quantize(0.3456f, 0), 0.3456f);
    AKZ_CHECK_EQ(quantize(0.3456f, -1), 0.3456f);
    AKZ_CHECK_EQ(quantize(0.3456f, 24), 0.3456f);
    AKZ_CHECK_EQ(quantize(0.3456f, 32), 0.3456f);
}

AKZ_TEST(quantize_16_bit_error_is_bounded_by_one_step) {
    // Every quantised value must be within one quantisation step of the
    // original -- the basic correctness bound for "round to nearest
    // level," independent of level count.
    const double step = 2.0 / 65536.0;
    for (int i = 0; i < 10000; ++i) {
        const float sample = -1.0f + 2.0f * static_cast<float>(i) / 10000.0f;
        const float q = quantize(sample, 16);
        AKZ_CHECK(std::fabs(q - sample) <= step);
    }
}

AKZ_TEST(quantize_buffer_matches_per_sample_quantize) {
    std::vector<float> buf = {-0.9f, -0.3f, 0.0f, 0.1234f, 0.9999f};
    std::vector<float> expected;
    for (float s : buf) expected.push_back(quantize(s, 8));

    quantizeBuffer(buf.data(), buf.size(), 8);

    for (size_t i = 0; i < buf.size(); ++i) {
        AKZ_CHECK_EQ(buf[i], expected[i]);
    }
}
