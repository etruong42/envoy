#include "source/common/config/xds_symbol_table.h"

#include <algorithm>
#include <utility>

namespace Envoy {
namespace Config {

XdsSymbolTable::XdsSymbolTable() {
  // Pre-seed reserved symbols 0..9.
  decode_table_.push_back("");    // 0: unused
  decode_table_.push_back("/");   // 1: kSlash
  decode_table_.push_back(".");   // 2: kDot
  decode_table_.push_back(":");   // 3: kColon
  decode_table_.push_back("?");   // 4: kQuestion
  decode_table_.push_back("&");   // 5: kAmpersand
  decode_table_.push_back("=");   // 6: kEquals
  decode_table_.push_back("_");   // 7: kUnderscore
  decode_table_.push_back("-");   // 8: kDash
  decode_table_.push_back("://"); // 9: kSchemeSep

  ref_counts_.assign(10, 1); // Reserved symbols have immortal ref count.

  for (size_t i = 1; i < decode_table_.size(); ++i) {
    encode_map_[decode_table_[i]] = static_cast<XdsSymbol>(i);
  }
}

XdsSymbol XdsSymbolTable::toSymbol(absl::string_view token) {
  if (token.empty()) {
    return 0;
  }

  auto it = encode_map_.find(token);
  if (it != encode_map_.end()) {
    return it->second;
  }

  XdsSymbol sym;
  if (!free_list_.empty()) {
    sym = free_list_.back();
    free_list_.pop_back();
    decode_table_[sym] = std::string(token);
    ref_counts_[sym] = 0;
  } else {
    sym = static_cast<XdsSymbol>(decode_table_.size());
    decode_table_.emplace_back(token);
    ref_counts_.push_back(0);
  }

  encode_map_[decode_table_[sym]] = sym;
  return sym;
}

std::vector<XdsSymbol> XdsSymbolTable::tokenizeToSymbols(absl::string_view name) {
  std::vector<XdsSymbol> symbols;
  symbols.reserve(name.size() / 4 + 4);

  size_t i = 0;
  size_t token_start = 0;

  auto flush_token = [&](size_t end) {
    if (end > token_start) {
      absl::string_view token = name.substr(token_start, end - token_start);
      symbols.push_back(toSymbol(token));
    }
  };

  while (i < name.size()) {
    // Check for "://"
    if (i + 2 < name.size() && name[i] == ':' && name[i + 1] == '/' && name[i + 2] == '/') {
      flush_token(i);
      symbols.push_back(kSchemeSep);
      i += 3;
      token_start = i;
      continue;
    }

    char c = name[i];
    XdsSymbol delimiter_sym = 0;
    switch (c) {
    case '/':
      delimiter_sym = kSlash;
      break;
    case '.':
      delimiter_sym = kDot;
      break;
    case ':':
      delimiter_sym = kColon;
      break;
    case '?':
      delimiter_sym = kQuestion;
      break;
    case '&':
      delimiter_sym = kAmpersand;
      break;
    case '=':
      delimiter_sym = kEquals;
      break;
    case '_':
      delimiter_sym = kUnderscore;
      break;
    case '-':
      delimiter_sym = kDash;
      break;
    default:
      delimiter_sym = 0;
      break;
    }

    if (delimiter_sym != 0) {
      flush_token(i);
      symbols.push_back(delimiter_sym);
      ++i;
      token_start = i;
    } else {
      ++i;
    }
  }

  flush_token(name.size());
  return symbols;
}

std::vector<uint8_t> XdsSymbolTable::encodeSymbolsToBytes(absl::Span<const XdsSymbol> symbols) {
  std::vector<uint8_t> bytes;
  bytes.reserve(symbols.size() * 2);
  for (XdsSymbol sym : symbols) {
    while (sym >= 128) {
      bytes.push_back(static_cast<uint8_t>((sym & 0x7F) | 0x80));
      sym >>= 7;
    }
    bytes.push_back(static_cast<uint8_t>(sym & 0x7F));
  }
  return bytes;
}

std::vector<XdsSymbol> XdsSymbolTable::decodeBytesToSymbols(absl::Span<const uint8_t> bytes) {
  std::vector<XdsSymbol> symbols;
  size_t i = 0;
  while (i < bytes.size()) {
    XdsSymbol sym = 0;
    int shift = 0;
    while (i < bytes.size()) {
      uint8_t b = bytes[i++];
      sym |= static_cast<XdsSymbol>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) {
        break;
      }
      shift += 7;
    }
    symbols.push_back(sym);
  }
  return symbols;
}

void XdsSymbolTable::addRef(absl::Span<const XdsSymbol> symbols) {
  for (XdsSymbol s : symbols) {
    if (s < ref_counts_.size()) {
      ++ref_counts_[s];
    }
  }
}

void XdsSymbolTable::releaseRef(absl::Span<const XdsSymbol> symbols) {
  for (XdsSymbol s : symbols) {
    if (s >= kFirstDynamicSymbol && s < ref_counts_.size()) {
      if (--ref_counts_[s] == 0) {
        encode_map_.erase(decode_table_[s]);
        decode_table_[s].clear();
        free_list_.push_back(s);
      }
    }
  }
}

XdsResourceNameStorage XdsSymbolTable::encode(absl::string_view name) {
  std::vector<XdsSymbol> symbols = tokenizeToSymbols(name);
  addRef(symbols);
  std::vector<uint8_t> bytes = encodeSymbolsToBytes(symbols);
  std::unique_ptr<uint8_t[]> data;
  if (!bytes.empty()) {
    data = std::make_unique<uint8_t[]>(bytes.size());
    std::memcpy(data.get(), bytes.data(), bytes.size()); // NOLINT(safe-memcpy)
  }
  return XdsResourceNameStorage(shared_from_this(), std::move(data),
                                static_cast<uint32_t>(bytes.size()));
}

std::string XdsSymbolTable::toString(const XdsResourceName& name) const {
  const auto symbols = decodeBytesToSymbols(name.span());
  return toString(symbols);
}

std::string XdsSymbolTable::toString(absl::Span<const XdsSymbol> symbols) const {
  size_t total_len = 0;
  for (XdsSymbol s : symbols) {
    if (s < decode_table_.size()) {
      total_len += decode_table_[s].size();
    }
  }
  std::string result;
  result.reserve(total_len);
  for (XdsSymbol s : symbols) {
    if (s < decode_table_.size()) {
      result.append(decode_table_[s]);
    }
  }
  return result;
}

// ============================================================================
// XdsResourceNameStorage implementation
// ============================================================================

XdsResourceNameStorage::XdsResourceNameStorage(std::shared_ptr<XdsSymbolTable> table,
                                               absl::string_view name)
    : table_(table) {
  if (table_ != nullptr) {
    auto storage = table_->encode(name);
    data_ = std::move(storage.data_);
    size_ = storage.size_;
  }
}

XdsResourceNameStorage::XdsResourceNameStorage(std::shared_ptr<XdsSymbolTable> table,
                                               std::unique_ptr<uint8_t[]> data, uint32_t size)
    : table_(std::move(table)), data_(std::move(data)), size_(size) {}

XdsResourceNameStorage::~XdsResourceNameStorage() {
  if (table_ != nullptr && data_ != nullptr && size_ > 0) {
    const auto symbols = table_->decodeBytesToSymbols({data_.get(), size_});
    table_->releaseRef(symbols);
  }
}

XdsResourceNameStorage::XdsResourceNameStorage(XdsResourceNameStorage&& other) noexcept
    : table_(std::move(other.table_)), data_(std::move(other.data_)), size_(other.size_) {
  other.size_ = 0;
}

XdsResourceNameStorage& XdsResourceNameStorage::operator=(XdsResourceNameStorage&& other) noexcept {
  if (this != &other) {
    if (table_ != nullptr && data_ != nullptr && size_ > 0) {
      const auto symbols = table_->decodeBytesToSymbols({data_.get(), size_});
      table_->releaseRef(symbols);
    }
    table_ = std::move(other.table_);
    data_ = std::move(other.data_);
    size_ = other.size_;
    other.size_ = 0;
  }
  return *this;
}

XdsResourceNameStorage::XdsResourceNameStorage(const XdsResourceNameStorage& other)
    : table_(other.table_), size_(other.size_) {
  if (other.data_ != nullptr && other.size_ > 0) {
    data_ = std::make_unique<uint8_t[]>(size_);
    std::memcpy(data_.get(), other.data_.get(), size_); // NOLINT(safe-memcpy)
    if (table_ != nullptr) {
      const auto symbols = table_->decodeBytesToSymbols({data_.get(), size_});
      table_->addRef(symbols);
    }
  }
}

XdsResourceNameStorage& XdsResourceNameStorage::operator=(const XdsResourceNameStorage& other) {
  if (this != &other) {
    if (table_ != nullptr && data_ != nullptr && size_ > 0) {
      const auto symbols = table_->decodeBytesToSymbols({data_.get(), size_});
      table_->releaseRef(symbols);
    }
    table_ = other.table_;
    size_ = other.size_;
    if (other.data_ != nullptr && other.size_ > 0) {
      data_ = std::make_unique<uint8_t[]>(size_);
      std::memcpy(data_.get(), other.data_.get(), size_); // NOLINT(safe-memcpy)
      if (table_ != nullptr) {
        const auto symbols = table_->decodeBytesToSymbols({data_.get(), size_});
        table_->addRef(symbols);
      }
    } else {
      data_.reset();
    }
  }
  return *this;
}

} // namespace Config
} // namespace Envoy
