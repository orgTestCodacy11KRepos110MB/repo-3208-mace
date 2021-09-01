// Copyright 2021 The MACE Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mace/rpcmems/qualcomm/qualcomm_rpcmem.h"

#include "mace/utils/logging.h"

namespace mace {

QualcommRpcmem::QualcommRpcmem() {
  mace_rpcmem_init(&rm);
  if (rm.flag != 0) {
    LOG(WARNING) << "QualcommRpcmem, rpcmem_init failed!";
    valid_detected_ = true;
    valid_ = false;
  }
}

QualcommRpcmem::~QualcommRpcmem() {
  mace_rpcmem_deinit(&rm);
}

void *QualcommRpcmem::New(int nbytes) {
  return mace_rpcmem_alloc(&rm, RPCMEM_HEAP_ID_SYSTEM, RPCMEM_FLAG_CACHED, nbytes);
}

void QualcommRpcmem::Delete(void *data) {
  mace_rpcmem_free(&rm, data);
}

int QualcommRpcmem::ToFd(void *data) {
  return mace_rpcmem_to_fd(&rm, data);
}

int QualcommRpcmem::SyncCacheStart(void *data) {
  return mace_rpcmem_sync_cache(&rm, data, RPCMEM_SYNC_START);
}

int QualcommRpcmem::SyncCacheEnd(void *data) {
  return mace_rpcmem_sync_cache(&rm, data, RPCMEM_SYNC_END);
}

RpcmemType QualcommRpcmem::GetRpcmemType() {
  return RpcmemType::ION_QUALCOMM;
}

}  // namespace mace
