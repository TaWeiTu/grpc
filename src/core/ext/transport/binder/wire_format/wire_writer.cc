// Copyright 2021 gRPC authors.
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

#include <grpc/impl/codegen/port_platform.h>

#include "src/core/ext/transport/binder/wire_format/wire_writer.h"

#include <grpc/support/log.h>

#include <utility>

#define RETURN_IF_ERROR(expr)           \
  do {                                  \
    const absl::Status status = (expr); \
    if (!status.ok()) return status;    \
  } while (0)

namespace grpc_binder {
WireWriterImpl::WireWriterImpl(std::unique_ptr<Binder> binder)
    : binder_(std::move(binder)) {}

absl::Status WireWriterImpl::WriteInitialMetadata(const Transaction& tx,
                                                  WritableParcel* parcel) {
  if (tx.IsClient()) {
    // Only client sends method ref.
    RETURN_IF_ERROR(parcel->WriteString(tx.GetMethodRef()));
  }
  RETURN_IF_ERROR(parcel->WriteInt32(tx.GetPrefixMetadata().size()));
  for (const auto& kv : tx.GetPrefixMetadata()) {
    RETURN_IF_ERROR(parcel->WriteByteArrayWithLength(kv.ViewKey()));
    RETURN_IF_ERROR(parcel->WriteByteArrayWithLength(kv.ViewValue()));
  }
  return absl::OkStatus();
}

absl::Status WireWriterImpl::WriteTrailingMetadata(const Transaction& tx,
                                                   WritableParcel* parcel) {
  if (tx.IsServer()) {
    if (tx.GetFlags() & kFlagStatusDescription) {
      RETURN_IF_ERROR(parcel->WriteString(tx.GetStatusDesc()));
    }
    RETURN_IF_ERROR(parcel->WriteInt32(tx.GetSuffixMetadata().size()));
    for (const auto& kv : tx.GetSuffixMetadata()) {
      RETURN_IF_ERROR(parcel->WriteByteArrayWithLength(kv.ViewKey()));
      RETURN_IF_ERROR(parcel->WriteByteArrayWithLength(kv.ViewValue()));
    }
  } else {
    // client suffix currently is always empty according to the wireformat
    if (!tx.GetSuffixMetadata().empty()) {
      gpr_log(GPR_ERROR, "Got non-empty suffix metadata from client.");
    }
  }
  return absl::OkStatus();
}

const int64_t WireWriterImpl::kBlockSize = 16 * 1024;
const int64_t WireWriterImpl::kFlowControlWindowSize = 128 * 1024;

absl::Status WireWriterImpl::RpcCall(Transaction tx) {
  // TODO(mingcl): check tx_code <= last call id
  grpc_core::MutexLock lock(&mu_);
  GPR_ASSERT(tx.GetTxCode() >= kFirstCallId);
  int& seq = seq_num_[tx.GetTxCode()];
  // If there's no message data or the message data is completely empty.
  if ((tx.GetFlags() & kFlagMessageData) == 0 ||
      tx.GetMessageData()->count == 0) {
    // Fast path: send data in one transaction.
    RETURN_IF_ERROR(binder_->PrepareTransaction());
    WritableParcel* parcel = binder_->GetWritableParcel();
    RETURN_IF_ERROR(parcel->WriteInt32(tx.GetFlags()));
    RETURN_IF_ERROR(parcel->WriteInt32(seq++));
    if (tx.GetFlags() & kFlagPrefix) {
      RETURN_IF_ERROR(WriteInitialMetadata(tx, parcel));
    }
    if (tx.GetFlags() & kFlagMessageData) {
      // Empty message. Only send 0 as its length.
      RETURN_IF_ERROR(parcel->WriteInt32(0));
    }
    if (tx.GetFlags() & kFlagSuffix) {
      RETURN_IF_ERROR(WriteTrailingMetadata(tx, parcel));
    }
    // FIXME(waynetu): Construct BinderTransportTxCode from an arbitrary integer
    // is an undefined behavior.
    return binder_->Transact(BinderTransportTxCode(tx.GetTxCode()));
  }
  // Slow path: we have non-empty message data to be sent.
  int original_flags = tx.GetFlags();
  GPR_ASSERT(original_flags & kFlagMessageData);
  grpc_slice_buffer* buffer = tx.GetMessageData();
  GPR_ASSERT(buffer->count > 0);
  bool is_first = true;
  while (buffer->count > 0) {
    grpc_slice slice = grpc_slice_buffer_take_first(buffer);
    absl::string_view data = grpc_core::StringViewFromSlice(slice);
    size_t ptr = 0, len = data.size();
    // The condition on the right is a small hack for empty messages. We will
    // increment ptr by at least one so for empty messages we will sent exactly
    // one transaction.
    while (ptr < len || (ptr == 0 && len == 0)) {
      while (num_outgoing_bytes_ >=
             num_acknowledged_bytes_ + kFlowControlWindowSize) {
        cv_.Wait(&mu_);
      }
      RETURN_IF_ERROR(binder_->PrepareTransaction());
      WritableParcel* parcel = binder_->GetWritableParcel();
      int flags = kFlagMessageData;
      if (is_first) {
        // First transaction. Include initial metadata if there's any.
        if (original_flags & kFlagPrefix) {
          flags |= kFlagPrefix;
        }
        is_first = false;
      }
      if (buffer->count > 0 || ptr + kBlockSize < len) {
        // We can't complete in this transaction.
        flags |= kFlagMessageDataIsPartial;
      } else {
        // Last transaction. Include trailing metadata if there's any.
        if (original_flags & kFlagSuffix) {
          flags |= kFlagSuffix;
        }
      }
      RETURN_IF_ERROR(parcel->WriteInt32(flags));
      RETURN_IF_ERROR(parcel->WriteInt32(seq++));
      if (flags & kFlagPrefix) {
        RETURN_IF_ERROR(WriteInitialMetadata(tx, parcel));
      }
      size_t size = std::min(static_cast<size_t>(kBlockSize), len - ptr);
      RETURN_IF_ERROR(parcel->WriteByteArrayWithLength(data.substr(ptr, size)));
      if (flags & kFlagSuffix) {
        RETURN_IF_ERROR(WriteTrailingMetadata(tx, parcel));
      }
      num_outgoing_bytes_ += parcel->GetDataSize();
      RETURN_IF_ERROR(binder_->Transact(BinderTransportTxCode(tx.GetTxCode())));
      ptr += std::max<size_t>(1, size);
    }
    grpc_slice_unref_internal(slice);
  }
  return absl::OkStatus();
}

absl::Status WireWriterImpl::SendAck(int64_t num_bytes) {
  grpc_core::MutexLock lock(&mu_);
  RETURN_IF_ERROR(binder_->PrepareTransaction());
  WritableParcel* parcel = binder_->GetWritableParcel();
  RETURN_IF_ERROR(parcel->WriteInt64(num_bytes));
  return binder_->Transact(BinderTransportTxCode::ACKNOWLEDGE_BYTES);
}

void WireWriterImpl::RecvAck(int64_t num_bytes) {
  grpc_core::MutexLock lock(&mu_);
  num_acknowledged_bytes_ = std::max(num_acknowledged_bytes_, num_bytes);
  cv_.Signal();
}

}  // namespace grpc_binder
