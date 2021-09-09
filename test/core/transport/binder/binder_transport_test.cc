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

// Unit-tests for grpc_binder_transport
//
// Verify that a calls to the perform_stream_op of grpc_binder_transport
// transform into the correct sequence of binder transactions.
#include "src/core/ext/transport/binder/transport/binder_transport.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "absl/memory/memory.h"
#include "absl/strings/match.h"

#include <grpc/grpc.h>

#include "src/core/ext/transport/binder/transport/binder_stream.h"
#include "test/core/transport/binder/mock_objects.h"
#include "test/core/util/test_config.h"

namespace grpc_binder {
namespace {

using ::testing::Expectation;
using ::testing::NiceMock;
using ::testing::Return;

class BinderTransportTest : public ::testing::Test {
 public:
  BinderTransportTest()
      : arena_(grpc_core::Arena::Create(/* initial_size = */ 1)),
        transport_(grpc_create_binder_transport_client(
            absl::make_unique<NiceMock<MockBinder>>())) {
    auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
    gbt->wire_writer = absl::make_unique<MockWireWriter>();
    GRPC_STREAM_REF_INIT(&ref_, 1, nullptr, nullptr, "phony ref");
  }

  ~BinderTransportTest() override {
    auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
    delete gbt;
    for (grpc_binder_stream* gbs : stream_buffer_) {
      gpr_free(gbs);
    }
    arena_->Destroy();
  }

  void PerformStreamOp(grpc_binder_stream* gbs,
                       grpc_transport_stream_op_batch* op) {
    grpc_transport_perform_stream_op(transport_,
                                     reinterpret_cast<grpc_stream*>(gbs), op);
  }

  grpc_binder_transport* GetBinderTransport() {
    return reinterpret_cast<grpc_binder_transport*>(transport_);
  }

  grpc_binder_stream* InitNewBinderStream() {
    grpc_binder_stream* gbs = static_cast<grpc_binder_stream*>(
        gpr_malloc(grpc_transport_stream_size(transport_)));
    grpc_transport_init_stream(transport_, reinterpret_cast<grpc_stream*>(gbs),
                               &ref_, nullptr, arena_);
    stream_buffer_.push_back(gbs);
    return gbs;
  }

  MockWireWriter& GetWireWriter() {
    return *reinterpret_cast<MockWireWriter*>(
        GetBinderTransport()->wire_writer.get());
  }

  static void SetUpTestSuite() { grpc_init(); }
  static void TearDownTestSuite() { grpc_shutdown(); }

 protected:
  grpc_core::Arena* arena_;
  grpc_transport* transport_;
  grpc_stream_refcount ref_;
  std::vector<grpc_binder_stream*> stream_buffer_;
};

void MockCallback(void* arg, grpc_error_handle error);

class MockGrpcClosure {
 public:
  MockGrpcClosure() {
    GRPC_CLOSURE_INIT(&closure_, MockCallback, this, nullptr);
  }

  grpc_closure* GetGrpcClosure() { return &closure_; }
  MOCK_METHOD(void, Callback, (grpc_error_handle), ());

 private:
  grpc_closure closure_;
};

void MockCallback(void* arg, grpc_error_handle error) {
  MockGrpcClosure* mock_closure = static_cast<MockGrpcClosure*>(arg);
  mock_closure->Callback(error);
}

using TestingMetadata = std::vector<std::pair<std::string, std::string>>;

Metadata TestingMetadataToMetadata(const TestingMetadata& md) {
  Metadata result(md.size());
  for (size_t i = 0; i < md.size(); ++i) {
    result[i].key = grpc_slice_from_cpp_string(md[i].first);
    result[i].value = grpc_slice_from_cpp_string(md[i].second);
  }
  return result;
}

bool operator==(const Metadata& lhs, const TestingMetadata& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].ViewKey() != rhs[i].first) {
      return false;
    }
    if (lhs[i].ViewValue() != rhs[i].second) {
      return false;
    }
  }
  return true;
}

bool operator!=(const Metadata& lhs, const TestingMetadata& rhs) {
  return !(lhs == rhs);
}

