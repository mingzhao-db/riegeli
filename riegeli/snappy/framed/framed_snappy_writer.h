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

#ifndef RIEGELI_SNAPPY_FRAMED_FRAMED_SNAPPY_WRITER_H_
#define RIEGELI_SNAPPY_FRAMED_FRAMED_SNAPPY_WRITER_H_

#include <tuple>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/types/optional.h"
#include "riegeli/base/base.h"
#include "riegeli/base/buffer.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/pushable_writer.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

template <typename Src>
class FramedSnappyReader;

// Template parameter independent part of `FramedSnappyWriter`.
class FramedSnappyWriterBase : public PushableWriter {
 public:
  class Options {
   public:
    Options() noexcept {}

    // Expected uncompressed size, or `absl::nullopt` if unknown. This may
    // improve performance and memory usage.
    //
    // If the size hint turns out to not match reality, nothing breaks.
    //
    // Default: `absl::nullopt`.
    Options& set_size_hint(absl::optional<Position> size_hint) & {
      size_hint_ = size_hint;
      return *this;
    }
    Options&& set_size_hint(absl::optional<Position> size_hint) && {
      return std::move(set_size_hint(size_hint));
    }
    absl::optional<Position> size_hint() const { return size_hint_; }

   private:
    absl::optional<Position> size_hint_;
  };

  // Returns the compressed `Writer`. Unchanged by `Close()`.
  virtual Writer* dest_writer() = 0;
  virtual const Writer* dest_writer() const = 0;

  bool SupportsReadMode() override;

 protected:
  explicit FramedSnappyWriterBase(Closed) noexcept : PushableWriter(kClosed) {}

  explicit FramedSnappyWriterBase(absl::optional<Position> size_hint);

  FramedSnappyWriterBase(FramedSnappyWriterBase&& that) noexcept;
  FramedSnappyWriterBase& operator=(FramedSnappyWriterBase&& that) noexcept;

  void Reset(Closed);
  void Reset(absl::optional<Position> size_hint);
  void Initialize(Writer* dest);
  ABSL_ATTRIBUTE_COLD absl::Status AnnotateOverDest(absl::Status status);

  void Done() override;
  ABSL_ATTRIBUTE_COLD absl::Status AnnotateStatusImpl(
      absl::Status status) override;
  bool PushBehindScratch() override;
  bool FlushBehindScratch(FlushType flush_type);
  Reader* ReadModeBehindScratch(Position initial_pos) override;

 private:
  // Compresses buffered data, but unlike `PushSlow()`, does not ensure that a
  // buffer is allocated.
  //
  // Precondition: `healthy()`
  //
  // Postcondition: `start_to_cursor() == 0`
  bool PushInternal(Writer& dest);

  Position size_hint_ = 0;
  Position initial_compressed_pos_ = 0;
  // Buffered uncompressed data.
  Buffer uncompressed_;

  AssociatedReader<FramedSnappyReader<Reader*>> associated_reader_;

  // Invariants if scratch is not used:
  //   `start() == nullptr` or `start() == uncompressed_.data()`
  //   `start_to_limit() <= snappy::kBlockSize`
};

// A `Writer` which compresses data with framed Snappy format before passing it
// to another `Writer`:
// https://github.com/google/snappy/blob/master/framing_format.txt
//
// The `Dest` template parameter specifies the type of the object providing and
// possibly owning the compressed `Writer`. `Dest` must support
// `Dependency<Writer*, Dest>`, e.g. `Writer*` (not owned, default),
// `std::unique_ptr<Writer>` (owned), `ChainWriter<>` (owned).
//
// By relying on CTAD the template argument can be deduced as the value type of
// the first constructor argument. This requires C++17.
//
// The compressed `Writer` must not be accessed until the `FramedSnappyWriter`
// is closed or no longer used.
template <typename Dest = Writer*>
class FramedSnappyWriter : public FramedSnappyWriterBase {
 public:
  // Creates a closed `FramedSnappyWriter`.
  explicit FramedSnappyWriter(Closed) noexcept
      : FramedSnappyWriterBase(kClosed) {}

