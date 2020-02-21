// Copyright 2017 Google LLC
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

#include "riegeli/bytes/message_serialize.h"

#include <stddef.h>
#include <stdint.h>

#include <limits>
#include <string>
#include <tuple>

#include "absl/base/optimization.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream.h"
#include "google/protobuf/message_lite.h"
#include "riegeli/base/base.h"
#include "riegeli/base/canonical_errors.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/status.h"
#include "riegeli/bytes/chain_writer.h"
#include "riegeli/bytes/cord_writer.h"
#include "riegeli/bytes/string_writer.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

namespace {

// Adapts a `Writer` to a `google::protobuf::io::ZeroCopyOutputStream`.
class WriterOutputStream : public google::protobuf::io::ZeroCopyOutputStream {
 public:
  explicit WriterOutputStream(Writer* dest)
      : dest_(RIEGELI_ASSERT_NOTNULL(dest)), initial_pos_(dest_->pos()) {}

  bool Next(void** data, int* size) override;
  void BackUp(int length) override;
  int64_t ByteCount() const override;

 private:
  Position relative_pos() const;

  Writer* dest_;
  // Invariants:
  //   `dest_->pos() >= initial_pos_`
  //   `dest_->pos() - initial_pos_ <= std::numeric_limits<int64_t>::max()`
  Position initial_pos_;
};

inline Position WriterOutputStream::relative_pos() const {
  RIEGELI_ASSERT_GE(dest_->pos(), initial_pos_)
      << "Failed invariant of WriterOutputStream: "
         "current position smaller than initial position";
  const Position pos = dest_->pos() - initial_pos_;
  RIEGELI_ASSERT_LE(pos, Position{std::numeric_limits<int64_t>::max()})
      << "Failed invariant of WriterOutputStream: relative position overflow";
  return pos;
}

bool WriterOutputStream::Next(void** data, int* size) {
  const Position pos = relative_pos();
  if (ABSL_PREDICT_FALSE(pos ==
                         Position{std::numeric_limits<int64_t>::max()})) {
    return false;
  }
  if (ABSL_PREDICT_FALSE(!dest_->Push())) return false;
  *data = dest_->cursor();
  *size = SaturatingIntCast<int>(UnsignedMin(
      dest_->available(), Position{std::numeric_limits<int64_t>::max()} - pos));
  dest_->move_cursor(IntCast<size_t>(*size));
  return true;
}

void WriterOutputStream::BackUp(int length) {
  RIEGELI_ASSERT_GE(length, 0)
      << "Failed precondition of ZeroCopyOutputStream::BackUp(): "
         "negative length";
  RIEGELI_ASSERT_LE(IntCast<size_t>(length), dest_->written_to_buffer())
      << "Failed precondition of ZeroCopyOutputStream::BackUp(): "
         "length larger than the amount of buffered data";
  dest_->set_cursor(dest_->cursor() - length);
}

int64_t WriterOutputStream::ByteCount() const {
  return IntCast<int64_t>(relative_pos());
}

}  // namespace

namespace internal {

Status SerializeToWriterImpl(const google::protobuf::MessageLite& src,
                             Writer* dest, SerializeOptions options) {
  if (!options.partial() && ABSL_PREDICT_FALSE(!src.IsInitialized())) {
    return InvalidArgumentError(
        absl::StrCat("Failed to serialize message of type ", src.GetTypeName(),
                     " because it is missing required fields: ",
                     src.InitializationErrorString()));
  }
  const size_t size = src.ByteSizeLong();
  if (ABSL_PREDICT_FALSE(size > size_t{std::numeric_limits<int>::max()})) {
    return ResourceExhaustedError(absl::StrCat(
        "Failed to serialize message of type ", src.GetTypeName(),
        " because it exceeds maximum protobuf size of 2GB: ", size));
  }
  WriterOutputStream output_stream(dest);
  google::protobuf::io::CodedOutputStream coded_stream(&output_stream);
  coded_stream.SetSerializationDeterministic(options.deterministic());
  if (ABSL_PREDICT_FALSE(!src.SerializePartialToCodedStream(&coded_stream))) {
    RIEGELI_ASSERT(!dest->healthy())
        << "Failed to serialize message of type " << src.GetTypeName()
        << ": SerializePartialToCodedStream() failed for an unknown reason";
    return dest->status();
  }
  return OkStatus();
}

}  // namespace internal

Status SerializeToString(const google::protobuf::MessageLite& src,
                         std::string* dest, SerializeOptions options) {
  dest->clear();
  return SerializeToWriter<StringWriter<>>(
      src,
      std::forward_as_tuple(
          dest, StringWriterBase::Options().set_size_hint(src.ByteSizeLong())),
      options);
}

Status SerializeToChain(const google::protobuf::MessageLite& src, Chain* dest,
                        SerializeOptions options) {
  dest->Clear();
  return SerializeToWriter<ChainWriter<>>(
      src,
      std::forward_as_tuple(
          dest, ChainWriterBase::Options().set_size_hint(src.ByteSizeLong())),
      options);
}

Status SerializeToCord(const google::protobuf::MessageLite& src,
                       absl::Cord* dest, SerializeOptions options) {
  dest->Clear();
  return SerializeToWriter<CordWriter<>>(
      src,
      std::forward_as_tuple(
          dest, CordWriterBase::Options().set_size_hint(src.ByteSizeLong())),
      options);
}

}  // namespace riegeli