// Matches with transactions having the desired flag, method_ref,
// initial_metadata, and message_data.
MATCHER_P4(TransactionMatches, flag, method_ref, initial_metadata, message_data,
           "") {
  Transaction tx = arg;
  if (tx.GetFlags() != flag) return false;
  if (flag & kFlagPrefix) {
    if (tx.GetMethodRef() != method_ref) return false;
    if (tx.GetPrefixMetadata() != initial_metadata) return false;
  }
  if (flag & kFlagMessageData) {
    if (tx.GetMessageData()->count != message_data.size()) return false;
    grpc_slice_buffer* buffer = tx.GetMessageData();
    std::vector<grpc_slice> slices;
    while (buffer->count > 0) {
      slices.push_back(grpc_slice_buffer_take_first(buffer));
    }
    for (size_t i = 0; i < slices.size(); ++i) {
      if (grpc_core::StringViewFromSlice(slices[i]) != message_data[i]) {
        return false;
      }
      grpc_slice_buffer_add(buffer, slices[i]);
    }
  }
  return true;
}

// Matches with grpc_error having error message containing |msg|.
MATCHER_P(GrpcErrorMessageContains, msg, "") {
  return absl::StrContains(grpc_error_std_string(arg), msg);
}

// Verify that the lower-level metadata has the same content as the gRPC
// metadata.
void VerifyMetadataEqual(const TestingMetadata& md,
                         grpc_metadata_batch grpc_md) {
  grpc_linked_mdelem* elm = grpc_md.list.head;
  for (size_t i = 0; i < md.size(); ++i) {
    ASSERT_NE(elm, nullptr);
    EXPECT_EQ(grpc_core::StringViewFromSlice(GRPC_MDKEY(elm->md)), md[i].first);
    EXPECT_EQ(grpc_core::StringViewFromSlice(GRPC_MDVALUE(elm->md)),
              md[i].second);
    elm = elm->next;
  }
  EXPECT_EQ(elm, nullptr);
}

// RAII helper classes for constructing gRPC metadata and receiving callbacks.
struct MakeSendInitialMetadata {
  MakeSendInitialMetadata(const TestingMetadata& initial_metadata,
                          const std::string& method_ref,
                          grpc_transport_stream_op_batch* op)
      : storage(initial_metadata.size()) {
    grpc_metadata_batch_init(&grpc_initial_metadata);
    size_t i = 0;
    for (const auto& md : initial_metadata) {
      const std::string& key = md.first;
      const std::string& value = md.second;
      EXPECT_EQ(grpc_metadata_batch_add_tail(
                    &grpc_initial_metadata, &storage[i],
                    grpc_mdelem_from_slices(grpc_slice_from_cpp_string(key),
                                            grpc_slice_from_cpp_string(value))),
                GRPC_ERROR_NONE);
      i++;
    }
    if (!method_ref.empty()) {
      EXPECT_EQ(
          grpc_metadata_batch_add_tail(
              &grpc_initial_metadata, &method_ref_storage,
              grpc_mdelem_from_slices(GRPC_MDSTR_PATH,
                                      grpc_slice_from_cpp_string(method_ref))),
          GRPC_ERROR_NONE);
    }
    op->send_initial_metadata = true;
    op->payload->send_initial_metadata.send_initial_metadata =
        &grpc_initial_metadata;
  }
  ~MakeSendInitialMetadata() {
    grpc_metadata_batch_destroy(&grpc_initial_metadata);
  }

  std::vector<grpc_linked_mdelem> storage;
  grpc_linked_mdelem method_ref_storage;
  grpc_metadata_batch grpc_initial_metadata{};
};

struct MakeSendMessage {
  MakeSendMessage(const std::string& message,
                  grpc_transport_stream_op_batch* op) {
    grpc_slice_buffer send_buffer;
    grpc_slice_buffer_init(&send_buffer);
    grpc_slice send_slice = grpc_slice_from_cpp_string(message);
    grpc_slice_buffer_add(&send_buffer, send_slice);

    send_stream.Init(&send_buffer, 0);
    grpc_slice_buffer_destroy(&send_buffer);

    op->send_message = true;
    op->payload->send_message.send_message.reset(send_stream.get());
  }

  grpc_core::ManualConstructor<grpc_core::SliceBufferByteStream> send_stream;
};

struct MakeSendTrailingMetadata {
  explicit MakeSendTrailingMetadata(const TestingMetadata& trailing_metadata,
                                    grpc_transport_stream_op_batch* op) {
    EXPECT_TRUE(trailing_metadata.empty());
    grpc_metadata_batch_init(&grpc_trailing_metadata);

    op->send_trailing_metadata = true;
    op->payload->send_trailing_metadata.send_trailing_metadata =
        &grpc_trailing_metadata;
  }

  grpc_metadata_batch grpc_trailing_metadata{};
};