  // Will write to the compressed `Writer` provided by `dest`.
  explicit FramedSnappyWriter(const Dest& dest, Options options = Options());
  explicit FramedSnappyWriter(Dest&& dest, Options options = Options());

  // Will write to the compressed `Writer` provided by a `Dest` constructed from
  // elements of `dest_args`. This avoids constructing a temporary `Dest` and
  // moving from it.
  template <typename... DestArgs>
  explicit FramedSnappyWriter(std::tuple<DestArgs...> dest_args,
                              Options options = Options());

  FramedSnappyWriter(FramedSnappyWriter&& that) noexcept;
  FramedSnappyWriter& operator=(FramedSnappyWriter&& that) noexcept;

  // Makes `*this` equivalent to a newly constructed `FramedSnappyWriter`. This
  // avoids constructing a temporary `FramedSnappyWriter` and moving from it.
  void Reset(Closed);
  void Reset(const Dest& dest, Options options = Options());
  void Reset(Dest&& dest, Options options = Options());
  template <typename... DestArgs>
  void Reset(std::tuple<DestArgs...> dest_args, Options options = Options());

  // Returns the object providing and possibly owning the compressed `Writer`.
  // Unchanged by `Close()`.
  Dest& dest() { return dest_.manager(); }
  const Dest& dest() const { return dest_.manager(); }
  Writer* dest_writer() override { return dest_.get(); }
  const Writer* dest_writer() const override { return dest_.get(); }

 protected:
  void Done() override;
  bool FlushImpl(FlushType flush_type) override;

 private:
  // The object providing and possibly owning the compressed `Writer`.
  Dependency<Writer*, Dest> dest_;
};

// Support CTAD.
#if __cpp_deduction_guides
explicit FramedSnappyWriter(Closed)->FramedSnappyWriter<DeleteCtad<Closed>>;
template <typename Dest>
explicit FramedSnappyWriter(
    const Dest& dest,
    FramedSnappyWriterBase::Options options = FramedSnappyWriterBase::Options())
    -> FramedSnappyWriter<std::decay_t<Dest>>;
template <typename Dest>
explicit FramedSnappyWriter(
    Dest&& dest,
    FramedSnappyWriterBase::Options options = FramedSnappyWriterBase::Options())
    -> FramedSnappyWriter<std::decay_t<Dest>>;
template <typename... DestArgs>
explicit FramedSnappyWriter(
    std::tuple<DestArgs...> dest_args,
    FramedSnappyWriterBase::Options options = FramedSnappyWriterBase::Options())
    -> FramedSnappyWriter<DeleteCtad<std::tuple<DestArgs...>>>;
#endif

// Implementation details follow.

inline FramedSnappyWriterBase::FramedSnappyWriterBase(
    absl::optional<Position> size_hint)
    : size_hint_(size_hint.value_or(0)) {}

inline FramedSnappyWriterBase::FramedSnappyWriterBase(
    FramedSnappyWriterBase&& that) noexcept
    : PushableWriter(std::move(that)),
      // Using `that` after it was moved is correct because only the base class
      // part was moved.
      size_hint_(that.size_hint_),
      initial_compressed_pos_(that.initial_compressed_pos_),
      uncompressed_(std::move(that.uncompressed_)),
      associated_reader_(std::move(that.associated_reader_)) {}

inline FramedSnappyWriterBase& FramedSnappyWriterBase::operator=(
    FramedSnappyWriterBase&& that) noexcept {
  PushableWriter::operator=(std::move(that));
  // Using `that` after it was moved is correct because only the base class part
  // was moved.
  size_hint_ = that.size_hint_;
  initial_compressed_pos_ = that.initial_compressed_pos_;
  uncompressed_ = std::move(that.uncompressed_);
  associated_reader_ = std::move(that.associated_reader_);
  return *this;
}

