#include "png_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fpshud {

namespace {

uint32_t compute_crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) noexcept {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
    }
  }
  return ~crc;
}

uint32_t compute_adler32(const uint8_t* data, size_t len) noexcept {
  uint32_t s1 = 1;
  uint32_t s2 = 0;
  constexpr uint32_t kMod = 65521u;
  size_t i = 0;
  while (i < len) {
    size_t chunk = std::min<size_t>(len - i, 5552);
    for (size_t j = 0; j < chunk; ++j) {
      s1 += data[i + j];
      s2 += s1;
    }
    s1 %= kMod;
    s2 %= kMod;
    i += chunk;
  }
  return (s2 << 16) | s1;
}

void push_be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void write_png_chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* payload,
                     uint32_t len) {
  push_be32(out, len);
  size_t type_pos = out.size();
  out.push_back(static_cast<uint8_t>(type[0]));
  out.push_back(static_cast<uint8_t>(type[1]));
  out.push_back(static_cast<uint8_t>(type[2]));
  out.push_back(static_cast<uint8_t>(type[3]));
  if (payload && len > 0) {
    out.insert(out.end(), payload, payload + len);
  }
  uint32_t crc = compute_crc32(out.data() + type_pos, 4 + len);
  push_be32(out, crc);
}

struct BitWriter {
  std::vector<uint8_t>& out;
  uint32_t bit_buf = 0;
  int bit_count = 0;

  explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}

  // Writes LSB-first bits (for Deflate header fields, extra bits)
  void write_bits(uint32_t val, int nbits) {
    bit_buf |= (val << bit_count);
    bit_count += nbits;
    while (bit_count >= 8) {
      out.push_back(static_cast<uint8_t>(bit_buf & 0xFF));
      bit_buf >>= 8;
      bit_count -= 8;
    }
  }

  // Writes MSB-first Huffman code (RFC 1951 Section 3.1.1)
  void write_huff(uint32_t code, int nbits) {
    uint32_t rev = 0;
    for (int i = 0; i < nbits; ++i) {
      rev = (rev << 1) | ((code >> i) & 1u);
    }
    write_bits(rev, nbits);
  }

  void flush() {
    if (bit_count > 0) {
      out.push_back(static_cast<uint8_t>(bit_buf & 0xFF));
      bit_buf = 0;
      bit_count = 0;
    }
  }
};

void emit_fixed_lit(BitWriter& bw, uint16_t sym) {
  if (sym <= 143) {
    bw.write_huff(0x30 + sym, 8);
  } else if (sym <= 255) {
    bw.write_huff(0x190 + (sym - 144), 9);
  } else if (sym <= 279) {
    bw.write_huff(sym - 256, 7);
  } else {
    bw.write_huff(0xC0 + (sym - 280), 8);
  }
}

void emit_fixed_match(BitWriter& bw, uint32_t len, uint32_t dist) {
  static constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                            15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                            67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr uint16_t kDistBase[30] = {
      1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

  int len_idx = 28;
  for (int i = 0; i < 28; ++i) {
    if (len < kLenBase[i + 1]) {
      len_idx = i;
      break;
    }
  }
  emit_fixed_lit(bw, static_cast<uint16_t>(257 + len_idx));
  if (kLenExtra[len_idx] > 0) {
    bw.write_bits(len - kLenBase[len_idx], kLenExtra[len_idx]);
  }

  int dist_idx = 29;
  for (int i = 0; i < 29; ++i) {
    if (dist < kDistBase[i + 1]) {
      dist_idx = i;
      break;
    }
  }
  bw.write_huff(static_cast<uint32_t>(dist_idx), 5);
  if (kDistExtra[dist_idx] > 0) {
    bw.write_bits(dist - kDistBase[dist_idx], kDistExtra[dist_idx]);
  }
}

void compress_zlib_fixed(const std::vector<uint8_t>& raw, std::vector<uint8_t>& zlib_out) {
  zlib_out.reserve(raw.size() / 2 + 128);
  // Zlib header (CMF=0x78: Deflate 32K window, FLG=0x01: check bits for 0x7801 % 31 == 0)
  zlib_out.push_back(0x78);
  zlib_out.push_back(0x01);

  BitWriter bw(zlib_out);
  // Single final block (BFINAL=1, BTYPE=01 fixed Huffman)
  bw.write_bits(1, 1);
  bw.write_bits(1, 2);

  constexpr size_t kHashSize = 4096;
  std::vector<int32_t> head(kHashSize, -1);

  const uint8_t* data = raw.data();
  const size_t n = raw.size();
  size_t pos = 0;

  auto hash3 = [](const uint8_t* p) noexcept -> size_t {
    uint32_t v = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    return ((v * 2654435761u) >> 20) & (kHashSize - 1);
  };

  while (pos < n) {
    uint32_t best_len = 0;
    uint32_t best_dist = 0;

    if (pos + 3 <= n) {
      size_t h = hash3(data + pos);
      int32_t cand = head[h];
      head[h] = static_cast<int32_t>(pos);

      if (cand >= 0 && (pos - static_cast<size_t>(cand)) <= 32768) {
        size_t dist = pos - static_cast<size_t>(cand);
        size_t max_len = std::min<size_t>(258, n - pos);
        size_t l = 0;
        while (l < max_len && data[cand + l] == data[pos + l]) {
          ++l;
        }
        if (l >= 3) {
          best_len = static_cast<uint32_t>(l);
          best_dist = static_cast<uint32_t>(dist);
        }
      }
      // Also check distance 1 and 3 (byte/pixel RLE after Sub filter)
      for (size_t rle_d : {size_t(1), size_t(3)}) {
        if (pos >= rle_d) {
          size_t max_len = std::min<size_t>(258, n - pos);
          size_t l = 0;
          while (l < max_len && data[pos - rle_d + l] == data[pos + l]) {
            ++l;
          }
          if (l >= 3 && l > best_len) {
            best_len = static_cast<uint32_t>(l);
            best_dist = static_cast<uint32_t>(rle_d);
          }
        }
      }
    }

    if (best_len >= 3) {
      emit_fixed_match(bw, best_len, best_dist);
      for (uint32_t k = 1; k < best_len; ++k) {
        if (pos + k + 3 <= n) {
          head[hash3(data + pos + k)] = static_cast<int32_t>(pos + k);
        }
      }
      pos += best_len;
    } else {
      emit_fixed_lit(bw, data[pos]);
      ++pos;
    }
  }

  // End-of-block symbol 256
  emit_fixed_lit(bw, 256);
  bw.flush();

  uint32_t adler = compute_adler32(raw.data(), raw.size());
  push_be32(zlib_out, adler);
}

}  // namespace