struct MakeRecvInitialMetadata {
  explicit MakeRecvInitialMetadata(grpc_transport_stream_op_batch* op,
                                   Expectation* call_before = nullptr) {
    grpc_metadata_batch_init(&grpc_initial_metadata);
    op->recv_initial_metadata = true;
    op->payload->recv_initial_metadata.recv_initial_metadata =
        &grpc_initial_metadata;
    op->payload->recv_initial_metadata.recv_initial_metadata_ready =
        ready.GetGrpcClosure();
    if (call_before) {
      EXPECT_CALL(ready, Callback).After(*call_before);
    } else {
      EXPECT_CALL(ready, Callback);
    }
  }

  ~MakeRecvInitialMetadata() {
    grpc_metadata_batch_destroy(&grpc_initial_metadata);
  }

  MockGrpcClosure ready;
  grpc_metadata_batch grpc_initial_metadata;
};

struct MakeRecvMessage {
  explicit MakeRecvMessage(grpc_transport_stream_op_batch* op,
                           Expectation* call_before = nullptr) {
    op->recv_message = true;
    op->payload->recv_message.recv_message = &grpc_message;
    op->payload->recv_message.recv_message_ready = ready.GetGrpcClosure();
    if (call_before) {
      EXPECT_CALL(ready, Callback).After(*call_before);
    } else {
      EXPECT_CALL(ready, Callback);
    }
  }

  MockGrpcClosure ready;
  grpc_core::OrphanablePtr<grpc_core::ByteStream> grpc_message;
};

struct MakeRecvTrailingMetadata {
  explicit MakeRecvTrailingMetadata(grpc_transport_stream_op_batch* op,
                                    Expectation* call_before = nullptr) {
    grpc_metadata_batch_init(&grpc_trailing_metadata);
    op->recv_trailing_metadata = true;
    op->payload->recv_trailing_metadata.recv_trailing_metadata =
        &grpc_trailing_metadata;
    op->payload->recv_trailing_metadata.recv_trailing_metadata_ready =
        ready.GetGrpcClosure();
    if (call_before) {
      EXPECT_CALL(ready, Callback).After(*call_before);
    } else {
      EXPECT_CALL(ready, Callback);
    }
  }

  ~MakeRecvTrailingMetadata() {
    grpc_metadata_batch_destroy(&grpc_trailing_metadata);
  }

  MockGrpcClosure ready;
  grpc_metadata_batch grpc_trailing_metadata;
};

const TestingMetadata kDefaultMetadata = {
    {"", ""},
    {"", "value"},
    {"key", ""},
    {"key", "value"},
};

constexpr char kDefaultMethodRef[] = "/some/path";
constexpr char kDefaultMessage[] = "binder transport message";
constexpr int kDefaultStatus = 0x1234;

TestingMetadata AppendMethodRef(const TestingMetadata& md,
                                const std::string& method_ref) {
  TestingMetadata result = md;
  result.emplace_back(":path", method_ref);
  return result;
}

TestingMetadata AppendStatus(const TestingMetadata& md, int status) {
  TestingMetadata result = md;
  result.emplace_back("grpc-status", std::to_string(status));
  return result;
}

}  // namespace

TEST_F(BinderTransportTest, CreateBinderTransport) {
  EXPECT_NE(transport_, nullptr);
}

TEST_F(BinderTransportTest, TransactionIdIncrement) {
  grpc_binder_stream* gbs0 = InitNewBinderStream();
  EXPECT_EQ(gbs0->t, GetBinderTransport());
  EXPECT_EQ(gbs0->tx_code, kFirstCallId);
  grpc_binder_stream* gbs1 = InitNewBinderStream();
  EXPECT_EQ(gbs1->t, GetBinderTransport());
  EXPECT_EQ(gbs1->tx_code, kFirstCallId + 1);
  grpc_binder_stream* gbs2 = InitNewBinderStream();
  EXPECT_EQ(gbs2->t, GetBinderTransport());
  EXPECT_EQ(gbs2->tx_code, kFirstCallId + 2);
}

TEST_F(BinderTransportTest, PerformSendInitialMetadata) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;
  const TestingMetadata kInitialMetadata = kDefaultMetadata;
  MakeSendInitialMetadata send_initial_metadata(kInitialMetadata, "", &op);
  MockGrpcClosure mock_on_complete;
  op.on_complete = mock_on_complete.GetGrpcClosure();

  ::testing::InSequence sequence;
  EXPECT_CALL(GetWireWriter(),
              RpcCall(TransactionMatches(kFlagPrefix, "", kInitialMetadata,
                                         std::vector<std::string>{""})));
  EXPECT_CALL(mock_on_complete, Callback);

  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();
}

