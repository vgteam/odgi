#pragma once

#include <vector>
#include <utility>
#include <algorithm>
#include <limits>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <iomanip> // std::setprecision
#include <handlegraph/handle_graph.hpp>
#include <handlegraph/util.hpp>
#include <handlegraph/path_handle_graph.hpp>
#include "progress.hpp"

namespace odgi {
    namespace algorithms {

        using namespace std;
        using namespace handlegraph;

        void bin_path_coverage(const PathHandleGraph &graph,
                           uint64_t num_bins = 0,
                           uint64_t bin_width = 0,
                           bool progress = false);
    }
}