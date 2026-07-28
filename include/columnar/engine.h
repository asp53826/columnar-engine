#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace columnar {

class ValidityBitmap {
 public:
  explicit ValidityBitmap(std::size_t size = 0, bool valid = true);
  bool valid(std::size_t row) const;
  void set(std::size_t row, bool valid);
  std::size_t size() const { return size_; }

 private:
  std::size_t size_ = 0;
  std::vector<std::uint64_t> words_;
};

template <typename T>
class Column {
 public:
  Column() = default;
  explicit Column(std::vector<T> values)
      : values_(std::move(values)), validity_(values_.size(), true) {}
  Column(std::vector<T> values, ValidityBitmap validity)
      : values_(std::move(values)), validity_(std::move(validity)) {
    if (values_.size() != validity_.size()) {
      throw std::invalid_argument("column/value validity size mismatch");
    }
  }

  std::size_t size() const { return values_.size(); }
  const T& value(std::size_t row) const { return values_.at(row); }
  bool valid(std::size_t row) const { return validity_.valid(row); }
  const std::vector<T>& values() const { return values_; }
  const ValidityBitmap& validity() const { return validity_; }

 private:
  std::vector<T> values_;
  ValidityBitmap validity_;
};

class Table {
 public:
  void add_int64(std::string name, Column<std::int64_t> column);
  void add_double(std::string name, Column<double> column);

  const Column<std::int64_t>& int64(const std::string& name) const;
  const Column<double>& real(const std::string& name) const;
  std::size_t rows() const { return rows_; }

 private:
  void accept_size(std::size_t size);
  std::size_t rows_ = 0;
  bool has_columns_ = false;
  std::unordered_map<std::string, Column<std::int64_t>> int64_columns_;
  std::unordered_map<std::string, Column<double>> double_columns_;
};

enum class Compare { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

using Selection = std::vector<std::uint32_t>;

Selection all_rows(std::size_t rows);

Selection filter(const Column<std::int64_t>& column, Compare comparison,
                 std::int64_t constant, const Selection* input = nullptr,
                 std::size_t batch_size = 1024);
Selection filter(const Column<double>& column, Compare comparison,
                 double constant, const Selection* input = nullptr,
                 std::size_t batch_size = 1024);

Selection scalar_filter(const Column<std::int64_t>& column, Compare comparison,
                        std::int64_t constant,
                        const Selection* input = nullptr);
Selection scalar_filter(const Column<double>& column, Compare comparison,
                        double constant, const Selection* input = nullptr);

struct Group {
  std::uint64_t count = 0;
  double sum = 0.0;

  bool operator==(const Group& other) const;
};

using Groups = std::map<std::int64_t, Group>;

Groups group_sum(const Column<std::int64_t>& keys,
                 const Column<double>& values, const Selection& selection,
                 std::size_t batch_size = 1024);
Groups scalar_group_sum(const Column<std::int64_t>& keys,
                        const Column<double>& values,
                        const Selection& selection);

using JoinPairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

JoinPairs hash_join(const Column<std::int64_t>& left,
                    const Column<std::int64_t>& right,
                    std::size_t batch_size = 1024);
JoinPairs nested_loop_join(const Column<std::int64_t>& left,
                           const Column<std::int64_t>& right);

}  // namespace columnar
