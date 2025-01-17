/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_PYTHON_IFRT_SHARDING_H_
#define XLA_PYTHON_IFRT_SHARDING_H_

#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/check.h"
#include "llvm/Support/ExtensibleRTTI.h"
#include "xla/python/ifrt/device.h"
#include "xla/python/ifrt/index_domain.h"
#include "xla/python/ifrt/ir/sharding_param.h"
#include "xla/python/ifrt/memory.h"
#include "xla/python/ifrt/serdes.h"
#include "xla/python/ifrt/shape.h"
#include "xla/statusor.h"

namespace xla {
namespace ifrt {

// TODO(hyeontaek): Unify sharding types with jax::Sharding.

// Abstract sharding type.
//
// TODO(hyeontaek): There is an indication that we may prefer to split logical
// partitioning and device assignment into two separate data structures. It is
// common that an operation preserves the logical partitioning and only updates
// devices (e.g., "copy to devices" and portable execution). This fine-grained
// sharding design may help reduce overhead around these operations.
class Sharding : public llvm::RTTIExtends<Sharding, Serializable> {
 public:
  // All devices in this sharding. Devices may appear more than once.
  const DeviceList& devices() const { return devices_; }

  // Returns the memory kind for all shards in this sharding.
  MemoryKind memory_kind() const { return memory_kind_; }

  // Breaks a shape up into per-device shapes and shardings. See
  // Array::DisassembleIntoSingleDeviceArrays(). It may return an error if
  // disassembly is unsupported.
  virtual StatusOr<
      std::vector<std::pair<Shape, std::shared_ptr<const Sharding>>>>
  Disassemble(const Shape& shape) const = 0;

  // Variant of `Disassemble` that takes a dynamic shape.
  virtual StatusOr<
      std::vector<std::pair<DynamicShape, std::shared_ptr<const Sharding>>>>
  Disassemble(const DynamicShape& dynamic_shape) const = 0;

  // Maps each shard to an `IndexDomain` over `shape`. The result is a list of
  // `index_domain_i` such that `array[index_domain_i] = disassembled_array_i`.
  // Note that multiple shards may map onto equal `IndexDomain`. For instance, a
  // fully replicated sharding would return a vector of `[IndexDomain(shape)] *
  // devices().size()`.
  virtual StatusOr<std::vector<IndexDomain>> IndexDomains(
      const Shape& shape) const = 0;

  virtual std::string DebugString() const = 0;

  static char ID;  // NOLINT

 protected:
  Sharding(DeviceList devices, MemoryKind memory_kind)
      : devices_(devices), memory_kind_(memory_kind) {}

  DeviceList devices_;
  MemoryKind memory_kind_;
};

std::ostream& operator<<(std::ostream& os, const Sharding& sharding);

// Single-device sharding.
//
// TODO(hyeontaek): `SingleDeviceSharding` tends to be created or consumed in a
// large quantity. It may be useful for performance optimization to special-case
// this sharding type rather than expressing it as a general `Sharding`.
class SingleDeviceSharding final
    : public llvm::RTTIExtends<SingleDeviceSharding, Sharding> {
 public:
  // Creates a single-device sharding.
  static std::unique_ptr<SingleDeviceSharding> Create(Device* device,
                                                      MemoryKind memory_kind);

  // Sharding implementation.

  ~SingleDeviceSharding() override = default;

  StatusOr<std::vector<std::pair<Shape, std::shared_ptr<const Sharding>>>>
  Disassemble(const Shape& shape) const override;

  StatusOr<
      std::vector<std::pair<DynamicShape, std::shared_ptr<const Sharding>>>>
  Disassemble(const DynamicShape& dynamic_shape) const override;

  StatusOr<std::vector<IndexDomain>> IndexDomains(
      const Shape& shape) const override;

  std::string DebugString() const override;

  static char ID;  // NOLINT

 private:
  explicit SingleDeviceSharding(Device* device, MemoryKind memory_kind)
      : llvm::RTTIExtends<SingleDeviceSharding, Sharding>(DeviceList({device}),
                                                          memory_kind) {}
};

// Opaque sharding that does not define a fixed semantics for conversion between
// a logical shape and per-device shapes, and device placements.
class OpaqueSharding : public llvm::RTTIExtends<OpaqueSharding, Sharding> {
 public:
  // Creates an opaque sharding. `Disassemble()` will fail.
  static std::unique_ptr<OpaqueSharding> Create(DeviceList devices,
                                                MemoryKind memory_kind);

  // Sharding implementation.

  ~OpaqueSharding() override = default;

  StatusOr<std::vector<std::pair<Shape, std::shared_ptr<const Sharding>>>>
  Disassemble(const Shape& shape) const override;

  StatusOr<
      std::vector<std::pair<DynamicShape, std::shared_ptr<const Sharding>>>>
  Disassemble(const DynamicShape& dynamic_shape) const override;

  StatusOr<std::vector<IndexDomain>> IndexDomains(
      const Shape& shape) const override;

  std::string DebugString() const override;

  static char ID;  // NOLINT

 private:
  explicit OpaqueSharding(DeviceList devices, MemoryKind memory_kind);
};

// Opaque sharding that does not define a fixed semantics for conversion between
// a logical shape and shard shapes, and device placements. It can disassemble a
// certain shape into shard shapes that may not be identical. It is advised to
// use `ConcreteEvenSharding` if all shard shapes are identical.
class ConcreteSharding : public llvm::RTTIExtends<ConcreteSharding, Sharding> {
 public:
  // Creates a concrete sharding that may contain non-identical shard shapes.
  // REQUIRES: `devices`.size() == `shard_shapes`.size()
  static std::unique_ptr<ConcreteSharding> Create(
      DeviceList devices, MemoryKind memory_kind, Shape shape,
      std::vector<Shape> shard_shapes);

