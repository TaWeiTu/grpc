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

#include <android/log.h>
#include <jni.h>
#include "src/core/ext/transport/binder/client/channel_create.h"
#include "test/cpp/microbenchmarks/bm_callback_unary_ping_pong_binder.h"

extern "C" JNIEXPORT jstring JNICALL
Java_io_grpc_binder_cpp_example_ButtonPressHandler_native_1entry(
    JNIEnv* env, jobject /*this*/, jobject application) {
  static bool first = true;
  __android_log_print(ANDROID_LOG_INFO, "Demo", "Line number %d", __LINE__);
  if (first) {
    first = false;
    grpc::experimental::BindToOnDeviceServerService(
        env, application, "io.grpc.binder.cpp.exampleserver",
        "io.grpc.binder.cpp.exampleserver.ExportedEndpointService");
    return env->NewStringUTF("Clicked 1 time");
  } else {
    __android_log_print(ANDROID_LOG_INFO, "Demo", "Line number %d", __LINE__);
    grpc::testing::RunCallbackUnaryPingPongBinderBenchmarks(env, application);
    return env->NewStringUTF("Clicked more than 1 time. Status not ok");
  }
}
