#include "columnar/engine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace columnar {
namespace {

template <typename T>
bool compare(T value, Compare operation, T constant) {
  switch (operation) {
    case Compare::Equal:
      return value == constant;
    case Compare::NotEqual:
      return value != constant;
    case Compare::Less:
      return value < constant;
    case Compare::LessEqual:
      return value <= constant;
    case Compare::Greater:
      return value > constant;
    case Compare::GreaterEqual:
      return value >= constant;
  }
  return false;
}

template <typename T>
Selection vector_filter(const Column<T>& column, Compare operation, T constant,
                        const Selection* input, std::size_t batch_size) {
  if (batch_size == 0) {
    throw std::invalid_argument("batch size must be positive");
  }
  Selection output;
  output.reserve(input == nullptr ? column.size() : input->size());
  const std::size_t input_size = input == nullptr ? column.size() : input->size();
  std::vector<std::uint8_t> mask(batch_size);
  for (std::size_t base = 0; base < input_size; base += batch_size) {
    const std::size_t count = std::min(batch_size, input_size - base);
    for (std::size_t lane = 0; lane < count; ++lane) {
      const std::size_t row = input == nullptr ? base + lane : (*input)[base + lane];
      if (row >= column.size()) {
        throw std::out_of_range("selection row exceeds column");
      }
      mask[lane] = static_cast<std::uint8_t>(
          column.valid(row) &&
          compare(column.values()[row], operation, constant));
    }
    for (std::size_t lane = 0; lane < count; ++lane) {
      if (mask[lane] != 0) {
        output.push_back(static_cast<std::uint32_t>(
            input == nullptr ? base + lane : (*input)[base + lane]));
      }
    }
  }
  return output;
}

template <typename T>
Selection tuple_filter(const Column<T>& column, Compare operation, T constant,
                       const Selection* input) {
  Selection output;
  const std::size_t size = input == nullptr ? column.size() : input->size();
  for (std::size_t index = 0; index < size; ++index) {
    const std::size_t row = input == nullptr ? index : (*input)[index];
    if (row >= column.size()) {
      throw std::out_of_range("selection row exceeds column");
    }
    if (column.valid(row) &&
        compare(column.value(row), operation, constant)) {
      output.push_back(static_cast<std::uint32_t>(row));
    }
  }
  return output;
}

}  // namespace

ValidityBitmap::ValidityBitmap(std::size_t size, bool valid)
    : size_(size),
      words_((size + 63) / 64,
             valid ? std::numeric_limits<std::uint64_t>::max() : 0) {
  if (valid && size % 64 != 0 && !words_.empty()) {
    words_.back() = (1ULL << (size % 64)) - 1ULL;
  }
}

bool ValidityBitmap::valid(std::size_t row) const {
  if (row >= size_) {
    throw std::out_of_range("validity row exceeds bitmap");
  }
  return (words_[row / 64] & (1ULL << (row % 64))) != 0;
}

void ValidityBitmap::set(std::size_t row, bool valid_value) {
  if (row >= size_) {
    throw std::out_of_range("validity row exceeds bitmap");
  }
  const std::uint64_t bit = 1ULL << (row % 64);
  if (valid_value) {
    words_[row / 64] |= bit;
  } else {
    words_[row / 64] &= ~bit;
  }
}

void Table::accept_size(std::size_t size) {
  if (has_columns_ && rows_ != size) {
    throw std::invalid_argument("all table columns must have equal length");
  }
  rows_ = size;
  has_columns_ = true;
}

void Table::add_int64(std::string name, Column<std::int64_t> column) {
  accept_size(column.size());
  if (!int64_columns_.emplace(std::move(name), std::move(column)).second) {
    throw std::invalid_argument("duplicate int64 column name");
  }
}

void Table::add_double(std::string name, Column<double> column) {
  accept_size(column.size());
  if (!double_columns_.emplace(std::move(name), std::move(column)).second) {
    throw std::invalid_argument("duplicate double column name");
  }
}

const Column<std::int64_t>& Table::int64(const std::string& name) const {
  return int64_columns_.at(name);
}

const Column<double>& Table::real(const std::string& name) const {
  return double_columns_.at(name);
}

