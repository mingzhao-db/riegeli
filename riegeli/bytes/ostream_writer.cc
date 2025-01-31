// Copyright 2019 Google LLC
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

#include "riegeli/bytes/ostream_writer.h"

#include <stddef.h>

#include <cerrno>
#include <limits>
#include <ostream>
#include <string>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "riegeli/base/base.h"
#include "riegeli/base/errno_mapping.h"
#include "riegeli/bytes/istream_reader.h"
#include "riegeli/bytes/reader.h"

namespace riegeli {

void OstreamWriterBase::Initialize(std::ostream* dest,
                                   absl::optional<Position> assumed_pos) {
  RIEGELI_ASSERT(dest != nullptr)
      << "Failed precondition of OstreamWriter: null stream pointer";
  RIEGELI_ASSERT(supports_random_access_ == LazyBoolState::kFalse)
      << "Failed precondition of OstreamWriterBase::Initialize(): "
         "supports_random_access_ not reset";
  RIEGELI_ASSERT(supports_read_mode_ == LazyBoolState::kFalse)
      << "Failed precondition of OstreamWriterBase::Initialize(): "
         "supports_read_mode_ not reset";
  if (ABSL_PREDICT_FALSE(dest->fail())) {
    // Either constructing the stream failed or the stream was already in a
    // failed state. In any case `OstreamWriterBase` should fail.
    FailOperation("ostream::ostream()");
    return;
  }
  if (assumed_pos != absl::nullopt) {
    if (ABSL_PREDICT_FALSE(
            *assumed_pos >
            Position{std::numeric_limits<std::streamoff>::max()})) {
      FailOverflow();
      return;
    }
    set_start_pos(*assumed_pos);
  } else {
    const std::streamoff stream_pos = dest->tellp();
    if (stream_pos < 0) {
      // Random access is not supported. Assume 0 as the initial position.
      return;
    }
    set_start_pos(IntCast<Position>(stream_pos));
    // `std::ostream::tellp()` succeeded, and `std::ostream::seekp()` will be
    // checked later.
    supports_random_access_ = LazyBoolState::kUnknown;
    supports_read_mode_ = LazyBoolState::kUnknown;
  }
}

void OstreamWriterBase::Done() {
  BufferedWriter::Done();
  // If `supports_random_access_` is still `LazyBoolState::kUnknown`, change it
  // to `LazyBoolState::kFalse`, because trying to resolve it later might access
  // a closed stream. The resolution is no longer interesting anyway.
  if (supports_random_access_ == LazyBoolState::kUnknown) {
    supports_random_access_ = LazyBoolState::kFalse;
  }
  // Same for `supports_read_mode_`.
  if (supports_read_mode_ == LazyBoolState::kUnknown) {
    supports_read_mode_ = LazyBoolState::kFalse;
  }
  associated_reader_.Reset();
}

bool OstreamWriterBase::FailOperation(absl::string_view operation) {
  // There is no way to get details why a stream operation failed without
  // letting the stream throw exceptions. Hopefully low level failures have set
  // `errno` as a side effect.
  //
  // This requires resetting `errno` to 0 before the stream operation because
  // the operation may fail without setting `errno`.
  const int error_number = errno;
  const std::string message = absl::StrCat(operation, " failed");
  return Fail(error_number == 0
                  ? absl::UnknownError(message)
                  : ErrnoToCanonicalStatus(error_number, message));
}

bool OstreamWriterBase::supports_random_access() {
  switch (supports_random_access_) {
    case LazyBoolState::kFalse:
      return false;
    case LazyBoolState::kTrue:
      return true;
    case LazyBoolState::kUnknown:
      break;
  }
  RIEGELI_ASSERT(is_open())
      << "Failed invariant of OstreamWriterBase: "
         "unresolved supports_random_access_ but object closed";
  std::ostream& dest = *dest_stream();
  bool supported = false;
  dest.seekp(0, std::ios_base::end);
  if (dest.fail()) {
    dest.clear(dest.rdstate() & ~std::ios_base::failbit);
  } else {
    errno = 0;
    dest.seekp(IntCast<std::streamoff>(start_pos()), std::ios_base::beg);
    if (ABSL_PREDICT_FALSE(dest.fail())) {
      FailOperation("ostream::seekp()");
    } else {
      supported = true;
    }
  }
  supports_random_access_ =
      supported ? LazyBoolState::kTrue : LazyBoolState::kFalse;
  return supported;
}

bool OstreamWriterBase::supports_read_mode() {
  switch (supports_read_mode_) {
    case LazyBoolState::kFalse:
      return false;
    case LazyBoolState::kTrue:
      return true;
    case LazyBoolState::kUnknown:
      break;
  }
  RIEGELI_ASSERT(is_open())
      << "Failed invariant of OstreamWriterBase: "
         "unresolved supports_read_mode_ but object closed";
  std::istream* const src = src_stream();
  bool supported = false;
  if (src != nullptr) {
    const std::streamoff stream_pos = src->tellg();
    if (stream_pos >= 0) {
      src->seekg(0, std::ios_base::end);
      if (src->fail()) {
        src->clear(src->rdstate() & ~std::ios_base::failbit);
      } else {
        std::ostream& dest = *dest_stream();
        errno = 0;
        dest.seekp(IntCast<std::streamoff>(start_pos()), std::ios_base::beg);
        if (ABSL_PREDICT_FALSE(dest.fail())) {
          FailOperation("ostream::seekp()");
        } else {
          supported = true;
        }
      }
    }
  }
  supports_read_mode_ =
      supported ? LazyBoolState::kTrue : LazyBoolState::kFalse;
  return supported;
}

inline bool OstreamWriterBase::WriteMode() {
  if (ABSL_PREDICT_TRUE(!read_mode_)) return true;
  read_mode_ = false;
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  std::ostream& dest = *dest_stream();
  errno = 0;
  dest.seekp(IntCast<std::streamoff>(start_pos()), std::ios_base::beg);
  if (ABSL_PREDICT_FALSE(dest.fail())) {
    return FailOperation("ostream::seekp()");
  }
  return true;
}

bool OstreamWriterBase::WriteInternal(absl::string_view src) {
  RIEGELI_ASSERT(!src.empty())
      << "Failed precondition of BufferedWriter::WriteInternal(): "
         "nothing to write";
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of BufferedWriter::WriteInternal(): " << status();
  if (ABSL_PREDICT_FALSE(!WriteMode())) return false;
  std::ostream& dest = *dest_stream();
  if (ABSL_PREDICT_FALSE(src.size() >
                         Position{std::numeric_limits<std::streamoff>::max()} -
                             start_pos())) {
    return FailOverflow();
  }
  errno = 0;
  do {
    const size_t length_to_write = UnsignedMin(
        src.size(), size_t{std::numeric_limits<std::streamsize>::max()});
    dest.write(src.data(), IntCast<std::streamsize>(length_to_write));
    if (ABSL_PREDICT_FALSE(dest.fail())) {
      return FailOperation("ostream::write()");
    }
    move_start_pos(length_to_write);
    src.remove_prefix(length_to_write);
  } while (!src.empty());
  return true;
}

bool OstreamWriterBase::FlushBehindBuffer(absl::string_view src,
                                          FlushType flush_type) {
  RIEGELI_ASSERT_EQ(start_to_limit(), 0u)
      << "Failed precondition of BufferedWriter::FlushBehindBuffer(): "
         "buffer not empty";
  if (ABSL_PREDICT_FALSE(!WriteMode())) return false;
  return BufferedWriter::FlushBehindBuffer(src, flush_type);
}

bool OstreamWriterBase::SeekBehindBuffer(Position new_pos) {
  RIEGELI_ASSERT_NE(new_pos, pos())
      << "Failed precondition of BufferedWriter::SeekBehindBuffer(): "
         "position unchanged, use Seek() instead";
  RIEGELI_ASSERT_EQ(start_to_limit(), 0u)
      << "Failed precondition of BufferedWriter::SeekBehindBuffer(): "
         "buffer not empty";
  if (ABSL_PREDICT_FALSE(!supports_random_access())) {
    // Delegate to base class version which fails, to avoid duplicating the
    // failure message here.
    return BufferedWriter::SeekBehindBuffer(new_pos);
  }
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  std::ostream& dest = *dest_stream();
  errno = 0;
  if (new_pos > start_pos()) {
    // Seeking forwards.
    dest.seekp(0, std::ios_base::end);
    if (ABSL_PREDICT_FALSE(dest.fail())) {
      return FailOperation("ostream::seekp()");
    }
    const std::streamoff stream_size = dest.tellp();
    if (ABSL_PREDICT_FALSE(stream_size < 0)) {
      return FailOperation("ostream::tellp()");
    }
    if (ABSL_PREDICT_FALSE(new_pos > IntCast<Position>(stream_size))) {
      // Stream ends.
      set_start_pos(IntCast<Position>(stream_size));
      return false;
    }
  }
  dest.seekp(IntCast<std::streamoff>(new_pos), std::ios_base::beg);
  if (ABSL_PREDICT_FALSE(dest.fail())) {
    return FailOperation("ostream::seekp()");
  }
  set_start_pos(new_pos);
  return true;
}

absl::optional<Position> OstreamWriterBase::SizeBehindBuffer() {
  RIEGELI_ASSERT_EQ(start_to_limit(), 0u)
      << "Failed precondition of BufferedWriter::SizeBehindBuffer(): "
         "buffer not empty";
  if (ABSL_PREDICT_FALSE(!supports_random_access())) {
    // Delegate to base class version which fails, to avoid duplicating the
    // failure message here.
    return BufferedWriter::SizeBehindBuffer();
  }
  if (ABSL_PREDICT_FALSE(!healthy())) return absl::nullopt;
  std::ostream& dest = *dest_stream();
  errno = 0;
  dest.seekp(0, std::ios_base::end);
  if (ABSL_PREDICT_FALSE(dest.fail())) {
    FailOperation("ostream::seekp()");
    return absl::nullopt;
  }
  const std::streamoff stream_size = dest.tellp();
  if (ABSL_PREDICT_FALSE(stream_size < 0)) {
    FailOperation("ostream::tellp()");
    return absl::nullopt;
  }
  dest.seekp(IntCast<std::streamoff>(start_pos()), std::ios_base::beg);
  if (ABSL_PREDICT_FALSE(dest.fail())) {
    FailOperation("ostream::seekp()");
    return absl::nullopt;
  }
  return IntCast<Position>(stream_size);
}

Reader* OstreamWriterBase::ReadModeBehindBuffer(Position initial_pos) {
  RIEGELI_ASSERT_EQ(start_to_limit(), 0u)
      << "Failed precondition of BufferedWriter::ReadModeBehindBuffer(): "
         "buffer not empty";
  if (ABSL_PREDICT_FALSE(!supports_read_mode())) {
    // Delegate to base class version which fails, to avoid duplicating the
    // failure message here.
    return BufferedWriter::ReadModeBehindBuffer(initial_pos);
  }
  if (ABSL_PREDICT_FALSE(!healthy())) return nullptr;
  std::istream& src = *src_stream();
  IstreamReader<>* const reader = associated_reader_.ResetReader(
      &src, IstreamReaderBase::Options().set_buffer_size(buffer_size()));
  read_mode_ = true;
  reader->Seek(initial_pos);
  return reader;
}

}  // namespace riegeli
