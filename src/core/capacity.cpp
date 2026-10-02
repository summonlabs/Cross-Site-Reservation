// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/capacity.hpp"

#include <algorithm>

#include "xsr/checked.hpp"

namespace xsr {

Result<CapacityVector> CapacityVector::create(std::vector<Dimension> dims) {
  if (dims.size() > limits::max_dimensions) {
    return make_error(ErrorCode::too_large, "capacity vector exceeds the dimension budget");
  }
  std::sort(dims.begin(), dims.end(),
            [](const Dimension& a, const Dimension& b) { return a.name < b.name; });
  for (std::size_t i = 1; i < dims.size(); ++i) {
    if (dims[i - 1].name == dims[i].name) {
      return make_error(ErrorCode::duplicate, "capacity vector repeats dimension '" + dims[i].name.str() + "'");
    }
  }
  CapacityVector result;
  result.dims_ = std::move(dims);
  return result;
}

Result<CapacityVector> CapacityVector::parse(std::string_view text) {
  if (text.empty()) return CapacityVector{};
  if (text.size() > limits::max_canonical_value_length) {
    return make_error(ErrorCode::too_large, "capacity text exceeds the allowed length");
  }
  std::vector<Dimension> dims;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const std::size_t comma = text.find(',', offset);
    const std::string_view token = comma == std::string_view::npos ? text.substr(offset)
                                                                   : text.substr(offset, comma - offset);
    const std::size_t equals = token.find('=');
    if (equals == std::string_view::npos) {
      return make_error(ErrorCode::invalid_argument, "capacity dimension must be name=quantity");
    }
    auto name = DimensionName::parse(token.substr(0, equals));
    if (!name.ok()) return name.error();
    auto quantity = parse_u64(token.substr(equals + 1));
    if (!quantity.ok()) {
      return make_error(quantity.error().code, "capacity dimension '" + std::string(token.substr(0, equals)) +
                                                   "': " + quantity.error().message);
    }
    dims.push_back(Dimension{std::move(name).value(), quantity.value()});
    if (comma == std::string_view::npos) break;
    offset = comma + 1;
  }
  return create(std::move(dims));
}

std::uint64_t CapacityVector::quantity(const DimensionName& name) const noexcept {
  const Dimension* dim = find(name);
  return dim == nullptr ? 0 : dim->quantity;
}

const Dimension* CapacityVector::find(const DimensionName& name) const noexcept {
  for (const Dimension& dim : dims_) {
    if (dim.name == name) return &dim;
  }
  return nullptr;
}

bool CapacityVector::covers(const CapacityVector& other) const noexcept {
  for (const Dimension& dim : other.dims_) {
    const Dimension* mine = find(dim.name);
    if (mine == nullptr || mine->quantity < dim.quantity) return false;
  }
  return true;
}

namespace {

Result<std::vector<Dimension>> merge_dims(const std::vector<Dimension>& a, const std::vector<Dimension>& b,
                                          bool take_max) {
  std::vector<Dimension> out;
  out.reserve(a.size() + b.size());
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() || j < b.size()) {
    if (j >= b.size() || (i < a.size() && a[i].name < b[j].name)) {
      out.push_back(a[i++]);
    } else if (i >= a.size() || b[j].name < a[i].name) {
      out.push_back(b[j++]);
    } else {
      std::uint64_t quantity = 0;
      if (take_max) {
        quantity = std::max(a[i].quantity, b[j].quantity);
      } else {
        auto sum = add_checked(a[i].quantity, b[j].quantity);
        if (!sum.ok()) {
          return make_error(ErrorCode::overflow, "capacity sum overflowed for dimension '" + a[i].name.str() + "'");
        }
        quantity = sum.value();
      }
      out.push_back(Dimension{a[i].name, quantity});
      ++i;
      ++j;
    }
  }
  return out;
}

}  // namespace

Result<CapacityVector> CapacityVector::plus(const CapacityVector& other) const {
  auto merged = merge_dims(dims_, other.dims_, false);
  if (!merged.ok()) return merged.error();
  return create(std::move(merged).value());
}

Result<CapacityVector> CapacityVector::max_of(const CapacityVector& other) const {
  auto merged = merge_dims(dims_, other.dims_, true);
  if (!merged.ok()) return merged.error();
  return create(std::move(merged).value());
}

Result<CapacityVector> CapacityVector::minus(const CapacityVector& other) const {
  std::vector<Dimension> out;
  out.reserve(dims_.size());
  for (const Dimension& dim : dims_) {
    const std::uint64_t other_quantity = other.quantity(dim.name);
    auto diff = sub_checked(dim.quantity, other_quantity);
    if (!diff.ok()) {
      return make_error(ErrorCode::underflow, "capacity subtraction underflowed for dimension '" + dim.name.str() +
                                                  "'");
    }
    if (diff.value() != 0) {
      out.push_back(Dimension{dim.name, diff.value()});
    }
  }
  return create(std::move(out));
}

std::string CapacityVector::to_string() const {
  std::string out;
  for (const Dimension& dim : dims_) {
    if (!out.empty()) out.push_back(',');
    out.append(dim.name.str());
    out.push_back('=');
    out.append(std::to_string(dim.quantity));
  }
  return out;
}

Digest CapacityVector::digest() const { return Digest::of(to_string()); }

void CapacityVector::write_to(CanonicalWriter& writer, std::string_view name) const {
  writer.put(name, to_string());
}

Result<CapacityVector> CapacityVector::read_from(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return parse(text.value());
}

}  // namespace xsr
