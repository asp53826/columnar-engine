#include "columnar/engine.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

struct Dataset {
  columnar::Column<std::int64_t> region;
  columnar::Column<std::int64_t> quantity;
  columnar::Column<double> revenue;
};

Dataset generate(std::size_t rows) {
  std::mt19937_64 random(20260728);
  std::vector<std::int64_t> regions(rows);
  std::vector<std::int64_t> quantities(rows);
  std::vector<double> revenue(rows);
  columnar::ValidityBitmap revenue_validity(rows, true);
  for (std::size_t row = 0; row < rows; ++row) {
    regions[row] = random() % 16;
    quantities[row] = random() % 100;
    revenue[row] = static_cast<double>(random() % 100000) / 100.0;
    if (random() % 97 == 0) {
      revenue_validity.set(row, false);
    }
  }
  return {columnar::Column<std::int64_t>(std::move(regions)),
          columnar::Column<std::int64_t>(std::move(quantities)),
          columnar::Column<double>(std::move(revenue), revenue_validity)};
}

template <typename Function>
double measure(Function function, int repetitions, std::uint64_t& checksum) {
  const auto started = std::chrono::steady_clock::now();
  for (int repetition = 0; repetition < repetitions; ++repetition) {
    const auto groups = function();
    for (const auto& [key, group] : groups) {
      checksum += static_cast<std::uint64_t>(key + 1) * group.count;
    }
  }
  return std::chrono::duration<double, std::micro>(
             std::chrono::steady_clock::now() - started)
             .count() /
         repetitions;
}

int benchmark() {
  std::cout << "rows,selectivity,scalar_us,vector_us,speedup\n";
  std::uint64_t checksum = 0;
  for (const std::size_t rows : {1000U, 10000U, 100000U, 1000000U}) {
    const Dataset dataset = generate(rows);
    for (const std::int64_t threshold : {90, 50, 10}) {
      const auto scalar = [&] {
        const auto quantity = columnar::scalar_filter(
            dataset.quantity, columnar::Compare::GreaterEqual, threshold);
        const auto revenue = columnar::scalar_filter(
            dataset.revenue, columnar::Compare::GreaterEqual, 250.0,
            &quantity);
        return columnar::scalar_group_sum(dataset.region, dataset.revenue,
                                          revenue);
      };
      const auto vector = [&] {
        const auto quantity = columnar::filter(
            dataset.quantity, columnar::Compare::GreaterEqual, threshold,
            nullptr, 1024);
        const auto revenue = columnar::filter(
            dataset.revenue, columnar::Compare::GreaterEqual, 250.0,
            &quantity, 1024);
        return columnar::group_sum(dataset.region, dataset.revenue, revenue,
                                   1024);
      };
      if (vector() != scalar()) {
        throw std::runtime_error("benchmark executors disagree");
      }
      const int repetitions = rows < 100000 ? 20 : 5;
      const double scalar_us = measure(scalar, repetitions, checksum);
      const double vector_us = measure(vector, repetitions, checksum);
      std::uint64_t selected_rows = 0;
      for (const auto& [group_key, group] : vector()) {
        static_cast<void>(group_key);
        selected_rows += group.count;
      }
      const double selectivity =
          100.0 * selected_rows / static_cast<double>(rows);
      std::cout << rows << "," << std::fixed << std::setprecision(2)
                << selectivity << "," << scalar_us << "," << vector_us << ","
                << scalar_us / vector_us << "\n";
    }
  }
  std::cerr << "checksum=" << checksum << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "benchmark") {
      return benchmark();
    }
    std::cerr << "usage: columnar_tool benchmark\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