TEST_F(BinderTransportTest, PerformSendInitialMetadataMethodRef) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;
  const TestingMetadata kInitialMetadata = kDefaultMetadata;
  const std::string kMethodRef = kDefaultMethodRef;
  MakeSendInitialMetadata send_initial_metadata(kInitialMetadata, kMethodRef,
                                                &op);
  MockGrpcClosure mock_on_complete;
  op.on_complete = mock_on_complete.GetGrpcClosure();

  ::testing::InSequence sequence;
  EXPECT_CALL(GetWireWriter(),
              RpcCall(TransactionMatches(kFlagPrefix, kMethodRef.substr(1),
                                         kInitialMetadata,
                                         std::vector<std::string>{""})));
  EXPECT_CALL(mock_on_complete, Callback);

  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();
}

TEST_F(BinderTransportTest, PerformSendMessage) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  const std::string kMessage = kDefaultMessage;
  MakeSendMessage send_message(kMessage, &op);
  MockGrpcClosure mock_on_complete;
  op.on_complete = mock_on_complete.GetGrpcClosure();

  ::testing::InSequence sequence;
  EXPECT_CALL(GetWireWriter(),
              RpcCall(TransactionMatches(kFlagMessageData, "", Metadata{},
                                         std::vector<std::string>{kMessage})));
  EXPECT_CALL(mock_on_complete, Callback);

  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();
}

TEST_F(BinderTransportTest, PerformSendTrailingMetadata) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;
  // The wireformat guarantees that suffix metadata will always be empty.
  // TODO(waynetu): Check whether gRPC can internally add extra trailing
  // metadata.
  const TestingMetadata kTrailingMetadata = {};
  MakeSendTrailingMetadata send_trailing_metadata(kTrailingMetadata, &op);
  MockGrpcClosure mock_on_complete;
  op.on_complete = mock_on_complete.GetGrpcClosure();

  ::testing::InSequence sequence;
  EXPECT_CALL(GetWireWriter(),
              RpcCall(TransactionMatches(kFlagSuffix, "", kTrailingMetadata,
                                         std::vector<std::string>{""})));
  EXPECT_CALL(mock_on_complete, Callback);

  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();
}

TEST_F(BinderTransportTest, PerformSendAll) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  const TestingMetadata kInitialMetadata = kDefaultMetadata;
  const std::string kMethodRef = kDefaultMethodRef;
  MakeSendInitialMetadata send_initial_metadata(kInitialMetadata, kMethodRef,
                                                &op);

  const std::string kMessage = kDefaultMessage;
  MakeSendMessage send_message(kMessage, &op);

  // The wireformat guarantees that suffix metadata will always be empty.
  // TODO(waynetu): Check whether gRPC can internally add extra trailing
  // metadata.
  const TestingMetadata kTrailingMetadata = {};
  MakeSendTrailingMetadata send_trailing_metadata(kTrailingMetadata, &op);

  MockGrpcClosure mock_on_complete;
  op.on_complete = mock_on_complete.GetGrpcClosure();

  ::testing::InSequence sequence;
  EXPECT_CALL(GetWireWriter(), RpcCall(TransactionMatches(
                                   kFlagPrefix | kFlagMessageData | kFlagSuffix,
                                   kMethodRef.substr(1), kInitialMetadata,
                                   std::vector<std::string>{kMessage})));
  EXPECT_CALL(mock_on_complete, Callback);

  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();
}

TEST_F(BinderTransportTest, PerformRecvInitialMetadata) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  MakeRecvInitialMetadata recv_initial_metadata(&op);

  const TestingMetadata kInitialMetadata = kDefaultMetadata;
  auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
  gbt->transport_stream_receiver->NotifyRecvInitialMetadata(
      gbs->tx_code, TestingMetadataToMetadata(kInitialMetadata));
  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();

  VerifyMetadataEqual(kInitialMetadata,
                      recv_initial_metadata.grpc_initial_metadata);
}

TEST_F(BinderTransportTest, PerformRecvInitialMetadataWithMethodRef) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  MakeRecvInitialMetadata recv_initial_metadata(&op);

  auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
  const TestingMetadata kInitialMetadataWithMethodRef =
      AppendMethodRef(kDefaultMetadata, kDefaultMethodRef);
  gbt->transport_stream_receiver->NotifyRecvInitialMetadata(
      gbs->tx_code, TestingMetadataToMetadata(kInitialMetadataWithMethodRef));
  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();

  VerifyMetadataEqual(kInitialMetadataWithMethodRef,
                      recv_initial_metadata.grpc_initial_metadata);
}