Selection all_rows(std::size_t rows) {
  if (rows > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("selection exceeds 32-bit row identifiers");
  }
  Selection selection(rows);
  std::iota(selection.begin(), selection.end(), 0U);
  return selection;
}

Selection filter(const Column<std::int64_t>& column, Compare operation,
                 std::int64_t constant, const Selection* input,
                 std::size_t batch_size) {
  return vector_filter(column, operation, constant, input, batch_size);
}

Selection filter(const Column<double>& column, Compare operation,
                 double constant, const Selection* input,
                 std::size_t batch_size) {
  return vector_filter(column, operation, constant, input, batch_size);
}

Selection scalar_filter(const Column<std::int64_t>& column, Compare operation,
                        std::int64_t constant, const Selection* input) {
  return tuple_filter(column, operation, constant, input);
}

Selection scalar_filter(const Column<double>& column, Compare operation,
                        double constant, const Selection* input) {
  return tuple_filter(column, operation, constant, input);
}

bool Group::operator==(const Group& other) const {
  return count == other.count &&
         std::abs(sum - other.sum) <=
             1e-10 * std::max({1.0, std::abs(sum), std::abs(other.sum)});
}

Groups group_sum(const Column<std::int64_t>& keys,
                 const Column<double>& values, const Selection& selection,
                 std::size_t batch_size) {
  if (keys.size() != values.size() || batch_size == 0) {
    throw std::invalid_argument("invalid aggregation inputs");
  }
  std::unordered_map<std::int64_t, Group> hash;
  for (std::size_t base = 0; base < selection.size(); base += batch_size) {
    const std::size_t end = std::min(selection.size(), base + batch_size);
    for (std::size_t index = base; index < end; ++index) {
      const std::size_t row = selection[index];
      if (row >= keys.size()) {
        throw std::out_of_range("selection row exceeds aggregation column");
      }
      if (!keys.valid(row) || !values.valid(row)) {
        continue;
      }
      Group& group = hash[keys.value(row)];
      ++group.count;
      group.sum += values.value(row);
    }
  }
  return Groups(hash.begin(), hash.end());
}

Groups scalar_group_sum(const Column<std::int64_t>& keys,
                        const Column<double>& values,
                        const Selection& selection) {
  if (keys.size() != values.size()) {
    throw std::invalid_argument("invalid aggregation inputs");
  }
  Groups groups;
  for (const std::uint32_t row : selection) {
    if (row >= keys.size()) {
      throw std::out_of_range("selection row exceeds aggregation column");
    }
    if (keys.valid(row) && values.valid(row)) {
      Group& group = groups[keys.value(row)];
      ++group.count;
      group.sum += values.value(row);
    }
  }
  return groups;
}

JoinPairs hash_join(const Column<std::int64_t>& left,
                    const Column<std::int64_t>& right,
                    std::size_t batch_size) {
  if (batch_size == 0) {
    throw std::invalid_argument("batch size must be positive");
  }
  std::unordered_map<std::int64_t, std::vector<std::uint32_t>> hash;
  for (std::size_t base = 0; base < right.size(); base += batch_size) {
    const std::size_t end = std::min(right.size(), base + batch_size);
    for (std::size_t row = base; row < end; ++row) {
      if (right.valid(row)) {
        hash[right.value(row)].push_back(static_cast<std::uint32_t>(row));
      }
    }
  }
  JoinPairs pairs;
  for (std::size_t base = 0; base < left.size(); base += batch_size) {
    const std::size_t end = std::min(left.size(), base + batch_size);
    for (std::size_t row = base; row < end; ++row) {
      if (!left.valid(row)) {
        continue;
      }
      const auto matches = hash.find(left.value(row));
      if (matches != hash.end()) {
        for (const auto right_row : matches->second) {
          pairs.push_back({static_cast<std::uint32_t>(row), right_row});
        }
      }
    }
  }
  return pairs;
}

