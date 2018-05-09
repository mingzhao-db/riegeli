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

#ifndef RIEGELI_CHUNK_ENCODING_CHUNK_DECODER_H_
#define RIEGELI_CHUNK_ENCODING_CHUNK_DECODER_H_

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/chain_reader.h"
#include "riegeli/chunk_encoding/field_filter.h"
#include "riegeli/chunk_encoding/types.h"

// Forward declarations to reduce the amount of includes going into public
// record_reader.h.
namespace google {
namespace protobuf {
class MessageLite;
}  // namespace protobuf
}  // namespace google

namespace riegeli {

// Forward declarations to reduce the amount of includes going into public
// record_reader.h.
class Chunk;
class ChunkHeader;

class ChunkDecoder : public Object {
 public:
  class Options {
   public:
    Options() noexcept {}

    // Specifies the set of fields to be included in returned records, allowing
    // to exclude the remaining fields (but does not guarantee exclusion).
    // Excluding data makes reading faster.
    Options& set_field_filter(FieldFilter field_filter) & {
      field_filter_ = std::move(field_filter);
      return *this;
    }
    Options&& set_field_filter(FieldFilter field_filter) && {
      return std::move(set_field_filter(std::move(field_filter)));
    }

   private:
    friend class ChunkDecoder;

    FieldFilter field_filter_ = FieldFilter::All();
  };

  // Creates an empty ChunkDecoder.
  explicit ChunkDecoder(Options options = Options());

  ChunkDecoder(ChunkDecoder&& src) noexcept;
  ChunkDecoder& operator=(ChunkDecoder&& src) noexcept;

  // Resets the ChunkDecoder to an empty chunk.
  void Reset();

  // Resets the ChunkDecoder and parses the chunk.
  //
  // chunk.header.chunk_type() must not be ChunkType::kFileSignature or
  // ChunkType::kPadding.
  //
  // Return values:
  //  * true  - success (healthy())
  //  * false - failure (!healthy())
  bool Reset(const Chunk& chunk);

  // Reads the next record.
  //
  // ReadRecord(MessageLite*) parses raw bytes to a proto message after reading.
  // The remaining overloads read raw bytes (they never generate a new failure).
  // For ReadRecord(string_view*) the string_view is valid until the next
  // non-const operation on this ChunkDecoder.
  //
  // If key != nullptr, *key is set to the record index on success.
  //
  // Return values:
  //  * true                    - success (*record is set, healthy())
  //  * false (when healthy())  - chunk ends
  //  * false (when !healthy()) - failure
  bool ReadRecord(google::protobuf::MessageLite* record);
  bool ReadRecord(absl::string_view* record);
  bool ReadRecord(std::string* record);
  bool ReadRecord(Chain* record);

  // If !healthy() and the failure was caused by an unparsable message, then
  // Recover() allows reading again by skipping the unparsable message.
  //
  // If healthy(), or if !healthy() but the failure was not caused by an
  // unparsable message, then Recover() does nothing and returns false.
  //
  // Return values:
  //  * true  - success
  //  * false - failure not caused by an unparsable message
  bool Recover();

  uint64_t index() const { return index_; }
  void SetIndex(uint64_t index);
  uint64_t num_records() const { return IntCast<uint64_t>(limits_.size()); }

 protected:
  void Done() override;

 private:
  bool Parse(const ChunkHeader& header, ChainReader* src, Chain* dest);

  FieldFilter field_filter_;
  // Invariants:
  //   limits_ are sorted
  //   (limits_.empty() ? 0 : limits_.back()) == size of values_reader_
  //   (index_ == 0 ? 0 : limits_[index_ - 1]) == values_reader_.pos()
  std::vector<size_t> limits_;
  ChainReader values_reader_;
  // Invariant: index_ <= num_records()
  uint64_t index_ = 0;
  std::string record_scratch_;
  // Whether Recover() is applicable.
  //
  // Invariant: if recoverable_ then !healthy()
  bool recoverable_ = false;
};

// Implementation details follow.

inline bool ChunkDecoder::ReadRecord(absl::string_view* record) {
  if (ABSL_PREDICT_FALSE(index() == num_records() || !healthy())) return false;
  const size_t start = IntCast<size_t>(values_reader_.pos());
  const size_t limit = limits_[IntCast<size_t>(index_++)];
  RIEGELI_ASSERT_LE(start, limit)
      << "Failed invariant of ChunkDecoder: record end positions not sorted";
  if (!values_reader_.Read(record, &record_scratch_, limit - start)) {
    RIEGELI_ASSERT_UNREACHABLE() << "Failed reading record from values reader: "
                                 << values_reader_.message();
  }
  return true;
}

inline bool ChunkDecoder::ReadRecord(std::string* record) {
  if (ABSL_PREDICT_FALSE(index() == num_records() || !healthy())) return false;
  const size_t start = IntCast<size_t>(values_reader_.pos());
  const size_t limit = limits_[IntCast<size_t>(index_++)];
  RIEGELI_ASSERT_LE(start, limit)
      << "Failed invariant of ChunkDecoder: record end positions not sorted";
  record->clear();
  if (!values_reader_.Read(record, limit - start)) {
    RIEGELI_ASSERT_UNREACHABLE() << "Failed reading record from values reader: "
                                 << values_reader_.message();
  }
  return true;
}

inline bool ChunkDecoder::ReadRecord(Chain* record) {
  if (ABSL_PREDICT_FALSE(index() == num_records() || !healthy())) return false;
  const size_t start = IntCast<size_t>(values_reader_.pos());
  const size_t limit = limits_[IntCast<size_t>(index_++)];
  RIEGELI_ASSERT_LE(start, limit)
      << "Failed invariant of ChunkDecoder: record end positions not sorted";
  record->Clear();
  if (!values_reader_.Read(record, limit - start)) {
    RIEGELI_ASSERT_UNREACHABLE() << "Failed reading record from values reader: "
                                 << values_reader_.message();
  }
  return true;
}

inline void ChunkDecoder::SetIndex(uint64_t index) {
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of ChunkDecoder::SetIndex(): " << message();
  index_ = UnsignedMin(index, num_records());
  const size_t start =
      index_ == 0 ? size_t{0} : limits_[IntCast<size_t>(index_ - 1)];
  if (!values_reader_.Seek(start)) {
    RIEGELI_ASSERT_UNREACHABLE()
        << "Failed seeking values reader: " << values_reader_.message();
  }
}

}  // namespace riegeli

#endif  // RIEGELI_CHUNK_ENCODING_CHUNK_DECODER_H_
