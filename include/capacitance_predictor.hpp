// capacitance_predictor.hpp
// Auto-generated capacitance prediction functions
// Based on statistical analysis of PEX results
// SAE model improved: R² 0.8746 → 0.9860 (piecewise quadratic regression)

#ifndef CAPACITANCE_PREDICTOR_HPP
#define CAPACITANCE_PREDICTOR_HPP

#include <map>
#include <string>
#include <algorithm>

namespace OpenFinRAM {

class CapacitancePredictor {
public:

    // WL: R² = 0.9874 (linear)
    double predict_wl(int bit_num, int stacked) {
        return -0.0000284373
             + 0.0000000076 * bit_num
             + 0.0000375906 * stacked;
    }

    // YSEL: R² = 0.9623 (quadratic)
    double predict_ysel(int bit_num, int stacked) {
        return 0.0000746758
             + 0.0000000952 * bit_num
             + 0.0000084173 * stacked
             + -0.0000000001 * bit_num * bit_num
             + -0.0000000014 * bit_num * stacked
             + 0.0000003746 * stacked * stacked;
    }

    // BLPRECHN: R² = 1.0000 (linear)
    double predict_blprechn(int bit_num, int stacked) {
        return 0.0000047503
             + 0.0000000001 * bit_num
             + 0.0000404337 * stacked;
    }

    // WRENA: R² = 0.9776 (linear)
    double predict_wrena(int bit_num, int stacked) {
        return -0.0000339219
             + 0.0000000096 * bit_num
             + 0.0000349225 * stacked;
    }

    // SAE: R² = 0.9860 (piecewise quadratic regression)
    // Uses different models for stacked <= 41 and stacked > 41
    double predict_sae(int bit_num, int stacked) {
        if (stacked <= 41) {
            // Low stacked model (stacked <= 41)
            return 0.0000446904
                 + 0.0000000000 * bit_num
                 + 0.0000066220 * stacked
                 + -0.0000000000 * bit_num * bit_num
                 + -0.0000000000 * bit_num * stacked
                 + 0.0000012697 * stacked * stacked;
        } else {
            // High stacked model (stacked > 41)
            return 0.0056369119
                 + -0.0000000000 * bit_num
                 + -0.0000000095 * stacked
                 + -0.0000000000 * bit_num * bit_num
                 + 0.0000000000 * bit_num * stacked
                 + -0.0000010023 * stacked * stacked;
        }
    }

    // Get all capacitances for a given configuration
    std::map<std::string, double> predict_all(int bit_num, int stacked) {
        std::map<std::string, double> capacitances;
        capacitances["WLT"] = std::max(0.0, predict_wl(bit_num, stacked));
        capacitances["YSELT"] = std::max(0.0, predict_ysel(bit_num, stacked));
        capacitances["BLPRECHTN"] = std::max(0.0, predict_blprechn(bit_num, stacked));
        capacitances["WRENA"] = std::max(0.0, predict_wrena(bit_num, stacked));
        capacitances["SAE"] = std::max(0.0, predict_sae(bit_num, stacked));
        return capacitances;
    }

    // Get capacitance for a specific signal
    double predict(const std::string& signal, int bit_num, int stacked) {
        auto all_caps = predict_all(bit_num, stacked);
        auto it = all_caps.find(signal);
        if (it != all_caps.end()) {
            return it->second;
        }
        return 0.0;
    }
};

} // namespace OpenFinRAM

#endif // CAPACITANCE_PREDICTOR_HPP
