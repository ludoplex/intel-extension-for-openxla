/* Copyright (c) 2023 Intel Corporation

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

#include "xla/stream_executor/sycl/sycl_gpu_runtime.h"

#include <cassert>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "tsl/platform/status.h"
#include "tsl/util/env_var.h"

namespace {

// SYCL_TILE_AS_DEVICE
//   True (default behaviour): Tile as an individual device in device list
//   False: Only root device as an individual device in device list
inline bool TileAsDevice() {
  bool tile_as_device;
  TF_CHECK_OK(
      tsl::ReadBoolFromEnvVar("SYCL_TILE_AS_DEVICE", true, &tile_as_device));
  return tile_as_device;
}

inline bool RunOnLevelZero() {
  char* sycl_device_filter = getenv("SYCL_DEVICE_FILTER");
  // Current default backend platform is Level-Zero
  if (sycl_device_filter == nullptr) return true;
  auto filter_device = std::string(sycl_device_filter);
  std::transform(filter_device.begin(), filter_device.end(),
                 filter_device.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return filter_device.find("level_zero") != std::string::npos;
}

bool hasDevice() {
  int count = 0;
  SYCLError_t error = SYCLGetDeviceCount(&count);
  if (error != SYCL_SUCCESS) {
    LOG(ERROR) << "Error to get the device count because " << ToString(error);
    return false;
  }

  if (count == 0) {
    LOG(ERROR) << "Error. The device count is 0, "
               << ToString(SYCL_ERROR_NO_DEVICE);
    return false;
  }

  return true;
}

bool isValidDevice(int ordinal) {
  int count = 0;
  SYCLGetDeviceCount(&count);

  if (ordinal > count) {
    return false;
  }

  return true;
}

class DevicePool {
 public:
  DevicePool() : current_ordinal_(0) {}

  static sycl::context& getDeviceContext() {
    static sycl::context context(DevicePool::GetDevicesPool());
    return context;
  }

  static SYCLError_t getDeviceCount(int* count) {
    *count = DevicePool::GetDevicesPool().size();
    return SYCL_SUCCESS;
  }

  static SYCLError_t getDevice(sycl::device** device, int device_ordinal) {
    // absl::ReaderMutexLock lock(&mu_);
    if (device_ordinal >= DevicePool::GetDevicesPool().size()) {
      return SYCL_ERROR_INVALID_DEVICE;
    } else {
      *device = &DevicePool::GetDevicesPool()[device_ordinal];
      return SYCL_SUCCESS;
    }
  }

  SYCLError_t getint(const sycl::device& device, int* device_ordinal) {
    const auto& devices = DevicePool::GetDevicesPool();
    auto it = std::find(devices.begin(), devices.end(), device);
    if (it != devices.end()) {
      *device_ordinal = it - devices.begin();
      return SYCL_SUCCESS;
    } else {
      return SYCL_ERROR_INVALID_DEVICE;
    }
  }

  static DevicePool* GetInstance();

 private:
  static std::vector<sycl::device>& GetDevicesPool() {
    static std::once_flag init_device_flag;
    static std::vector<sycl::device> devices;

    std::call_once(init_device_flag, []() {
      std::vector<sycl::device> root_devices;
      // Get root device list from platform list.
      auto platform_list = sycl::platform::get_platforms();
      for (const auto& platform : platform_list) {
        auto platform_name = platform.get_info<sycl::info::platform::name>();
        bool is_level_zero =
            platform_name.find("Level-Zero") != std::string::npos;
        // Add device in these two scenarios:
        // true == true means need Level-Zero and the backend platform is
        // Level-Zero.
        // false == false mean need OCL and the backend platform is OCL.
        if (is_level_zero == RunOnLevelZero()) {
          LOG(INFO) << "Selected platform: " << platform_name;
          auto device_list = platform.get_devices();
          for (const auto& device : device_list) {
            if (device.is_gpu()) {
              root_devices.push_back(device);
            }
          }
        }
      }

      if (TileAsDevice()) {
        // If SYCL_TILE_AS_DEVICE is true.
        // Create sub devices from root devices:
        //   If succ, add sub devices into devices list
        //   If fail, add root devices into devices list
        constexpr auto partition_by_affinity =
            sycl::info::partition_property::partition_by_affinity_domain;
        constexpr auto next_partitionable =
            sycl::info::partition_affinity_domain::next_partitionable;
        for (const auto& root_device : root_devices) {
          std::vector<sycl::device> sub_devices;
          auto max_sub_devices =
              root_device
                  .get_info<sycl::info::device::partition_max_sub_devices>();
          if (max_sub_devices == 0) {
            LOG(INFO) << "number of sub-devices is zero, expose root "
                         "device.";
            devices.push_back(root_device);
          } else {
            sub_devices = root_device.create_sub_devices<partition_by_affinity>(
                next_partitionable);
            devices.insert(devices.end(), sub_devices.begin(),
                           sub_devices.end());
          }
        }
      } else {
        // If SYCL_TILE_AS_DEVICE is false.
        // Only set root device as device list.
        devices = std::move(root_devices);
      }

      size_t num_device = devices.size();

      if (num_device <= 0) {
        LOG(ERROR) << "Can not found any devices.";
      }
      assert((num_device > 0));
    });

    return devices;
  }

  int current_ordinal_;
  static absl::Mutex mu_;
  static DevicePool* instance_;
};

/* static */ absl::Mutex DevicePool::mu_{absl::kConstInit};
/* static */ DevicePool* DevicePool::instance_{nullptr};

