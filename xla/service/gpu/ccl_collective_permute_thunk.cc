/* Copyright (c) 2023 Intel Corporation

Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/service/gpu/ccl_collective_permute_thunk.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/gpu/ccl_ops.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/stream_executor/sycl/sycl_stream.h"
#include "xla/translate/mhlo_to_hlo/attribute_exporter.h"
#include "xla/xla_data.pb.h"

namespace xla {
namespace gpu {
using mlir::lmhlo_gpu::CollectivePermuteStartOp;

namespace impl {

CollectiveOpGroupMode GetGroupMode(CollectivePermuteStartOp op) {
  return GetCollectiveOpGroupMode(op.getChannelId().has_value(), std::nullopt)
      .value();
}

CclCollectivePermuteConfig GetCclCollectivePermuteConfig(
    CollectivePermuteStartOp op, int64_t replica_count,
    int64_t partition_count) {
  CclCollectivePermuteConfig collective_permute_config;
  auto& config = collective_permute_config.config;

  config.operand_count = 1;
  const Shape shape = GetShape(op.getOperand());
  config.operand_element_type.push_back(shape.element_type());
  config.SetCollectiveOpKindAndID(op);
  config.group_mode = GetGroupMode(op);

  // With a collective permute, all execution instances together form one
  // replica group.
  const int64_t num_participants =
      config.group_mode == CollectiveOpGroupMode::kCrossReplica
          ? replica_count
          : partition_count;
  config.replica_groups.emplace_back();
  ReplicaGroup& replica_group = config.replica_groups.front();
  for (int i = 0; i < num_participants; ++i) {
    replica_group.add_replica_ids(i);
  }

  const std::vector<std::pair<int64_t, int64_t>> source_target_pairs =
      ConvertNx2Attribute(op.getSourceTargetPairs()).value();

  for (const std::pair<int64_t, int64_t>& source_target : source_target_pairs) {
    int64_t source = source_target.first;
    int64_t target = source_target.second;

    collective_permute_config.id_to_source_target.insert({target, {}})
        .first->second.source = source;
    collective_permute_config.id_to_source_target.insert({source, {}})
        .first->second.target = target;
  }

  return collective_permute_config;
}

// The collective permute is degenerate if all source-target pairs are identity,
// and all the IDs appear in the list.
bool IsDegenerate(CollectivePermuteStartOp op, int64_t replica_count,
                  int64_t partition_count) {
  const std::vector<std::pair<int64_t, int64_t>> source_target_pairs =
      ConvertNx2Attribute(op.getSourceTargetPairs()).value();
  // Each ID can appear only once as a source and as a target. So if all pairs
  // are identity, all IDs must appear in the list is the size == number of
  // replicas/partitions.
  const int64_t expected_size =
      op.getChannelId() ? partition_count : replica_count;
  return source_target_pairs.size() == expected_size &&
         absl::c_all_of(source_target_pairs,
                        [](const std::pair<int64_t, int64_t>& source_target) {
                          return source_target.first == source_target.second;
                        });
}

Status CheckImplementable(CollectivePermuteStartOp op) {
  TF_RETURN_IF_ERROR(CclCollectiveThunk::CheckImplementable());
  return IsValidOperand(op.getOperand(), Thunk::kNcclCollectivePermute);
}

}  // namespace impl

CclCollectivePermuteStartThunk::CclCollectivePermuteStartThunk(
    ThunkInfo thunk_info, CollectivePermuteStartOp op, int64_t replica_count,
    int64_t partition_count, const Buffer& buffer)
    : CclCollectiveThunk(Thunk::kNcclCollectivePermuteStart, thunk_info,
                         op.getIsSync()),
      config_(
          GetCclCollectivePermuteConfig(op, replica_count, partition_count)),
      buffer_(buffer) {}

/*static*/ CclCollectivePermuteConfig
CclCollectivePermuteStartThunk::GetCclCollectivePermuteConfig(
    CollectivePermuteStartOp op, int64_t replica_count,
    int64_t partition_count) {
  return impl::GetCclCollectivePermuteConfig(op, replica_count,
                                             partition_count);
}

/*static*/ Status CclCollectivePermuteStartThunk::CheckImplementable(
    CollectivePermuteStartOp op, int64_t replica_count,
    int64_t partition_count) {
  return AddOpDescription<CclCollectivePermuteStartThunk>(
      impl::CheckImplementable(op), op, replica_count, partition_count);
}

