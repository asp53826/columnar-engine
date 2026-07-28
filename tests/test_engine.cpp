#include "columnar/engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

int assertions = 0;
int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    ++assertions;                                                            \
    if (!(condition)) {                                                      \
      ++failures;                                                            \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: "       \
                << #condition << "\n";                                      \
    }                                                                        \
  } while (false)

void test_validity_bitmap_boundaries() {
  columnar::ValidityBitmap bitmap(130, true);
  bitmap.set(0, false);
  bitmap.set(63, false);
  bitmap.set(64, false);
  bitmap.set(129, false);
  CHECK(!bitmap.valid(0));
  CHECK(!bitmap.valid(63));
  CHECK(!bitmap.valid(64));
  CHECK(!bitmap.valid(129));
  CHECK(bitmap.valid(1));
  CHECK(bitmap.valid(128));
}

void test_table_schema_invariants() {
  columnar::Table table;
  table.add_int64("id", columnar::Column<std::int64_t>({1, 2, 3}));
  table.add_double("value", columnar::Column<double>({1.0, 2.0, 3.0}));
  CHECK(table.rows() == 3);
  CHECK(table.int64("id").value(1) == 2);
  bool mismatch = false;
  try {
    table.add_double("bad", columnar::Column<double>({1.0}));
  } catch (const std::invalid_argument&) {
    mismatch = true;
  }
  CHECK(mismatch);
}

void test_null_predicate_semantics() {
  columnar::ValidityBitmap validity(5, true);
  validity.set(2, false);
  columnar::Column<std::int64_t> column({1, 2, 100, 4, 5}, validity);
  const auto selected =
      columnar::filter(column, columnar::Compare::Greater, 2);
  CHECK(selected == columnar::Selection({3, 4}));
  CHECK(selected == columnar::scalar_filter(
                        column, columnar::Compare::Greater, 2));
}

void test_chained_selection_preserves_order() {
  columnar::Column<std::int64_t> region({2, 1, 2, 2, 1, 2});
  columnar::Column<double> revenue({5, 50, 20, 10, 90, 40});
  const auto first =
      columnar::filter(region, columnar::Compare::Equal, 2, nullptr, 2);
  const auto second = columnar::filter(
      revenue, columnar::Compare::GreaterEqual, 20.0, &first, 3);
  CHECK(first == columnar::Selection({0, 2, 3, 5}));
  CHECK(second == columnar::Selection({2, 5}));
}

void test_group_sum_ignores_nulls() {
  columnar::ValidityBitmap key_validity(5, true);
  key_validity.set(3, false);
  columnar::ValidityBitmap value_validity(5, true);
  value_validity.set(1, false);
  columnar::Column<std::int64_t> keys({1, 1, 2, 2, 1}, key_validity);
  columnar::Column<double> values({10, 999, 5, 999, 7}, value_validity);
  const auto selection = columnar::all_rows(5);
  const auto groups = columnar::group_sum(keys, values, selection, 2);
  CHECK(groups.size() == 2);
  CHECK((groups.at(1) == columnar::Group{2, 17}));
  CHECK((groups.at(2) == columnar::Group{1, 5}));
  CHECK(groups == columnar::scalar_group_sum(keys, values, selection));
}

void test_hash_join_duplicate_and_null_semantics() {
  columnar::ValidityBitmap left_validity(4, true);
  left_validity.set(3, false);
  columnar::ValidityBitmap right_validity(4, true);
  right_validity.set(3, false);
  columnar::Column<std::int64_t> left({1, 2, 2, 2}, left_validity);
  columnar::Column<std::int64_t> right({2, 2, 3, 2}, right_validity);
  auto actual = columnar::hash_join(left, right, 2);
  auto expected = columnar::nested_loop_join(left, right);
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  CHECK(actual == expected);
  CHECK(actual.size() == 4);
}

void test_randomized_filter_and_aggregate_differential() {
  std::mt19937_64 random(20260728);
  const std::vector<columnar::Compare> comparisons = {
      columnar::Compare::Equal,   columnar::Compare::NotEqual,
      columnar::Compare::Less,    columnar::Compare::LessEqual,
      columnar::Compare::Greater, columnar::Compare::GreaterEqual};
  const std::vector<std::size_t> batches = {1, 7, 64, 1024};

  for (int trial = 0; trial < 120; ++trial) {
    const std::size_t size = random() % 600;
    std::vector<std::int64_t> integers(size);
    std::vector<double> reals(size);
    std::vector<std::int64_t> groups(size);
    columnar::ValidityBitmap int_validity(size, true);
    columnar::ValidityBitmap real_validity(size, true);
    for (std::size_t row = 0; row < size; ++row) {
      integers[row] = static_cast<std::int64_t>(random() % 101) - 50;
      reals[row] = static_cast<double>(random() % 10000) / 37.0;
      groups[row] = static_cast<std::int64_t>(random() % 13);
      if (random() % 11 == 0) {
        int_validity.set(row, false);
      }
      if (random() % 13 == 0) {
        real_validity.set(row, false);
      }
    }
    const columnar::Column<std::int64_t> int_column(integers, int_validity);
    const columnar::Column<double> real_column(reals, real_validity);
    const columnar::Column<std::int64_t> group_column(groups);

    for (const auto comparison : comparisons) {
      const std::int64_t constant =
          static_cast<std::int64_t>(random() % 101) - 50;
      const auto expected = columnar::scalar_filter(
          int_column, comparison, constant);
      for (const auto batch : batches) {
        const auto actual =
            columnar::filter(int_column, comparison, constant, nullptr, batch);
        CHECK(actual == expected);
        const auto vector_groups =
            columnar::group_sum(group_column, real_column, actual, batch);
        const auto scalar_groups =
            columnar::scalar_group_sum(group_column, real_column, expected);
        CHECK(vector_groups == scalar_groups);
      }
    }
  }
}

void test_randomized_join_differential() {
  std::mt19937 random(8128);
  for (int trial = 0; trial < 100; ++trial) {
    const std::size_t left_size = random() % 80;
    const std::size_t right_size = random() % 80;
    std::vector<std::int64_t> left_values(left_size);
    std::vector<std::int64_t> right_values(right_size);
    columnar::ValidityBitmap left_validity(left_size, true);
    columnar::ValidityBitmap right_validity(right_size, true);
    for (std::size_t row = 0; row < left_size; ++row) {
      left_values[row] = random() % 20;
      if (random() % 9 == 0) {
        left_validity.set(row, false);
      }
    }
    for (std::size_t row = 0; row < right_size; ++row) {
      right_values[row] = random() % 20;
      if (random() % 9 == 0) {
        right_validity.set(row, false);
      }
    }
    const columnar::Column<std::int64_t> left(left_values, left_validity);
    const columnar::Column<std::int64_t> right(right_values, right_validity);
    auto actual = columnar::hash_join(left, right, 13);
    auto expected = columnar::nested_loop_join(left, right);
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    CHECK(actual == expected);
  }
}

void test_tpch_q1_known_answer() {
  columnar::LineItemTable table{
      columnar::Column<double>({10, 20, 30}),
      columnar::Column<double>({100, 200, 300}),
      columnar::Column<double>({0.10, 0.20, 0.30}),
      columnar::Column<double>({0.05, 0.10, 0.15}),
      columnar::Column<std::int64_t>({0, 0, 1}),
      columnar::Column<std::int64_t>({1, 1, 0}),
      columnar::Column<std::int64_t>({10, 20, 30})};
  const auto result = columnar::tpch_q1(table, 20, 2);
  CHECK(result.size() == 1);
  const auto& aggregate = result.at({0, 1});
  CHECK(aggregate.count == 2);
  CHECK(std::abs(aggregate.sum_quantity - 30.0) < 1e-12);
  CHECK(std::abs(aggregate.sum_base_price - 300.0) < 1e-12);
  CHECK(std::abs(aggregate.sum_discounted_price - 250.0) < 1e-12);
  CHECK(std::abs(aggregate.sum_charge - 270.5) < 1e-12);
  CHECK(std::abs(aggregate.average_discount() - 0.15) < 1e-12);
  CHECK(result == columnar::scalar_tpch_q1(table, 20));
}

void test_randomized_tpch_q1_differential() {
  std::mt19937_64 random(8675309);
  for (int trial = 0; trial < 100; ++trial) {
    const std::size_t size = random() % 1000;
    std::vector<double> quantity(size);
    std::vector<double> price(size);
    std::vector<double> discount(size);
    std::vector<double> tax(size);
    std::vector<std::int64_t> flag(size);
    std::vector<std::int64_t> status(size);
    std::vector<std::int64_t> ship_date(size);
    columnar::ValidityBitmap discount_validity(size, true);
    for (std::size_t row = 0; row < size; ++row) {
      quantity[row] = 1.0 + random() % 50;
      price[row] = static_cast<double>(random() % 100000) / 100.0;
      discount[row] = static_cast<double>(random() % 11) / 100.0;
      tax[row] = static_cast<double>(random() % 9) / 100.0;
      flag[row] = random() % 3;
      status[row] = random() % 2;
      ship_date[row] = random() % 2500;
      if (random() % 101 == 0) {
        discount_validity.set(row, false);
      }
    }
    const columnar::LineItemTable table{
        columnar::Column<double>(std::move(quantity)),
        columnar::Column<double>(std::move(price)),
        columnar::Column<double>(std::move(discount), discount_validity),
        columnar::Column<double>(std::move(tax)),
        columnar::Column<std::int64_t>(std::move(flag)),
        columnar::Column<std::int64_t>(std::move(status)),
        columnar::Column<std::int64_t>(std::move(ship_date))};
    const std::int64_t cutoff = random() % 2500;
    const auto expected = columnar::scalar_tpch_q1(table, cutoff);
    for (const std::size_t batch : {1U, 7U, 64U, 1024U}) {
      CHECK(columnar::tpch_q1(table, cutoff, batch) == expected);
    }
  }
}

void test_invalid_selection_is_rejected() {
  columnar::Column<double> values({1.0, 2.0});
  const columnar::Selection invalid = {0, 7};
  bool rejected = false;
  try {
    static_cast<void>(columnar::filter(
        values, columnar::Compare::Greater, 0.0, &invalid));
  } catch (const std::out_of_range&) {
    rejected = true;
  }
  CHECK(rejected);
}

}  // namespace

int main() {
  test_validity_bitmap_boundaries();
  test_table_schema_invariants();
  test_null_predicate_semantics();
  test_chained_selection_preserves_order();
  test_group_sum_ignores_nulls();
  test_hash_join_duplicate_and_null_semantics();
  test_randomized_filter_and_aggregate_differential();
  test_randomized_join_differential();
  test_tpch_q1_known_answer();
  test_randomized_tpch_q1_differential();
  test_invalid_selection_is_rejected();
  if (failures != 0) {
    std::cerr << failures << " of " << assertions << " assertions failed\n";
    return 1;
  }
  std::cout << assertions << " assertions passed\n";
  return 0;
}