DevicePool* DevicePool::GetInstance() {
  absl::MutexLock lock(&mu_);
  if (instance_ == nullptr) {
    instance_ = new DevicePool();
  }

  return instance_;
}
}  // namespace

/******************* SYCL context management**************************/
static sycl::async_handler SYCLAsyncHandler = [](sycl::exception_list eL) {
  for (auto& e : eL) {
    try {
      std::rethrow_exception(e);
    } catch (sycl::exception& e) {
      LOG(ERROR) << "DPC++ Exception: " << e.what() << ", file = " << __FILE__
                 << ", line = " << __LINE__ << ".";
    }
  }
};

class StreamPool {
 public:
  static SYCLError_t getDefaultStream(sycl::device* device_handle,
                                      sycl::queue** stream_p) {
    *stream_p = StreamPool::GetStreamsPool(device_handle)[0].get();
    return SYCL_SUCCESS;
  }

  static SYCLError_t createStream(sycl::device* device_handle,
                                  sycl::queue** stream_p) {
    if (IsMultipleStreamEnabled()) {
      sycl::property_list propList{sycl::property::queue::in_order()};
      StreamPool::GetStreamsPool(device_handle)
          .push_back(std::make_shared<sycl::queue>(
              DevicePool::getDeviceContext(), *device_handle, SYCLAsyncHandler,
              propList));
    }
    *stream_p = StreamPool::GetStreamsPool(device_handle).back().get();
    return SYCL_SUCCESS;
  }

  static SYCLError_t syncContext(sycl::device* device_handle) {
    for (auto stream : StreamPool::GetStreamsPool(device_handle)) {
      stream->wait();
    }
    return SYCL_SUCCESS;
  }

  static SYCLError_t destroyStream(sycl::device* device_handle,
                                   sycl::queue* stream_handle) {
    if (stream_handle == nullptr) return SYCL_ERROR_INVALID_STREAM;
    auto stream_pool = StreamPool::GetStreamsPool(device_handle);
    for (int i = 0; i < stream_pool.size(); i++) {
      if (stream_pool[i].get() == stream_handle) {
        stream_pool.erase(stream_pool.begin() + i);
        return SYCL_SUCCESS;
      }
    }
    return SYCL_ERROR_INVALID_STREAM;
  }

  static SYCLError_t getStreams(sycl::device* device_handle,
                                std::vector<sycl::queue*>* streams) {
    auto stream_pool = StreamPool::GetStreamsPool(device_handle);
    for (int i = 0; i < stream_pool.size(); i++) {
      streams->push_back(stream_pool[i].get());
    }
    return SYCL_SUCCESS;
  }