bool write_rgba_to_png(const std::string& path, const uint8_t* pixels, uint32_t width,
                       uint32_t height, uint32_t format, bool flip_y) {
  if (path.empty() || !pixels || width == 0 || height == 0 || width > 16384 || height > 16384) {
    return false;
  }

  const size_t row_rgb_bytes = static_cast<size_t>(width) * 3;
  std::vector<uint8_t> filtered((row_rgb_bytes + 1) * height);
  std::vector<uint8_t> cur_row(row_rgb_bytes);

  for (uint32_t y = 0; y < height; ++y) {
    uint32_t src_y = flip_y ? (height - 1 - y) : y;
    const uint8_t* src_row = pixels + static_cast<size_t>(src_y) * width * 4;

    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* px = src_row + static_cast<size_t>(x) * 4;
      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;
      if (format == 0) {  // BGRA
        b = px[0];
        g = px[1];
        r = px[2];
      } else if (format == 1) {  // RGBA
        r = px[0];
        g = px[1];
        b = px[2];
      } else if (format == 2) {  // X2BGR10LE
        uint32_t w = static_cast<uint32_t>(px[0]) | (static_cast<uint32_t>(px[1]) << 8) |
                     (static_cast<uint32_t>(px[2]) << 16) | (static_cast<uint32_t>(px[3]) << 24);
        r = static_cast<uint8_t>((w & 0x3FFu) >> 2);
        g = static_cast<uint8_t>(((w >> 10) & 0x3FFu) >> 2);
        b = static_cast<uint8_t>(((w >> 20) & 0x3FFu) >> 2);
      } else {  // X2RGB10LE (3)
        uint32_t w = static_cast<uint32_t>(px[0]) | (static_cast<uint32_t>(px[1]) << 8) |
                     (static_cast<uint32_t>(px[2]) << 16) | (static_cast<uint32_t>(px[3]) << 24);
        b = static_cast<uint8_t>((w & 0x3FFu) >> 2);
        g = static_cast<uint8_t>(((w >> 10) & 0x3FFu) >> 2);
        r = static_cast<uint8_t>(((w >> 20) & 0x3FFu) >> 2);
      }
      cur_row[x * 3 + 0] = r;
      cur_row[x * 3 + 1] = g;
      cur_row[x * 3 + 2] = b;
    }

    uint8_t* dst = filtered.data() + static_cast<size_t>(y) * (row_rgb_bytes + 1);
    dst[0] = 1;  // PNG Filter Type 1 (Sub)
    for (size_t i = 0; i < row_rgb_bytes; ++i) {
      uint8_t left = (i >= 3) ? cur_row[i - 3] : 0;
      dst[1 + i] = static_cast<uint8_t>(cur_row[i] - left);
    }
  }

  std::vector<uint8_t> idat_payload;
  compress_zlib_fixed(filtered, idat_payload);

  std::vector<uint8_t> png;
  png.reserve(idat_payload.size() + 64);

  const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  png.insert(png.end(), kPngSig, kPngSig + 8);

  uint8_t ihdr[13] = {0};
  ihdr[0] = static_cast<uint8_t>((width >> 24) & 0xFF);
  ihdr[1] = static_cast<uint8_t>((width >> 16) & 0xFF);
  ihdr[2] = static_cast<uint8_t>((width >> 8) & 0xFF);
  ihdr[3] = static_cast<uint8_t>(width & 0xFF);
  ihdr[4] = static_cast<uint8_t>((height >> 24) & 0xFF);
  ihdr[5] = static_cast<uint8_t>((height >> 16) & 0xFF);
  ihdr[6] = static_cast<uint8_t>((height >> 8) & 0xFF);
  ihdr[7] = static_cast<uint8_t>(height & 0xFF);
  ihdr[8] = 8;   // 8-bit depth
  ihdr[9] = 2;   // Color type 2 = Truecolor RGB
  ihdr[10] = 0;  // Compression method 0 (Deflate)
  ihdr[11] = 0;  // Filter method 0
  ihdr[12] = 0;  // Interlace method 0 (None)

  write_png_chunk(png, "IHDR", ihdr, sizeof(ihdr));
  write_png_chunk(png, "IDAT", idat_payload.data(), static_cast<uint32_t>(idat_payload.size()));
  write_png_chunk(png, "IEND", nullptr, 0);

  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    return false;
  }
  size_t written = std::fwrite(png.data(), 1, png.size(), fp);
  std::fclose(fp);
  return written == png.size();
}

}  // namespace fpshud