JoinPairs nested_loop_join(const Column<std::int64_t>& left,
                           const Column<std::int64_t>& right) {
  JoinPairs pairs;
  for (std::size_t left_row = 0; left_row < left.size(); ++left_row) {
    if (!left.valid(left_row)) {
      continue;
    }
    for (std::size_t right_row = 0; right_row < right.size(); ++right_row) {
      if (right.valid(right_row) &&
          left.value(left_row) == right.value(right_row)) {
        pairs.push_back({static_cast<std::uint32_t>(left_row),
                         static_cast<std::uint32_t>(right_row)});
      }
    }
  }
  return pairs;
}

std::size_t LineItemTable::rows() const {
  const std::size_t count = quantity.size();
  if (extended_price.size() != count || discount.size() != count ||
      tax.size() != count || return_flag.size() != count ||
      line_status.size() != count || ship_date.size() != count) {
    throw std::invalid_argument("lineitem columns have unequal lengths");
  }
  return count;
}

double Q1Aggregate::average_quantity() const {
  return count == 0 ? 0.0 : sum_quantity / static_cast<double>(count);
}

double Q1Aggregate::average_price() const {
  return count == 0 ? 0.0 : sum_base_price / static_cast<double>(count);
}

double Q1Aggregate::average_discount() const {
  return count == 0 ? 0.0 : sum_discount / static_cast<double>(count);
}

bool Q1Aggregate::operator==(const Q1Aggregate& other) const {
  const auto close = [](double left, double right) {
    return std::abs(left - right) <=
           1e-10 * std::max({1.0, std::abs(left), std::abs(right)});
  };
  return count == other.count && close(sum_quantity, other.sum_quantity) &&
         close(sum_base_price, other.sum_base_price) &&
         close(sum_discounted_price, other.sum_discounted_price) &&
         close(sum_charge, other.sum_charge) &&
         close(sum_discount, other.sum_discount);
}

namespace {

bool q1_row_valid(const LineItemTable& table, std::size_t row) {
  return table.quantity.valid(row) && table.extended_price.valid(row) &&
         table.discount.valid(row) && table.tax.valid(row) &&
         table.return_flag.valid(row) && table.line_status.valid(row) &&
         table.ship_date.valid(row);
}

void q1_update(Q1Aggregate& aggregate, const LineItemTable& table,
               std::size_t row) {
  const double price = table.extended_price.value(row);
  const double discount = table.discount.value(row);
  const double discounted = price * (1.0 - discount);
  aggregate.sum_quantity += table.quantity.value(row);
  aggregate.sum_base_price += price;
  aggregate.sum_discounted_price += discounted;
  aggregate.sum_charge += discounted * (1.0 + table.tax.value(row));
  aggregate.sum_discount += discount;
  ++aggregate.count;
}

}  // namespace

Q1Result tpch_q1(const LineItemTable& table,
                 std::int64_t maximum_ship_date, std::size_t batch_size) {
  const std::size_t rows = table.rows();
  if (batch_size == 0) {
    throw std::invalid_argument("batch size must be positive");
  }
  Q1Result result;
  std::vector<std::uint8_t> mask(batch_size);
  for (std::size_t base = 0; base < rows; base += batch_size) {
    const std::size_t count = std::min(batch_size, rows - base);
    for (std::size_t lane = 0; lane < count; ++lane) {
      const std::size_t row = base + lane;
      mask[lane] = static_cast<std::uint8_t>(
          q1_row_valid(table, row) &&
          table.ship_date.value(row) <= maximum_ship_date);
    }
    for (std::size_t lane = 0; lane < count; ++lane) {
      if (mask[lane] == 0) {
        continue;
      }
      const std::size_t row = base + lane;
      const Q1Key key{table.return_flag.value(row),
                      table.line_status.value(row)};
      q1_update(result[key], table, row);
    }
  }
  return result;
}

Q1Result scalar_tpch_q1(const LineItemTable& table,
                        std::int64_t maximum_ship_date) {
  Q1Result result;
  for (std::size_t row = 0; row < table.rows(); ++row) {
    if (!q1_row_valid(table, row) ||
        table.ship_date.value(row) > maximum_ship_date) {
      continue;
    }
    const Q1Key key{table.return_flag.value(row),
                    table.line_status.value(row)};
    q1_update(result[key], table, row);
  }
  return result;
}

}  // namespace columnar