 private:
  static std::vector<std::shared_ptr<sycl::queue>>& GetStreamsPool(
      sycl::device* device_handle) {
    static std::unordered_map<sycl::device*,
                              std::vector<std::shared_ptr<sycl::queue>>>
        stream_pool_map;
    auto iter = stream_pool_map.find(device_handle);
    if (iter != stream_pool_map.end()) return iter->second;
    sycl::property_list propList{sycl::property::queue::in_order()};
    std::vector<std::shared_ptr<sycl::queue>> stream_pool = {
        std::make_shared<sycl::queue>(DevicePool::getDeviceContext(),
                                      *device_handle, SYCLAsyncHandler,
                                      propList)};
    stream_pool_map.insert(std::make_pair(device_handle, stream_pool));
    return stream_pool_map[device_handle];
  }
};

SYCLError_t SYCLGetContext(sycl::context** context) {
  *context = &DevicePool::getDeviceContext();
}

SYCLError_t SYCLGetDeviceCount(int* count) {
  return DevicePool::getDeviceCount(count);
}

SYCLError_t SYCLGetDevice(sycl::device** device, int device_ordinal) {
  return DevicePool::getDevice(device, device_ordinal);
}

SYCLError_t SYCLCreateStream(sycl::device* device_handle,
                             sycl::queue** stream_p) {
  return StreamPool::createStream(device_handle, stream_p);
}

SYCLError_t SYCLDestroyStream(sycl::device* device_handle,
                              sycl::queue* stream_handle) {
  return StreamPool::destroyStream(device_handle, stream_handle);
}

SYCLError_t SYCLCtxSynchronize(sycl::device* device_handle) {
  return StreamPool::syncContext(device_handle);
}

/************************* SYCL memory management
 * ***************************/

static void memcpyHostToDevice(void* dstDevice, const void* srcHost,
                               size_t ByteCount, bool async,
                               sycl::queue* stream) {
  if (ByteCount == 0) return;

  auto event = stream->memcpy(dstDevice, srcHost, ByteCount);
  if (!async) {
    event.wait();
  }
}

static void memcpyDeviceToHost(void* dstHost, const void* srcDevice,
                               size_t ByteCount, bool async,
                               sycl::queue* stream) {
  if (ByteCount == 0) return;

  auto event = stream->memcpy(dstHost, srcDevice, ByteCount);

  if (!async) {
    event.wait();
  }
}

static void memcpyDeviceToDevice(void* dstDevice, const void* srcDevice,
                                 size_t ByteCount, bool async,
                                 sycl::queue* stream) {
  if (ByteCount == 0) return;

  auto event = stream->memcpy(dstDevice, srcDevice, ByteCount);

  if (!async) {
    event.wait();
  }
}

static void memsetDeviceD8(void* dstDevice, unsigned char value, size_t n,
                           bool async, sycl::queue* stream) {
  if (n == 0) return;

  auto event = stream->memset(dstDevice, value, n * sizeof(uint8_t));
  if (!async) {
    event.wait();
  }
}

static void memsetDeviceD32(void* dstDevice, int value, size_t n, bool async,
                            sycl::queue* stream) {
  if (n == 0) return;

  auto event = stream->fill(dstDevice, value, n);

  if (!async) {
    event.wait();
  }
}

SYCLError_t SYCLMemcpyDtoH(void* dstHost, const void* srcDevice,
                           size_t ByteCount, sycl::device* device) {
  sycl::queue* stream;
  auto res = StreamPool::getDefaultStream(device, &stream);
  memcpyDeviceToHost(dstHost, srcDevice, ByteCount, false, stream);
  return res;
}

SYCLError_t SYCLMemcpyHtoD(void* dstDevice, const void* srcHost,
                           size_t ByteCount, sycl::device* device) {
  sycl::queue* stream;
  auto res = StreamPool::getDefaultStream(device, &stream);
  memcpyHostToDevice(dstDevice, srcHost, ByteCount, false, stream);
  return res;
}

SYCLError_t SYCLMemcpyDtoD(void* dstDevice, const void* srcDevice,
                           size_t ByteCount, sycl::device* device) {
  sycl::queue* stream;
  auto res = StreamPool::getDefaultStream(device, &stream);
  memcpyDeviceToDevice(dstDevice, srcDevice, ByteCount, false, stream);
  return res;
}

