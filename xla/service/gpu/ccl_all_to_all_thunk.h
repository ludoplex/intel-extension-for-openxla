/* Copyright (c) 2023 Intel Corporation

Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#ifndef XLA_SERVICE_GPU_CCL_ALL_TO_ALL_THUNK_H_
#define XLA_SERVICE_GPU_CCL_ALL_TO_ALL_THUNK_H_

#include <vector>

#include "xla/mlir_hlo/lhlo/IR/lhlo_ops.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/gpu/ccl_collective_thunk.h"
#include "xla/xla_data.pb.h"

namespace xla {
namespace gpu {

struct CclAllToAllConfig {
  CclCollectiveConfig config;
  bool has_split_dimension;
};

// Base class for thunks that performs a NCCL-based All-to-All among CUDA
// GPU-based replicas.
class CclAllToAllThunkBase : public CclCollectiveThunk {
 public:
  CclAllToAllThunkBase(Kind kind, ThunkInfo thunk_info,
                       CclAllToAllConfig config, std::vector<Buffer> buffers);

 protected:
  Status RunAllToAll(const ExecuteParams& params, se::Stream& stream,
                     ncclComm_t comm);
  const CclCollectiveConfig& config() const override { return config_.config; }

 private:
  const CclAllToAllConfig config_;
  const std::vector<Buffer> buffers_;
};

class CclAllToAllThunk : public CclAllToAllThunkBase {
 public:
  CclAllToAllThunk(ThunkInfo thunk_info, mlir::lmhlo::AllToAllOp op,
                   std::vector<Buffer> buffers);

  // Returns whether the given instruction can be lowered to a nccl all-to-all
  // call.
  static bool CanImplement(mlir::lmhlo::AllToAllOp op);

  static const char* GetName() { return "AllToAll"; }
  static bool IsDegenerate(mlir::lmhlo::AllToAllOp op, int64_t replica_count,
                           int64_t partition_count);
  static CollectiveOpGroupMode GetGroupMode(mlir::lmhlo::AllToAllOp op);
  static constexpr bool IsAsync() { return false; }

 protected:
  Status RunCclCollective(const ExecuteParams& params,
                          ncclComm_t comm) override;
};

class CclAllToAllStartThunk : public CclAllToAllThunkBase {
 public:
  CclAllToAllStartThunk(ThunkInfo thunk_info,
                        mlir::lmhlo_gpu::AllToAllStartOp op,
                        std::vector<Buffer> buffers);

  // Returns whether the given instruction can be lowered to a nccl all-to-all
  // call.
  static bool CanImplement(mlir::lmhlo_gpu::AllToAllStartOp op);

  static const char* GetName() { return "AllToAllStart"; }
  static bool IsDegenerate(mlir::lmhlo_gpu::AllToAllStartOp op,
                           int64_t replica_count, int64_t partition_count);
  static CollectiveOpGroupMode GetGroupMode(
      mlir::lmhlo_gpu::AllToAllStartOp op);

  static constexpr bool IsAsync() { return true; }
  AsyncExecutor& async_executor() { return async_; }

 protected:
  Status RunCclCollective(const ExecuteParams& params,
                          ncclComm_t comm) override;

 private:
  AsyncExecutor async_;
};

class CclAllToAllDoneThunk : public CclCollectiveDoneThunk {
 public:
  CclAllToAllDoneThunk(ThunkInfo thunk_info,
                       CclCollectiveThunk::AsyncExecutor& async);
};

Status RunAllToAll(bool has_split_dimension,
                   std::vector<DeviceBufferPair>& buffers, se::Stream& stream,
                   ncclComm_t comm);

}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_CCL_ALL_TO_ALL_THUNK_H_