#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "BitmapHelpers.h"

#pragma pack(push, 1)
struct BmpHeader {
  struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
  } fileHeader;
  struct {
    uint32_t biSize;
    int32_t biWidth;
    int32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t biXPelsPerMeter;
    int32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
  } infoHeader;
  struct RgbQuad {
    uint8_t rgbBlue;
    uint8_t rgbGreen;
    uint8_t rgbRed;
    uint8_t rgbReserved;
  };
  RgbQuad colors[2];
};
#pragma pack(pop)

enum class BmpReaderError : uint8_t {
  Ok = 0,
  FileInvalid,
  SeekStartFailed,

  NotBMP,
  DIBTooSmall,

  BadPlanes,
  UnsupportedBpp,
  UnsupportedCompression,

  BadDimensions,
  ImageTooLarge,
  PaletteTooLarge,

  SeekPixelDataFailed,
  BufferTooSmall,
  OomRowBuffer,
  ShortReadRow,
};

class Bitmap {
 public:
  static const char* errorToString(BmpReaderError err);

  // File-backed bitmap. The caller owns `file` and must keep it open for the
  // lifetime of any read on this Bitmap. Used everywhere we stream a thumb
  // straight off the SD card.
  explicit Bitmap(FsFile& file, bool dithering = false) : fileBacking(&file), dithering(dithering) {}
  // Memory-backed bitmap. `data` must hold a full, valid BMP file (header +
  // pixel rows) and stay alive for the lifetime of this Bitmap. Used by the
  // Flow home carousel: thumbnails are slurped into a RAM cache at home-enter
  // so the per-render path doesn't reopen the SD file for every scroll.
  Bitmap(const uint8_t* data, size_t size, bool dithering = false)
      : fileBacking(nullptr), memBacking(data), memSize(size), dithering(dithering) {}
  ~Bitmap();
  BmpReaderError parseHeaders();
  BmpReaderError readNextRow(uint8_t* data, uint8_t* rowBuffer) const;
  BmpReaderError rewindToData() const;
  int getWidth() const { return width; }
  int getHeight() const { return height; }
  bool isTopDown() const { return topDown; }
  bool hasGreyscale() const { return bpp > 1; }
  int getRowBytes() const { return rowBytes; }
  bool is1Bit() const { return bpp == 1; }
  uint16_t getBpp() const { return bpp; }

 private:
  // Backing-store-agnostic I/O primitives. Each branches on whether the
  // bitmap was constructed against a file or a memory buffer.
  bool isOpen() const;
  bool seekAbsolute(size_t pos) const;
  bool seekRelative(int64_t offset) const;
  int readBlock(void* buf, size_t count) const;
  uint8_t readByte() const;
  uint16_t readLE16() const;
  uint32_t readLE32() const;

  FsFile* fileBacking = nullptr;
  const uint8_t* memBacking = nullptr;
  size_t memSize = 0;
  mutable size_t memPos = 0;
  bool dithering = false;
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t bfOffBits = 0;
  uint16_t bpp = 0;
  uint32_t colorsUsed = 0;
  bool nativePalette = false;  // true if all palette entries map to native gray levels
  int rowBytes = 0;
  uint8_t paletteLum[256] = {};

  // Dithering state (mutable for const methods)
  mutable int16_t* errorCurRow = nullptr;
  mutable int16_t* errorNextRow = nullptr;
  mutable int prevRowY = -1;  // Track row progression for error propagation

  mutable AtkinsonDitherer* atkinsonDitherer = nullptr;
  mutable FloydSteinbergDitherer* fsDitherer = nullptr;
};