TEST_F(BinderTransportTest, PerformRecvMessage) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  MakeRecvMessage recv_message(&op);

  auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
  const std::string kMessage = kDefaultMessage;
  gbt->transport_stream_receiver->NotifyRecvMessage(gbs->tx_code, kMessage);

  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();
  EXPECT_TRUE(recv_message.grpc_message->Next(SIZE_MAX, nullptr));
  grpc_slice slice;
  recv_message.grpc_message->Pull(&slice);
  EXPECT_EQ(kMessage,
            std::string(reinterpret_cast<char*>(GRPC_SLICE_START_PTR(slice)),
                        GRPC_SLICE_LENGTH(slice)));
  grpc_slice_unref_internal(slice);
}

TEST_F(BinderTransportTest, PerformRecvTrailingMetadata) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  MakeRecvTrailingMetadata recv_trailing_metadata(&op);

  const TestingMetadata kTrailingMetadata = kDefaultMetadata;
  auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
  constexpr int kStatus = kDefaultStatus;
  gbt->transport_stream_receiver->NotifyRecvTrailingMetadata(
      gbs->tx_code, TestingMetadataToMetadata(kTrailingMetadata), kStatus);
  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();
  VerifyMetadataEqual(AppendStatus(kTrailingMetadata, kStatus),
                      recv_trailing_metadata.grpc_trailing_metadata);
}

TEST_F(BinderTransportTest, PerformRecvAll) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  MakeRecvInitialMetadata recv_initial_metadata(&op);
  MakeRecvMessage recv_message(&op);
  MakeRecvTrailingMetadata recv_trailing_metadata(&op);

  auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
  const TestingMetadata kInitialMetadataWithMethodRef =
      AppendMethodRef(kDefaultMetadata, kDefaultMethodRef);
  gbt->transport_stream_receiver->NotifyRecvInitialMetadata(
      gbs->tx_code, TestingMetadataToMetadata(kInitialMetadataWithMethodRef));

  const std::string kMessage = kDefaultMessage;
  gbt->transport_stream_receiver->NotifyRecvMessage(gbs->tx_code, kMessage);

  TestingMetadata trailing_metadata = kDefaultMetadata;
  constexpr int kStatus = kDefaultStatus;
  gbt->transport_stream_receiver->NotifyRecvTrailingMetadata(
      gbs->tx_code, TestingMetadataToMetadata(trailing_metadata), kStatus);
  PerformStreamOp(gbs, &op);
  exec_ctx.Flush();

  VerifyMetadataEqual(kInitialMetadataWithMethodRef,
                      recv_initial_metadata.grpc_initial_metadata);
  trailing_metadata.emplace_back("grpc-status", std::to_string(kStatus));
  VerifyMetadataEqual(trailing_metadata,
                      recv_trailing_metadata.grpc_trailing_metadata);
  EXPECT_TRUE(recv_message.grpc_message->Next(SIZE_MAX, nullptr));
  grpc_slice slice;
  recv_message.grpc_message->Pull(&slice);
  EXPECT_EQ(kMessage,
            std::string(reinterpret_cast<char*>(GRPC_SLICE_START_PTR(slice)),
                        GRPC_SLICE_LENGTH(slice)));
  grpc_slice_unref_internal(slice);
}

