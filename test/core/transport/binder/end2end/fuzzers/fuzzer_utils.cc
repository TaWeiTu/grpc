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

#include "test/core/transport/binder/end2end/fuzzers/fuzzer_utils.h"

namespace grpc_binder {
namespace fuzzing {

std::vector<std::thread>* g_thread_pool = nullptr;

absl::Status FuzzedReadableParcel::ReadInt32(int32_t* data) const {
  if (!is_setup_transport_ && data_provider_->ConsumeBool()) {
    return absl::InternalError("error");
  }
  *data = data_provider_->ConsumeIntegral<int32_t>();
  return absl::OkStatus();
}

absl::Status FuzzedReadableParcel::ReadInt64(int64_t* data) const {
  if (!is_setup_transport_ && data_provider_->ConsumeBool()) {
    return absl::InternalError("error");
  }
  *data = data_provider_->ConsumeIntegral<int64_t>();
  return absl::OkStatus();
}

absl::Status FuzzedReadableParcel::ReadBinder(
    std::unique_ptr<Binder>* binder) const {
  if (!is_setup_transport_ && data_provider_->ConsumeBool()) {
    return absl::InternalError("error");
  }
  *binder = absl::make_unique<FuzzedBinder>();
  return absl::OkStatus();
}

absl::Status FuzzedReadableParcel::ReadByteArray(std::string* data) const {
  if (!is_setup_transport_ && data_provider_->ConsumeBool()) {
    return absl::InternalError("error");
  }
  *data = data_provider_->ConsumeRandomLengthString(100);
  return absl::OkStatus();
}

absl::Status FuzzedReadableParcel::ReadString(char data[111]) const {
  if (!is_setup_transport_ && data_provider_->ConsumeBool()) {
    return absl::InternalError("error");
  }
  std::string s = data_provider_->ConsumeRandomLengthString(100);
  std::memcpy(data, s.data(), s.size());
  return absl::OkStatus();
}

void FuzzingLoop(const uint8_t* data, size_t size,
                 grpc_core::RefCountedPtr<WireReader> wire_reader_ref,
                 TransactionReceiver::OnTransactCb callback) {
  FuzzedDataProvider data_provider(data, size);
  std::unique_ptr<ReadableParcel> parcel =
      absl::make_unique<FuzzedReadableParcel>(&data_provider,
                                              /*is_setup_transport=*/true);
  callback(
      static_cast<transaction_code_t>(BinderTransportTxCode::SETUP_TRANSPORT),
      parcel.get())
      .IgnoreError();
  while (data_provider.remaining_bytes() > 0) {
    gpr_log(GPR_INFO, "Fuzzing");
    bool streaming_call = data_provider.ConsumeBool();
    transaction_code_t tx_code =
        streaming_call
            ? data_provider.ConsumeIntegralInRange<transaction_code_t>(
                  0, static_cast<transaction_code_t>(
                         BinderTransportTxCode::PING_RESPONSE))
            : data_provider.ConsumeIntegralInRange<transaction_code_t>(
                  0, LAST_CALL_TRANSACTION);
    std::unique_ptr<ReadableParcel> parcel =
        absl::make_unique<FuzzedReadableParcel>(&data_provider,
                                                /*is_setup_transport=*/false);
    callback(tx_code, parcel.get()).IgnoreError();
  }
  wire_reader_ref = nullptr;
}

FuzzedTransactionReceiver::FuzzedTransactionReceiver(
    const uint8_t* data, size_t size,
    grpc_core::RefCountedPtr<WireReader> wire_reader_ref,
    TransactionReceiver::OnTransactCb cb) {
  gpr_log(GPR_INFO, "Construct FuzzedTransactionReceiver");
  g_thread_pool->emplace_back(FuzzingLoop, data, size, wire_reader_ref, cb);
}

std::unique_ptr<TransactionReceiver> FuzzedBinder::ConstructTxReceiver(
    grpc_core::RefCountedPtr<WireReader> wire_reader_ref,
    TransactionReceiver::OnTransactCb cb) const {
  return absl::make_unique<FuzzedTransactionReceiver>(data_, size_,
                                                      wire_reader_ref, cb);
}

}  // namespace fuzzing
}  // namespace grpc_binder
