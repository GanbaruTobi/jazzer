// Copyright 2021 Code Intelligence GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fuzz_target_runner.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/substitute.h"
#include "fuzzed_data_provider.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "java_reproducer_templates.h"
#include "third_party/jni/jni.h"
#include "utils.h"

DEFINE_string(
    target_class, "",
    "The Java class that contains the static fuzzerTestOneInput function");

DEFINE_string(target_args, "",
              "Arguments passed to fuzzerInitialize as a String array. "
              "Separated by space.");
DEFINE_int32(keep_going, 1,
             "Continue fuzzing until N distinct exception stack traces have"
             "been encountered");

DEFINE_string(
    ignore, "",
    "Comma-separated list of crash dedup tokens to ignore. This is useful to "
    "continue fuzzing before a crash is fixed.");

DEFINE_string(reproducer_path, ".",
              "Path at which fuzzing reproducers are stored. Defaults to the "
              "current directory.");

namespace jazzer {
// split a string on unescaped spaces
std::vector<std::string> splitOnSpace(const std::string &s) {
  if (s.empty()) {
    return {};
  }

  std::vector<std::string> tokens;
  std::size_t token_begin = 0;
  for (std::size_t i = 1; i < s.size() - 1; i++) {
    // only split if the space is not escaped by a backslash "\"
    if (s[i] == ' ' && s[i - 1] != '\\') {
      // don't split on multiple spaces
      if (i > token_begin + 1)
        tokens.push_back(s.substr(token_begin, i - token_begin));
      token_begin = i + 1;
    }
  }
  tokens.push_back(s.substr(token_begin));
  return tokens;
}

FuzzTargetRunner::FuzzTargetRunner(
    JVM &jvm, const std::vector<std::string> &additional_target_args)
    : ExceptionPrinter(jvm), jvm_(jvm), ignore_tokens_() {
  auto &env = jvm.GetEnv();
  // the first positional argument should be the fuzz target class
  if (FLAGS_target_class.empty()) {
    std::cerr << "Missing argument --target_class <fuzz_target_class>"
              << std::endl;
    exit(1);
  }
  jclass_ = jvm.FindClass(FLAGS_target_class);
  // one of the following functions is required:
  //    public static boolean fuzzerTestOneInput(byte[] input)
  //    public static boolean fuzzerTestOneInput(FuzzedDataProvider data)
  fuzzer_test_one_input_bytes_ =
      jvm.GetStaticMethodID(jclass_, "fuzzerTestOneInput", "([B)Z", false);
  fuzzer_test_one_input_data_ = jvm.GetStaticMethodID(
      jclass_, "fuzzerTestOneInput",
      "(Lcom/code_intelligence/jazzer/api/FuzzedDataProvider;)Z", false);
  bool using_bytes = fuzzer_test_one_input_bytes_ != nullptr;
  bool using_data = fuzzer_test_one_input_data_ != nullptr;
  // Fail if none ore both of the two possible fuzzerTestOneInput versions is
  // defined in the class.
  if (using_bytes == using_data) {
    LOG(ERROR) << FLAGS_target_class
               << " must define exactly one of the following two functions:";
    LOG(ERROR) << "public static boolean fuzzerTestOneInput(byte[] ...)";
    LOG(ERROR)
        << "public static boolean fuzzerTestOneInput(FuzzedDataProvider ...)";
    exit(1);
  }

  // check existence of optional methods for initialization and destruction
  fuzzer_initialize_ =
      jvm.GetStaticMethodID(jclass_, "fuzzerInitialize", "()V", false);
  fuzzer_tear_down_ =
      jvm.GetStaticMethodID(jclass_, "fuzzerTearDown", "()V", false);
  fuzzer_initialize_with_args_ = jvm.GetStaticMethodID(
      jclass_, "fuzzerInitialize", "([Ljava/lang/String;)V", false);

  auto fuzz_target_args_tokens = splitOnSpace(FLAGS_target_args);
  fuzz_target_args_tokens.insert(fuzz_target_args_tokens.end(),
                                 additional_target_args.begin(),
                                 additional_target_args.end());

  if (fuzzer_initialize_with_args_) {
    // fuzzerInitialize with arguments gets priority
    jclass string_class = jvm.FindClass("java/lang/String");
    jobjectArray arg_array = jvm.GetEnv().NewObjectArray(
        fuzz_target_args_tokens.size(), string_class, nullptr);
    for (std::size_t i = 0; i < fuzz_target_args_tokens.size(); i++) {
      jstring str = env.NewStringUTF(fuzz_target_args_tokens[i].c_str());
      env.SetObjectArrayElement(arg_array, i, str);
    }
    env.CallStaticObjectMethod(jclass_, fuzzer_initialize_with_args_,
                               arg_array);
  } else if (fuzzer_initialize_) {
    env.CallStaticVoidMethod(jclass_, fuzzer_initialize_);
  } else {
    LOG(INFO) << "did not call any fuzz target initialize functions";
  }

  if (env.ExceptionOccurred()) {
    LOG(ERROR) << "== Java Exception in fuzzerInitialize: ";
    LOG(ERROR) << getAndClearException();
    std::exit(1);
  }

  SetUpFuzzedDataProvider(jvm_);

  // Parse a comma-separated list of hex dedup tokens.
  std::vector<std::string> str_ignore_tokens =
      absl::StrSplit(FLAGS_ignore, ',');
  for (const std::string &str_token : str_ignore_tokens) {
    if (str_token.empty()) continue;
    try {
      ignore_tokens_.push_back(std::stoull(str_token, nullptr, 16));
    } catch (...) {
      LOG(ERROR) << "Invalid dedup token (expected up to 16 hex digits): '"
                 << str_token << "'";
      // Don't let libFuzzer print a crash stack trace.
      std::quick_exit(1);
    }
  }
}

FuzzTargetRunner::~FuzzTargetRunner() {
  if (fuzzer_tear_down_ != nullptr) {
    std::cerr << "calling fuzzer teardown function" << std::endl;
    jvm_.GetEnv().CallStaticVoidMethod(jclass_, fuzzer_tear_down_);
    std::cerr << getAndClearException() << std::endl;
  }
}

RunResult FuzzTargetRunner::Run(const uint8_t *data, const std::size_t size) {
  auto &env = jvm_.GetEnv();
  bool trigger_exit;
  if (fuzzer_test_one_input_data_ != nullptr) {
    FeedFuzzedDataProvider(data, size);
    trigger_exit =
        env.CallStaticBooleanMethod(jclass_, fuzzer_test_one_input_data_,
                                    GetFuzzedDataProviderJavaObject(jvm_));
  } else {
    jbyteArray byte_array = env.NewByteArray(size);
    if (byte_array == nullptr) {
      LOG(ERROR) << getAndClearException();
      throw std::runtime_error(std::string("Cannot create byte array"));
    }
    env.SetByteArrayRegion(byte_array, 0, size,
                           reinterpret_cast<const jbyte *>(data));
    trigger_exit = env.CallStaticBooleanMethod(
        jclass_, fuzzer_test_one_input_bytes_, byte_array);
    env.DeleteLocalRef(byte_array);
  }

  if (trigger_exit) {
    std::cerr << "== Java Assertion Error" << std::endl;
    return RunResult::kAssertion;
  } else if (env.ExceptionOccurred()) {
    jlong dedup_token = computeDedupToken();
    // Check whether this stack trace has been encountered before if
    // `--keep_going` has been supplied.
    if (FLAGS_keep_going > 1 &&
        std::find(ignore_tokens_.cbegin(), ignore_tokens_.cend(),
                  dedup_token) != ignore_tokens_.end()) {
      return RunResult::kOk;
    } else {
      ignore_tokens_.push_back(dedup_token);
      std::cout << std::endl;
      std::cerr << "== Java Exception: " << getAndClearException();
      std::cout << "DEDUP_TOKEN: " << std::hex << std::setfill('0')
                << std::setw(16) << dedup_token << std::endl;
      if (ignore_tokens_.size() < static_cast<std::size_t>(FLAGS_keep_going)) {
        return RunResult::kDumpAndContinue;
      } else {
        return RunResult::kException;
      }
    }
  }
  return RunResult::kOk;
}

void FuzzTargetRunner::DumpReproducer(const uint8_t *data, std::size_t size) {
  auto &env = jvm_.GetEnv();
  std::string base64_data;
  if (fuzzer_test_one_input_data_) {
    // Record the data retrieved from the FuzzedDataProvider and supply it to a
    // Java-only CannedFuzzedDataProvider in the reproducer.
    FeedFuzzedDataProvider(data, size);
    jobject recorder = GetRecordingFuzzedDataProviderJavaObject(jvm_);
    bool result = env.CallStaticBooleanMethod(
        jclass_, fuzzer_test_one_input_data_, recorder);
    if (!result && !env.ExceptionOccurred()) {
      LOG(ERROR) << "Failed to reproduce crash when rerunning with recorder";
      return;
    }
    env.ExceptionClear();
    base64_data = SerializeRecordingFuzzedDataProvider(jvm_, recorder);
  } else {
    absl::string_view data_str(reinterpret_cast<const char *>(data), size);
    absl::Base64Escape(data_str, &base64_data);
  }
  const char *fuzz_target_call = fuzzer_test_one_input_data_
                                     ? kTestOneInputWithData
                                     : kTestOneInputWithBytes;
  std::string data_sha1 = jazzer::Sha1Hash(data, size);
  std::string reproducer =
      absl::Substitute(kBaseReproducer, data_sha1, base64_data,
                       FLAGS_target_class, fuzz_target_call);
  std::string reproducer_filename = absl::StrFormat("Crash_%s.java", data_sha1);
  std::string reproducer_full_path = absl::StrFormat(
      "%s%c%s", FLAGS_reproducer_path, kPathSeparator, reproducer_filename);
  std::ofstream reproducer_out(reproducer_full_path);
  reproducer_out << reproducer;
  std::cout << absl::StrFormat(
                   "reproducer_path='%s'; Java reproducer written to %s",
                   FLAGS_reproducer_path, reproducer_full_path)
            << std::endl;
}
}  // namespace jazzer