/*static*/ bool CclCollectivePermuteStartThunk::IsDegenerate(
    CollectivePermuteStartOp op, int64_t replica_count,
    int64_t partition_count) {
  return impl::IsDegenerate(op, replica_count, partition_count);
}

/*static*/ CollectiveOpGroupMode CclCollectivePermuteStartThunk::GetGroupMode(
    CollectivePermuteStartOp op) {
  return impl::GetGroupMode(op);
}
Status CclCollectivePermuteStartThunk::RunCclCollective(
    const ExecuteParams& params, se::Stream& stream, ncclComm_t comm) {
  TF_ASSIGN_OR_RETURN(
      std::vector<DeviceBufferPair> device_buffers,
      ConvertToDeviceBuffers(params, {buffer_},
                             config_.config.operand_element_type));
  TF_RET_CHECK(device_buffers.size() == 1) << "Expected one buffer pair.";

  TF_ASSIGN_OR_RETURN(const GlobalDeviceId global_device_id,
                      params.nccl_params.GetGlobalDeviceId());
  TF_ASSIGN_OR_RETURN(
      const DeviceAssignment::LogicalID current_logical_id,
      params.nccl_params.device_assn->LogicalIdForDevice(global_device_id));
  const int64_t current_id =
      config_.config.group_mode == CollectiveOpGroupMode::kCrossReplica
          ? current_logical_id.replica_id
          : current_logical_id.computation_id;
  std::string device_string = GetDeviceString(params.nccl_params);

  const CclCollectivePermuteConfig::SourceTargetMapEntry source_target =
      CclCollectivePermuteConfig::GetSourceTarget(config_.id_to_source_target,
                                                  current_id);

  return ::xla::gpu::RunCollectivePermute(source_target, device_buffers[0],
                                          stream, comm, device_string,
                                          current_id);
}

Status RunCollectivePermute(
    CclCollectivePermuteConfig::SourceTargetMapEntry source_target,
    DeviceBufferPair& buffer, se::Stream& stream, ncclComm_t comm,
    absl::string_view device_string, int64_t current_id) {
  // Determine the source and target IDs for this instance. The source ID is the
  // ID which will copy its data to this instance. The destination ID is the ID
  // to which this instance will copy its data. Either are optional.
  //
  // No source and no dest:
  //  - this instance does not actually participate, no one send it any data and
  //    it does not have to send any data as well. Since there is no dest,
  //    just memzero() the dest buffer as required by the collective permute
  //    semantics.
  //
  // No source, dest present:
  //  - This instance has to send data to 'dest' Issue an send of the input.
  //    Since there is no source, memzero the dest buffer.
  //
  // Source present, no destination:
  //  - This instance received data from the source, does not have to send data
  //    to anyone, Issue a receive.
  //
  // Source and dest both present:
  //   - Issue a send of the input to dest, receive for the output from the
  //     src.
  //
  //

  int device_ordinal = stream.parent()->device_ordinal();
  VLOG(3) << "Performing collective permute from device ordinal: "
          << device_ordinal;

  const std::optional<int64_t> source_id = source_target.source;
  const std::optional<int64_t> target_id = source_target.target;

  se::DeviceMemoryBase src_addr = buffer.source_buffer;
  se::DeviceMemoryBase dest_addr = buffer.destination_buffer;

  VLOG(3) << absl::StreamFormat("%s : id = %d, source_id = %d, target_id = %d",
                                device_string, current_id,
                                source_id.value_or(-1), target_id.value_or(-1));

  PrimitiveType element_type = buffer.element_type;
  int element_count = buffer.element_count *
                      (primitive_util::IsComplexType(element_type) ? 2 : 1);

  se::gpu::GpuStreamHandle gpu_stream = se::gpu::AsGpuStreamValue(&stream);

  sycl_collective_permute(src_addr.opaque(), dest_addr.opaque(), element_count,
                          element_type, source_id, target_id, gpu_stream, comm);

  if (!source_id) {
    // If there is no source peer, i.e. no one send us any data, zero out dest
    // buffer.
    VLOG(3) << absl::StreamFormat("%s : collective-Permute: Issuing MemZero",
                                  device_string);
    stream.ThenMemZero(&dest_addr, dest_addr.size());
  }
  return OkStatus();
}

}  // namespace gpu
}  // namespace xla
