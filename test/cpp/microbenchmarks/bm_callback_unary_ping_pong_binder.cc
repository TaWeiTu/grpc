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

#include "test/cpp/microbenchmarks/bm_callback_unary_ping_pong_binder.h"

#ifdef GPR_ANDROID

// Some distros have RunSpecifiedBenchmarks under the benchmark namespace,
// and others do not. This allows us to support both modes.
namespace benchmark {
void RunTheBenchmarksNamespaced() { RunSpecifiedBenchmarks(); }
}  // namespace benchmark

namespace grpc {
namespace testing {

static void SweepSizesArgs(benchmark::internal::Benchmark* b) {
  b->Args({0, 0});
  for (int i = 1; i <= 128 * 1024 * 1024; i *= 8) {
    b->Args({i, 0});
    b->Args({0, i});
    b->Args({i, i});
  }
}

void RunCallbackUnaryPingPongBinderBenchmarks(JNIEnv* env,
                                              jobject application) {
  benchmark::RegisterBenchmark("callback", BM_CallbackUnaryPingPongBinder, env,
                               application)
      ->Apply(SweepSizesArgs);
  int argc = 1;
  char* argv[] = {const_cast<char*>("benchmark")};
  benchmark::Initialize(&argc, argv);
  benchmark::RunTheBenchmarksNamespaced();
}

}  // namespace testing
}  // namespace grpc

#endif  // GPR_ANDROID