inline void FramedSnappyWriterBase::Reset(Closed) {
  PushableWriter::Reset(kClosed);
  size_hint_ = 0;
  initial_compressed_pos_ = 0;
  uncompressed_ = Buffer();
  associated_reader_.Reset();
}

inline void FramedSnappyWriterBase::Reset(absl::optional<Position> size_hint) {
  PushableWriter::Reset();
  size_hint_ = size_hint.value_or(0);
  initial_compressed_pos_ = 0;
  associated_reader_.Reset();
}

template <typename Dest>
inline FramedSnappyWriter<Dest>::FramedSnappyWriter(const Dest& dest,
                                                    Options options)
    : FramedSnappyWriterBase(options.size_hint()), dest_(dest) {
  Initialize(dest_.get());
}

template <typename Dest>
inline FramedSnappyWriter<Dest>::FramedSnappyWriter(Dest&& dest,
                                                    Options options)
    : FramedSnappyWriterBase(options.size_hint()), dest_(std::move(dest)) {
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline FramedSnappyWriter<Dest>::FramedSnappyWriter(
    std::tuple<DestArgs...> dest_args, Options options)
    : FramedSnappyWriterBase(options.size_hint()), dest_(std::move(dest_args)) {
  Initialize(dest_.get());
}

template <typename Dest>
inline FramedSnappyWriter<Dest>::FramedSnappyWriter(
    FramedSnappyWriter&& that) noexcept
    : FramedSnappyWriterBase(std::move(that)),
      // Using `that` after it was moved is correct because only the base class
      // part was moved.
      dest_(std::move(that.dest_)) {}

template <typename Dest>
inline FramedSnappyWriter<Dest>& FramedSnappyWriter<Dest>::operator=(
    FramedSnappyWriter&& that) noexcept {
  FramedSnappyWriterBase::operator=(std::move(that));
  // Using `that` after it was moved is correct because only the base class part
  // was moved.
  dest_ = std::move(that.dest_);
  return *this;
}

template <typename Dest>
inline void FramedSnappyWriter<Dest>::Reset(Closed) {
  FramedSnappyWriterBase::Reset(kClosed);
  dest_.Reset();
}

template <typename Dest>
inline void FramedSnappyWriter<Dest>::Reset(const Dest& dest, Options options) {
  FramedSnappyWriterBase::Reset(options.size_hint());
  dest_.Reset(dest);
  Initialize(dest_.get());
}

template <typename Dest>
inline void FramedSnappyWriter<Dest>::Reset(Dest&& dest, Options options) {
  FramedSnappyWriterBase::Reset(options.size_hint());
  dest_.Reset(std::move(dest));
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline void FramedSnappyWriter<Dest>::Reset(std::tuple<DestArgs...> dest_args,
                                            Options options) {
  FramedSnappyWriterBase::Reset(options.size_hint());
  dest_.Reset(std::move(dest_args));
  Initialize(dest_.get());
}

template <typename Dest>
void FramedSnappyWriter<Dest>::Done() {
  FramedSnappyWriterBase::Done();
  if (dest_.is_owning()) {
    if (ABSL_PREDICT_FALSE(!dest_->Close())) {
      FailWithoutAnnotation(AnnotateOverDest(dest_->status()));
    }
  }
}

template <typename Dest>
bool FramedSnappyWriter<Dest>::FlushImpl(FlushType flush_type) {
  if (ABSL_PREDICT_FALSE(!FramedSnappyWriterBase::FlushImpl(flush_type))) {
    return false;
  }
  if (flush_type != FlushType::kFromObject || dest_.is_owning()) {
    if (ABSL_PREDICT_FALSE(!dest_->Flush(flush_type))) {
      FailWithoutAnnotation(AnnotateOverDest(dest_->status()));
    }
  }
  return true;
}

}  // namespace riegeli

#endif  // RIEGELI_SNAPPY_FRAMED_FRAMED_SNAPPY_WRITER_H_
