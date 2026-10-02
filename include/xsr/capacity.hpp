// Cross-Site Reservation runtime — canonical capacity vectors.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xsr/canonical.hpp"
#include "xsr/digest.hpp"
#include "xsr/error.hpp"
#include "xsr/ids.hpp"
#include "xsr/limits.hpp"

namespace xsr {

struct Dimension {
  DimensionName name;
  std::uint64_t quantity = 0;

  friend bool operator==(const Dimension& a, const Dimension& b) noexcept {
    return a.name == b.name && a.quantity == b.quantity;
  }
};

/// Immutable, sorted, duplicate-free vector of capacity dimensions.
/// Sorting is by dimension name, which makes serialization and digests canonical
/// regardless of the order the caller supplied.
class CapacityVector {
 public:
  CapacityVector() = default;

  /// Sorts and validates; duplicate dimensions are refused as conflicting input,
  /// not silently summed.
  static Result<CapacityVector> create(std::vector<Dimension> dims);
  static Result<CapacityVector> parse(std::string_view text);  // "cpu=4,mem=64"

  bool empty() const noexcept { return dims_.empty(); }
  const std::vector<Dimension>& dims() const noexcept { return dims_; }
  std::size_t size() const noexcept { return dims_.size(); }

  /// Returns 0 for an absent dimension; absence means zero, never "unknown".
  std::uint64_t quantity(const DimensionName& name) const noexcept;
  const Dimension* find(const DimensionName& name) const noexcept;

  /// True when every dimension of \p other is present with at least the same quantity.
  bool covers(const CapacityVector& other) const noexcept;
  bool operator==(const CapacityVector& other) const noexcept { return dims_ == other.dims_; }
  bool operator!=(const CapacityVector& other) const noexcept { return !(*this == other); }

  /// Checked per-dimension sum (union of dimensions).
  Result<CapacityVector> plus(const CapacityVector& other) const;
  /// Checked per-dimension difference; requires covers(other).
  Result<CapacityVector> minus(const CapacityVector& other) const;
  /// Smallest vector that covers both inputs.
  Result<CapacityVector> max_of(const CapacityVector& other) const;

  std::string to_string() const;
  Digest digest() const;

  void write_to(CanonicalWriter& writer, std::string_view name) const;
  static Result<CapacityVector> read_from(CanonicalReader& reader, std::string_view name);

 private:
  std::vector<Dimension> dims_;
};

}  // namespace xsr