SYCLError_t SYCLMemcpyDtoHAsync(void* dstHost, const void* srcDevice,
                                size_t ByteCount, sycl::queue* stream) {
  sycl::usm::alloc DstAllocType =
      get_pointer_type(dstHost, stream->get_context());
  memcpyDeviceToHost(dstHost, srcDevice, ByteCount,
                     DstAllocType == sycl::usm::alloc::host, stream);
  return SYCL_SUCCESS;
}

SYCLError_t SYCLMemcpyHtoDAsync(void* dstDevice, const void* srcHost,
                                size_t ByteCount, sycl::queue* stream) {
  sycl::usm::alloc SrcAllocType =
      get_pointer_type(srcHost, stream->get_context());
  memcpyHostToDevice(dstDevice, srcHost, ByteCount,
                     SrcAllocType == sycl::usm::alloc::host, stream);
  return SYCL_SUCCESS;
}

SYCLError_t SYCLMemcpyDtoDAsync(void* dstDevice, const void* srcDevice,
                                size_t ByteCount, sycl::queue* stream) {
  memcpyDeviceToDevice(dstDevice, srcDevice, ByteCount, true, stream);
  return SYCL_SUCCESS;
}

SYCLError_t SYCLMemsetD8(void* dstDevice, unsigned char uc, size_t N,
                         sycl::device* device) {
  sycl::queue* stream;
  auto res = StreamPool::getDefaultStream(device, &stream);
  memsetDeviceD8(dstDevice, uc, N, false, stream);
  return res;
}

SYCLError_t SYCLMemsetD8Async(void* dstDevice, unsigned char uc, size_t N,
                              sycl::queue* stream) {
  memsetDeviceD8(dstDevice, uc, N, true, stream);
  return SYCL_SUCCESS;
}

SYCLError_t SYCLMemsetD32(void* dstDevice, unsigned int ui, size_t N,
                          sycl::device* device) {
  sycl::queue* stream;
  auto res = StreamPool::getDefaultStream(device, &stream);
  memsetDeviceD32(dstDevice, ui, N, false, stream);
  return res;
}

SYCLError_t SYCLMemsetD32Async(void* dstDevice, unsigned int ui, size_t N,
                               sycl::queue* stream) {
  memsetDeviceD32(dstDevice, ui, N, true, stream);
  return SYCL_SUCCESS;
}

void* SYCLMalloc(sycl::device* device, size_t ByteCount) {
  sycl::queue* stream;
  StreamPool::getDefaultStream(device, &stream);

  // Always use default 0 stream to allocate mem
  auto ptr = aligned_alloc_device(/*alignment=*/64, ByteCount, *stream);
  return static_cast<void*>(ptr);
}

void* SYCLMallocHost(sycl::device* device, size_t ByteCount) {
  sycl::queue* stream;
  StreamPool::getDefaultStream(device, &stream);

  // Always use default 0 stream to allocate mem
  auto ptr = aligned_alloc_host(/*alignment=*/64, ByteCount, *stream);
  return static_cast<void*>(ptr);
}

void* SYCLMallocShared(sycl::device* device, size_t ByteCount) {
  sycl::queue* stream;
  StreamPool::getDefaultStream(device, &stream);

  // Always use default 0 stream to allocate mem
  auto ptr = aligned_alloc_shared(/*alignment=*/64, ByteCount, *stream);
  return static_cast<void*>(ptr);
}

void SYCLFree(sycl::device* device, void* ptr) {
  sycl::queue* stream;
  StreamPool::getDefaultStream(device, &stream);

  // Always use default 0 stream to free mem
  sycl::free(ptr, *stream);
}

const char* ToString(SYCLError_t error) {
  switch (error) {
    case SYCL_SUCCESS:
      return "DPC++ succeed.";
    case SYCL_ERROR_NO_DEVICE:
      return "DPC++ did not find the device.";
    case SYCL_ERROR_INVALID_DEVICE:
      return "DPC++ got invalid device id.";
    case SYCL_ERROR_INVALID_POINTER:
      return "DPC++ got invalid pointer.";
    case SYCL_ERROR_INVALID_STREAM:
      return "DPC++ got invalid stream.";
    case SYCL_ERROR_DESTROY_DEFAULT_STREAM:
      return "DPC++ cannot destroy default stream.";
    default:
      return "DPC++ got invalid error code.";
  }
}  // namespace