  // Creates a concrete sharding that may contain non-identical shard dynamic
  // shapes.
  // REQUIRES: `devices`.size() == `shard_dynamic_shapes`.size()
  static std::unique_ptr<ConcreteSharding> Create(
      DeviceList devices, MemoryKind memory_kind, DynamicShape dynamic_shape,
      std::vector<DynamicShape> shard_dynamic_shapes);

  bool has_dynamic_shape() const {
    DCHECK(this);
    return std::holds_alternative<DynamicShape>(shape_) &&
           std::holds_alternative<std::vector<DynamicShape>>(shard_shapes_);
  }

  bool has_static_shape() const {
    DCHECK(this);
    return std::holds_alternative<Shape>(shape_) &&
           std::holds_alternative<std::vector<Shape>>(shard_shapes_);
  }

  const Shape& shape() const {
    DCHECK(has_static_shape());
    return std::get<Shape>(shape_);
  }

  const DynamicShape& dynamic_shape() const {
    DCHECK(has_dynamic_shape());
    return std::get<DynamicShape>(shape_);
  }

  const std::vector<Shape>& shard_shapes() const {
    DCHECK(this);
    DCHECK(std::holds_alternative<std::vector<Shape>>(shard_shapes_));
    return std::get<std::vector<Shape>>(shard_shapes_);
  }

  const std::vector<DynamicShape>& shard_dynamic_shapes() const {
    DCHECK(this);
    DCHECK(std::holds_alternative<std::vector<DynamicShape>>(shard_shapes_));
    return std::get<std::vector<DynamicShape>>(shard_shapes_);
  }

  // Sharding implementation.

  ~ConcreteSharding() override = default;

  StatusOr<std::vector<std::pair<Shape, std::shared_ptr<const Sharding>>>>
  Disassemble(const Shape& shape) const override;
  StatusOr<
      std::vector<std::pair<DynamicShape, std::shared_ptr<const Sharding>>>>
  Disassemble(const DynamicShape& dynamic_shape) const override;

  StatusOr<std::vector<IndexDomain>> IndexDomains(
      const Shape& shape) const override;

  std::string DebugString() const override;

  static char ID;  // NOLINT

 private:
  ConcreteSharding(DeviceList devices, MemoryKind memory_kind, Shape shape,
                   std::vector<Shape> shard_shapes);

  ConcreteSharding(DeviceList devices, MemoryKind memory_kind,
                   DynamicShape dynamic_shape,
                   std::vector<DynamicShape> shard_dynamic_shapes);

  std::variant<Shape, DynamicShape> shape_;
  std::variant<std::vector<Shape>, std::vector<DynamicShape>> shard_shapes_;
};

// Opaque sharding that does not define a fixed semantics for conversion between
// a logical shape and shard shapes, and device placements. It can disassemble a
// certain shape into shard shapes that are identical.
class ConcreteEvenSharding
    : public llvm::RTTIExtends<ConcreteEvenSharding, Sharding> {
 public:
  // Creates a concrete even sharding.
  static std::unique_ptr<ConcreteEvenSharding> Create(DeviceList devices,
                                                      MemoryKind memory_kind,
                                                      Shape shape,
                                                      Shape shard_shape);

  Shape shape() const {
    DCHECK(this);
    return shape_;
  }
  const Shape& shard_shape() const {
    DCHECK(this);
    return shard_shape_;
  }

  // Sharding implementation.

  ~ConcreteEvenSharding() override = default;

  StatusOr<std::vector<std::pair<Shape, std::shared_ptr<const Sharding>>>>
  Disassemble(const Shape& shape) const override;
  StatusOr<
      std::vector<std::pair<DynamicShape, std::shared_ptr<const Sharding>>>>
  Disassemble(const DynamicShape& dynamic_shape) const override;

  StatusOr<std::vector<IndexDomain>> IndexDomains(
      const Shape& shape) const override;

  std::string DebugString() const override;

  static char ID;  // NOLINT

 private:
  ConcreteEvenSharding(DeviceList devices, MemoryKind memory_kind, Shape shape,
                       Shape shard_shape);

  Shape shape_;
  Shape shard_shape_;
};

// Sharding derived from an IR ShardingParam.
class ShardingParamSharding
    : public llvm::RTTIExtends<ShardingParamSharding, Sharding> {
 public:
  static StatusOr<std::unique_ptr<ShardingParamSharding>> Create(
      ShardingParam sharding_param, DeviceList devices, MemoryKind memory_kind);

  const ShardingParam& sharding_param() const { return sharding_param_; }

  StatusOr<std::vector<std::pair<Shape, std::shared_ptr<const Sharding>>>>
  Disassemble(const Shape& shape) const override;
  StatusOr<
      std::vector<std::pair<DynamicShape, std::shared_ptr<const Sharding>>>>
  Disassemble(const DynamicShape& dynamic_shape) const override;

  StatusOr<std::vector<IndexDomain>> IndexDomains(
      const Shape& shape) const override;

  std::string DebugString() const override;

  static char ID;  // NOLINT

 private:
  ShardingParamSharding(ShardingParam sharding_param, DeviceList devices,
                        MemoryKind memory_kind)
      : llvm::RTTIExtends<ShardingParamSharding, Sharding>(devices,
                                                           memory_kind),
        sharding_param_(sharding_param) {}

  ShardingParam sharding_param_;
};

}  // namespace ifrt
}  // namespace xla

#endif  // XLA_PYTHON_IFRT_SHARDING_H_
