#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace Envoy {
namespace Config {

class XdsSymbolTable;

using XdsSymbol = uint32_t;

/**
 * An immutable, lightweight value-type view of an encoded xDS resource name.
 * Encoded as a contiguous sequence of variable-length (`LEB128`) symbol IDs.
 */
class XdsResourceName {
public:
  constexpr XdsResourceName() : data_(nullptr), size_(0) {}
  constexpr XdsResourceName(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  absl::Span<const uint8_t> span() const { return {data_, size_}; }

  bool operator==(const XdsResourceName& other) const {
    if (size_ != other.size_) {
      return false;
    }
    if (data_ == other.data_) {
      return true;
    }
    return size_ == 0 || std::memcmp(data_, other.data_, size_) == 0;
  }

  bool operator!=(const XdsResourceName& other) const { return !(*this == other); }

  bool operator<(const XdsResourceName& other) const {
    const size_t min_len = std::min(size_, other.size_);
    if (min_len > 0) {
      const int cmp = std::memcmp(data_, other.data_, min_len);
      if (cmp != 0) {
        return cmp < 0;
      }
    }
    return size_ < other.size_;
  }

  template <typename H> friend H AbslHashValue(H h, const XdsResourceName& name) {
    return H::combine_contiguous(std::move(h), name.data_, name.size_);
  }

private:
  const uint8_t* data_;
  size_t size_;
};

/**
 * RAII owning storage for an encoded xDS resource name.
 * Manages symbol ref-counts in the backing XdsSymbolTable.
 */
class XdsResourceNameStorage {
public:
  XdsResourceNameStorage(std::shared_ptr<XdsSymbolTable> table, absl::string_view name);
  XdsResourceNameStorage(std::shared_ptr<XdsSymbolTable> table, std::unique_ptr<uint8_t[]> data,
                         uint32_t size);
  ~XdsResourceNameStorage();

  XdsResourceNameStorage(XdsResourceNameStorage&& other) noexcept;
  XdsResourceNameStorage& operator=(XdsResourceNameStorage&& other) noexcept;

  XdsResourceNameStorage(const XdsResourceNameStorage& other);
  XdsResourceNameStorage& operator=(const XdsResourceNameStorage& other);

  XdsResourceName resourceName() const { return XdsResourceName(data_.get(), size_); }

  size_t size() const { return size_; }

private:
  std::shared_ptr<XdsSymbolTable> table_;
  std::unique_ptr<uint8_t[]> data_;
  uint32_t size_{0};
};

/**
 * XdsSymbolTable interns sub-tokens and delimiters of xDS resource names.
 * Designed for the Envoy main thread where xDS configuration updates run.
 */
class XdsSymbolTable : public std::enable_shared_from_this<XdsSymbolTable> {
public:
  XdsSymbolTable();
  ~XdsSymbolTable() = default;

  // Encodes a resource name into owning RAII storage.
  XdsResourceNameStorage encode(absl::string_view name);

  // Decodes an encoded XdsResourceName back into its original string with 100% fidelity.
  std::string toString(const XdsResourceName& name) const;

  // Decodes from a list of symbol IDs.
  std::string toString(absl::Span<const XdsSymbol> symbols) const;

  // Number of unique symbols allocated in the table.
  size_t numSymbols() const { return decode_table_.size(); }

  // Number of currently active (referenced) symbols.
  size_t numActiveSymbols() const { return encode_map_.size(); }

  // Increment/decrement symbol ref counts.
  void addRef(absl::Span<const XdsSymbol> symbols);
  void releaseRef(absl::Span<const XdsSymbol> symbols);

  // Well-known reserved symbols for common single delimiters.
  static constexpr XdsSymbol kSlash = 1;      // '/'
  static constexpr XdsSymbol kDot = 2;        // '.'
  static constexpr XdsSymbol kColon = 3;      // ':'
  static constexpr XdsSymbol kQuestion = 4;   // '?'
  static constexpr XdsSymbol kAmpersand = 5;  // '&'
  static constexpr XdsSymbol kEquals = 6;     // '='
  static constexpr XdsSymbol kUnderscore = 7; // '_'
  static constexpr XdsSymbol kDash = 8;       // '-'
  static constexpr XdsSymbol kSchemeSep = 9;  // '://'
  static constexpr XdsSymbol kFirstDynamicSymbol = 10;

private:
  friend class XdsResourceNameStorage;

  // Tokenizes name into alternating tokens and delimiters.
  std::vector<XdsSymbol> tokenizeToSymbols(absl::string_view name);

  // Encodes a sequence of symbols into a compact byte vector using `LEB128`.
  static std::vector<uint8_t> encodeSymbolsToBytes(absl::Span<const XdsSymbol> symbols);

  // Decodes symbols from encoded bytes.
  static std::vector<XdsSymbol> decodeBytesToSymbols(absl::Span<const uint8_t> bytes);

  XdsSymbol toSymbol(absl::string_view token);

  absl::flat_hash_map<std::string, XdsSymbol> encode_map_;
  std::vector<std::string> decode_table_;
  std::vector<uint32_t> ref_counts_;
  std::vector<XdsSymbol> free_list_;
};

using XdsSymbolTablePtr = std::shared_ptr<XdsSymbolTable>;

} // namespace Config
} // namespace Envoy