TEST_F(BinderTransportTest, PerformAllOps) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();
  grpc_transport_stream_op_batch op{};
  grpc_transport_stream_op_batch_payload payload(nullptr);
  op.payload = &payload;

  const TestingMetadata kSendInitialMetadata = kDefaultMetadata;
  const std::string kMethodRef = kDefaultMethodRef;
  MakeSendInitialMetadata send_initial_metadata(kSendInitialMetadata,
                                                kMethodRef, &op);

  const std::string kSendMessage = kDefaultMessage;
  MakeSendMessage send_message(kSendMessage, &op);

  // The wireformat guarantees that suffix metadata will always be empty.
  // TODO(waynetu): Check whether gRPC can internally add extra trailing
  // metadata.
  const TestingMetadata kSendTrailingMetadata = {};
  MakeSendTrailingMetadata send_trailing_metadata(kSendTrailingMetadata, &op);

  MockGrpcClosure mock_on_complete;
  op.on_complete = mock_on_complete.GetGrpcClosure();

  // TODO(waynetu): Currently, we simply drop the prefix '/' from the :path
  // argument to obtain the method name. Update the test if this turns out to be
  // incorrect.
  EXPECT_CALL(GetWireWriter(), RpcCall(TransactionMatches(
                                   kFlagPrefix | kFlagMessageData | kFlagSuffix,
                                   kMethodRef.substr(1), kSendInitialMetadata,
                                   std::vector<std::string>{kSendMessage})));
  Expectation on_complete = EXPECT_CALL(mock_on_complete, Callback);

  // Recv callbacks can happen after the on_complete callback.
  MakeRecvInitialMetadata recv_initial_metadata(
      &op, /* call_before = */ &on_complete);
  MakeRecvMessage recv_message(&op, /* call_before = */ &on_complete);
  MakeRecvTrailingMetadata recv_trailing_metadata(
      &op, /* call_before = */ &on_complete);

  PerformStreamOp(gbs, &op);

  // Flush the execution context to force on_complete to run before recv
  // callbacks get scheduled.
  exec_ctx.Flush();

  auto* gbt = reinterpret_cast<grpc_binder_transport*>(transport_);
  const TestingMetadata kRecvInitialMetadata =
      AppendMethodRef(kDefaultMetadata, kDefaultMethodRef);
  gbt->transport_stream_receiver->NotifyRecvInitialMetadata(
      gbs->tx_code, TestingMetadataToMetadata(kRecvInitialMetadata));
  const std::string kRecvMessage = kDefaultMessage;
  gbt->transport_stream_receiver->NotifyRecvMessage(gbs->tx_code, kRecvMessage);
  const TestingMetadata kRecvTrailingMetadata = kDefaultMetadata;
  constexpr int kStatus = 0x1234;
  gbt->transport_stream_receiver->NotifyRecvTrailingMetadata(
      gbs->tx_code, TestingMetadataToMetadata(kRecvTrailingMetadata), kStatus);

  exec_ctx.Flush();
  VerifyMetadataEqual(kRecvInitialMetadata,
                      recv_initial_metadata.grpc_initial_metadata);
  VerifyMetadataEqual(AppendStatus(kRecvTrailingMetadata, kStatus),
                      recv_trailing_metadata.grpc_trailing_metadata);

  EXPECT_TRUE(recv_message.grpc_message->Next(SIZE_MAX, nullptr));
  grpc_slice slice;
  recv_message.grpc_message->Pull(&slice);
  EXPECT_EQ(kRecvMessage,
            std::string(reinterpret_cast<char*>(GRPC_SLICE_START_PTR(slice)),
                        GRPC_SLICE_LENGTH(slice)));
  grpc_slice_unref_internal(slice);
}

TEST_F(BinderTransportTest, WireWriterRpcCallErrorPropagates) {
  grpc_core::ExecCtx exec_ctx;
  grpc_binder_stream* gbs = InitNewBinderStream();

  MockGrpcClosure mock_on_complete1;
  MockGrpcClosure mock_on_complete2;

  EXPECT_CALL(GetWireWriter(), RpcCall)
      .WillOnce(Return(absl::OkStatus()))
      .WillOnce(Return(absl::InternalError("WireWriter::RpcCall failed")));
  EXPECT_CALL(mock_on_complete1, Callback(GRPC_ERROR_NONE));
  EXPECT_CALL(mock_on_complete2,
              Callback(GrpcErrorMessageContains("WireWriter::RpcCall failed")));

  const TestingMetadata kInitialMetadata = {};
  grpc_transport_stream_op_batch op1{};
  grpc_transport_stream_op_batch_payload payload1(nullptr);
  op1.payload = &payload1;
  MakeSendInitialMetadata send_initial_metadata1(kInitialMetadata, "", &op1);
  op1.on_complete = mock_on_complete1.GetGrpcClosure();

  grpc_transport_stream_op_batch op2{};
  grpc_transport_stream_op_batch_payload payload2(nullptr);
  op2.payload = &payload2;
  MakeSendInitialMetadata send_initial_metadata2(kInitialMetadata, "", &op2);
  op2.on_complete = mock_on_complete2.GetGrpcClosure();

  PerformStreamOp(gbs, &op1);
  PerformStreamOp(gbs, &op2);
  exec_ctx.Flush();
}

}  // namespace grpc_binder

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(argc, argv);
  return RUN_ALL_TESTS();
}
