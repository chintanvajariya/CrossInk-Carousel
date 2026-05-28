#include "HomeActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "../reader/BookReadingStats.h"
#include "../reader/BookStatsActivity.h"
#include "AppVersion.h"
#include "BookmarkStore.h"
#include "BookmarksHomeActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/chart.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t TXT_CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t TXT_CACHE_VERSION = 2;

// Home-menu plumbing for non-Flow themes. Flow keeps its bespoke 6-item
// horizontal icon strip (rendered inline in HomeActivity::render); every
// other theme defers to GUI.drawButtonMenu with a dynamic list mirroring
// upstream CrossInk's buildHomeMenuItems pattern: items that depend on user
// state (OPDS servers configured, reading stats present, bookmarks saved)
// appear conditionally.
enum class HomeMenuAction {
  BrowseFiles,
  RecentBooks,
  OpdsBrowser,
  ReadingStats,
  Bookmarks,
  FileTransfer,
  Settings,
};

struct HomeMenuItem {
  const char* label;
  UIIcon icon;
  HomeMenuAction action;
};

std::vector<HomeMenuItem> buildHomeMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {
  std::vector<HomeMenuItem> items = {
      {tr(STR_BROWSE_FILES), Folder, HomeMenuAction::BrowseFiles},
      {tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks},
  };
  if (hasOpdsServers) {
    items.push_back({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasReadingStats) {
    items.push_back({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }
  if (hasBookmarks) {
    items.push_back({tr(STR_BOOKMARKS), BookmarkIcon, HomeMenuAction::Bookmarks});
  }
  items.push_back({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  items.push_back({tr(STR_SETTINGS_TITLE), Settings, HomeMenuAction::Settings});
  return items;
}

bool isFlowTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_FLOW;
}

bool isMinimalTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::MINIMAL;
}

// Minimal home overlay menu — order/contents differ slightly from
// buildHomeMenuItems: Settings is excluded (mapped to a front-button hint
// slot instead), Bookmarks reorders before Reading Stats.
std::vector<HomeMenuItem> buildMinimalMenuItems(bool hasOpdsServers, bool hasReadingStats, bool hasBookmarks) {
  std::vector<HomeMenuItem> items = {
      {tr(STR_MENU_RECENT_BOOKS), Recent, HomeMenuAction::RecentBooks},
  };
  if (hasOpdsServers) {
    items.push_back({tr(STR_OPDS_BROWSER), Library, HomeMenuAction::OpdsBrowser});
  }
  if (hasBookmarks) {
    items.push_back({tr(STR_BOOKMARKS), BookmarkIcon, HomeMenuAction::Bookmarks});
  }
  if (hasReadingStats) {
    items.push_back({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }
  items.push_back({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  return items;
}

bool isAnyFrontButtonPressed(const MappedInputManager& mappedInput) {
  return mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}

int minimalHomeNavCount(bool hasCurrentBook) { return hasCurrentBook ? 4 : 3; }

// Draw a 1-bit packed icon scaled with nearest-neighbor blocks. drawIcon
// doesn't scale, and baking pre-scaled assets into flash would balloon their
// size, so this trades a pile of small fillRects for keeping the existing
// 32×32 sources.
//
// Important: the icon bytes are stored in eink-native orientation
// (LandscapeCounterClockwise — the panel's physical layout). The renderer
// translates draw coordinates to native via Portrait rotation
// `native = (logical_y, panelHeight-1-logical_x)`. To read the right pixel
// for a logical destination, we map logical (lx, ly) within the icon to
// native (nx=ly, ny=srcSize-1-lx) and read the bit from native row-major
// storage. Without this mapping the icon renders rotated 90° CCW.
void drawIconScaled(const GfxRenderer& renderer, const uint8_t* iconData, int srcSize, int dstX, int dstY,
                    int dstSize) {
  for (int slx = 0; slx < srcSize; ++slx) {
    const int dstXStart = (slx * dstSize) / srcSize;
    const int dstXEnd = ((slx + 1) * dstSize) / srcSize;
    const int blockW = dstXEnd - dstXStart;
    if (blockW <= 0) continue;
    for (int sly = 0; sly < srcSize; ++sly) {
      // Logical (slx, sly) → native (sly, srcSize-1-slx). Bytes are row-major
      // in native: bitPos = ny * srcSize + nx.
      const int bitPos = (srcSize - 1 - slx) * srcSize + sly;
      const uint8_t byte = iconData[bitPos / 8];
      const bool isBlack = !((byte >> (7 - (bitPos % 8))) & 1);
      if (!isBlack) continue;
      const int dstYStart = (sly * dstSize) / srcSize;
      const int dstYEnd = ((sly + 1) * dstSize) / srcSize;
      const int blockH = dstYEnd - dstYStart;
      if (blockH > 0) {
        renderer.fillRect(dstX + dstXStart, dstY + dstYStart, blockW, blockH, true);
      }
    }
  }
}

float clampProgressPercent(const float progress) { return std::clamp(progress, 0.0f, 100.0f); }

bool hasAnyBookStats(const BookReadingStats& stats) {
  return stats.sessionCount > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 || stats.isCompleted;
}

bool hasAnyGlobalStats(const GlobalReadingStats& stats) {
  return stats.totalSessions > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 ||
         stats.completedBooks > 0;
}

float loadEpubProgressPercent(const RecentBook& book) {
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", epub.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[6];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 6) {
    return -1.0f;
  }

  const int spineIndex = data[0] | (data[1] << 8);
  const int currentPage = data[2] | (data[3] << 8);
  const int pageCount = data[4] | (data[5] << 8);
  if (pageCount <= 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
  return clampProgressPercent(epub.calculateProgress(spineIndex, chapterProgress) * 100.0f);
}

float loadXtcProgressPercent(const RecentBook& book) {
  Xtc xtc(book.path, "/.crosspoint");
  if (!xtc.load()) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", xtc.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[4];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return clampProgressPercent(static_cast<float>(xtc.calculateProgress(currentPage)));
}

float loadTxtProgressPercent(const RecentBook& book) {
  Txt txt(book.path, "/.crosspoint");
  if (!txt.load()) {
    return -1.0f;
  }

  FsFile progressFile;
  if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/progress.bin", progressFile)) {
    return -1.0f;
  }

  uint8_t progressData[4];
  const int progressBytes = progressFile.read(progressData, sizeof(progressData));
  progressFile.close();
  if (progressBytes != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(progressData[0]) | (static_cast<uint32_t>(progressData[1]) << 8);

  FsFile indexFile;
  if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/index.bin", indexFile)) {
    return -1.0f;
  }

  uint32_t magic = 0;
  serialization::readPod(indexFile, magic);
  uint8_t version = 0;
  serialization::readPod(indexFile, version);
  uint32_t fileSize = 0;
  serialization::readPod(indexFile, fileSize);
  int32_t cachedWidth = 0;
  serialization::readPod(indexFile, cachedWidth);
  int32_t cachedLines = 0;
  serialization::readPod(indexFile, cachedLines);
  int32_t fontId = 0;
  serialization::readPod(indexFile, fontId);
  int32_t margin = 0;
  serialization::readPod(indexFile, margin);
  uint8_t alignment = 0;
  serialization::readPod(indexFile, alignment);
  uint32_t totalPages = 0;
  serialization::readPod(indexFile, totalPages);
  indexFile.close();
  (void)cachedWidth;
  (void)cachedLines;
  (void)fontId;
  (void)margin;
  (void)alignment;

  if (magic != TXT_CACHE_MAGIC || version != TXT_CACHE_VERSION || fileSize != txt.getFileSize() || totalPages == 0) {
    return -1.0f;
  }

  return clampProgressPercent((static_cast<float>(currentPage + 1) / static_cast<float>(totalPages)) * 100.0f);
}

float loadRecentBookProgressPercent(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return loadEpubProgressPercent(book);
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return loadXtcProgressPercent(book);
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return loadTxtProgressPercent(book);
  }
  return -1.0f;
}

// Per-book stats live at /.crosspoint/{prefix}_{hash}/stats.bin where the
// prefix tracks the format of the book file. Same dispatch as the progress
// loader above so we resolve the right cache directory per book.
std::string statsCachePathForBook(const std::string& bookPath) {
  const char* prefix = nullptr;
  if (FsHelpers::hasEpubExtension(bookPath)) prefix = "epub_";
  else if (FsHelpers::hasXtcExtension(bookPath)) prefix = "xtc_";
  else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) prefix = "txt_";
  if (prefix == nullptr) return {};
  return "/.crosspoint/" + std::string(prefix) + std::to_string(std::hash<std::string>{}(bookPath));
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  // The home strip is a fixed 6-icon horizontal row (Recent, Stats,
  // FileTransfer, Browse, Bookmarks, Settings). The "menu count" used by
  // loop() is the total navigable count (recent books + the 6 menu items).
  int count = 6;
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // ESP32-C3 has ~380 KB usable RAM and no PSRAM. Sequential thumb generation
  // (EPUB load + JPEG decode + BMP write) fragments the heap; once free heap
  // drops below this floor the next decode cannot find a contiguous block and
  // crashes. Bail out early instead — the home screen renders with whatever
  // covers loaded so far rather than bricking. See "more than 5 books bricks"
  // bug, May 2026.
  constexpr size_t MIN_FREE_HEAP_FOR_THUMB = 80 * 1024;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // Skip stale entries: recent.bin may reference a book that was deleted
    // from the SD card. Opening a missing path crashes inside Epub::load.
    if (!Storage.exists(book.path.c_str())) {
      progress++;
      continue;
    }
    // Self-heal: if a previous loadRecentCovers pass cleared this book's
    // coverBmpPath (because thumb gen returned false), re-derive the template
    // path from a fresh Epub/Xtc instance so we get another shot at it. This
    // matters after a firmware upgrade where the new build's larger thumb
    // dimensions stress the JPEG decoder past the first run's heap headroom —
    // without this branch, those books permanently render as the fallback
    // silhouette.
    if (book.coverBmpPath.empty()) {
      RecentBook refreshed = RECENT_BOOKS.getDataFromBook(book.path);
      if (!refreshed.coverBmpPath.empty()) {
        book.coverBmpPath = refreshed.coverBmpPath;
        (void)RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
      }
    }
    if (!book.coverBmpPath.empty()) {
      // Use the EXACT-size path (no fallback chain) so the regen decision is
      // based on whether the thumb at this theme's coverHeight exists on
      // disk — not whether ANY thumb size happens to be on disk. The render
      // path keeps the fallback chain via UITheme::getCoverThumbPath so we
      // still show something while a higher-resolution thumb regenerates.
      // Without this, switching from Flow (392-tall thumbs) to Minimal
      // (583-tall slots) would just upscale the Flow thumb to fill the
      // Minimal slot, producing visibly pixelated covers.
      std::string coverPath = UITheme::resolveExactCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // Heap-floor guard before each fresh thumb gen. If we're already low,
        // stop the loop entirely so the remaining cards render as fallbacks
        // instead of risking an OOM mid-decode.
        const uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < MIN_FREE_HEAP_FOR_THUMB) {
          LOG_ERR("HOME", "Skipping remaining thumb gen (free heap %u < %u)", freeHeap,
                  static_cast<unsigned>(MIN_FREE_HEAP_FOR_THUMB));
          break;
        }
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            // Transient: a single failure (heap pressure, decoder edge case,
            // an SD read hiccup) used to permanently zero out coverBmpPath
            // here, which left the book rendering as the fallback silhouette
            // forever even after the underlying cause cleared. Keep the
            // template path so the next home enter retries — the render path
            // already degrades gracefully when the file is missing.
            LOG_ERR("HOME", "generateThumbBmp(%d) failed for %s; will retry next home enter",
                    coverHeight, book.path.c_str());
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              LOG_ERR("HOME", "xtc generateThumbBmp(%d) failed for %s; will retry next home enter",
                      coverHeight, book.path.c_str());
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;

  // Now that every recent book either has its thumb on disk or is known to
  // have no cover, slurp the thumb file bytes into RAM so per-render carousel
  // draws don't reopen the SD file for each of the 5 covers. Capped at
  // kMaxThumbCacheBytes per entry; entries we can't cache fall through to
  // the existing file-backed Bitmap path.
  loadRecentThumbsToRam(coverHeight);
}

void HomeActivity::loadRecentThumbsToRam(int coverHeight) {
  // Reset the cache to mirror the current recentBooks vector size and clear
  // any stale buffers (e.g. after a recents reshuffle).
  recentBookThumbData.clear();
  recentBookThumbData.resize(recentBooks.size());
  recentBookDecodedThumbs.clear();
  recentBookDecodedThumbs.resize(recentBooks.size());

  for (size_t i = 0; i < recentBooks.size(); ++i) {
    const auto& book = recentBooks[i];
    if (book.coverBmpPath.empty()) continue;

    const std::string resolvedPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
    if (resolvedPath.empty()) continue;

    FsFile file;
    if (!Storage.openFileForRead("HOME", resolvedPath, file)) continue;

    const size_t fileSize = file.fileSize();
    if (fileSize == 0 || fileSize > kMaxThumbCacheBytes) {
      file.close();
      LOG_DBG("HOME", "Thumb %s skipped from RAM cache (size %u, cap %u)", resolvedPath.c_str(),
              static_cast<unsigned>(fileSize), static_cast<unsigned>(kMaxThumbCacheBytes));
      continue;
    }

    // Heap-floor guard: we build with -fno-exceptions, so a bad_alloc from
    // vector::resize() doesn't unwind — it goes straight to __terminate ->
    // abort() and panics the firmware (observed in a real crash report on
    // first home enter when the largest contiguous heap block was smaller
    // than the requested thumb size). Conservatively skip caching when the
    // largest contiguous block doesn't have enough headroom; the affected
    // book just renders through the slow file-backed path instead.
    constexpr size_t kAllocHeadroom = 8 * 1024;
    const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
    if (maxAllocHeap < fileSize + kAllocHeadroom) {
      file.close();
      LOG_ERR("HOME", "Thumb %s skipped from RAM cache (max alloc %u < needed %u)", resolvedPath.c_str(),
              static_cast<unsigned>(maxAllocHeap), static_cast<unsigned>(fileSize + kAllocHeadroom));
      continue;
    }

    auto& buf = recentBookThumbData[i];
    buf.resize(fileSize);
    const int bytesRead = file.read(buf.data(), fileSize);
    file.close();

    if (bytesRead != static_cast<int>(fileSize)) {
      // Partial read — drop the entry rather than serving truncated data.
      buf.clear();
      buf.shrink_to_fit();
      LOG_ERR("HOME", "Thumb %s partial RAM read (%d / %u)", resolvedPath.c_str(), bytesRead,
              static_cast<unsigned>(fileSize));
      continue;
    }

    // Pre-decode the BMP into a flat 1-bit grid so the per-render carousel
    // path can skip the BMP header parse + per-row 2-bit quantization on
    // every scroll. Failures here are non-fatal — the byte cache above is
    // the fallback. We allocate up-front based on the source dimensions so
    // a failed decode doesn't leave half a grid populated.
    Bitmap bitmap(buf.data(), buf.size());
    if (bitmap.parseHeaders() != BmpReaderError::Ok) continue;
    const size_t gridBytes = static_cast<size_t>((bitmap.getWidth() + 7) / 8) * static_cast<size_t>(bitmap.getHeight());
    // Heap headroom check — same rationale as the byte slurp above. A failed
    // decode falls through to the byte-cache rendering path.
    constexpr size_t kDecodeAllocHeadroom = 4 * 1024;
    if (ESP.getMaxAllocHeap() < gridBytes + kDecodeAllocHeadroom) continue;
    (void)renderer.decodeBitmapTo1BitGrid(bitmap, recentBookDecodedThumbs[i]);
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  // Check if any books have bookmarks (directory scan only, no file parsing)
  hasBookmarks = BookmarkStore::hasAnyBookmarks();

  selectorIndex = 0;

  // Minimal-theme state: swallow the first front-button release after
  // entering home so a long-press exit from the reader doesn't immediately
  // re-trigger the first Minimal nav slot.
  minimalMenuOpen = false;
  minimalHomeNavIndex = -1;
  minimalSuppressInitialFrontRelease = isMinimalTheme();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  // Load reading stats for the most recent EPUB book so they can be shown on the home card.
  currentBookStats = BookReadingStats{};
  currentBookProgressPercent = -1.0f;
  if (!recentBooks.empty() && FsHelpers::hasEpubExtension(recentBooks[0].path)) {
    const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(recentBooks[0].path));
    currentBookStats = BookReadingStats::load(cachePath);
  }
  if (!recentBooks.empty()) {
    currentBookProgressPercent = loadRecentBookProgressPercent(recentBooks[0]);
  }

  // Eagerly load per-book stats for the carousel covers. Each load is a
  // 12-byte file; with homeRecentBooksCount capped at 5 this is negligible
  // and cheaper than re-reading on every carousel scroll.
  recentBookStats.clear();
  recentBookStats.reserve(recentBooks.size());
  recentBookProgress.clear();
  recentBookProgress.reserve(recentBooks.size());
  for (const auto& book : recentBooks) {
    BookReadingStats stats;
    const std::string path = statsCachePathForBook(book.path);
    if (!path.empty()) {
      stats = BookReadingStats::load(path);
    }
    recentBookStats.push_back(stats);
    // Per-book progress: more expensive (opens the EPUB to resolve the cache
    // path and run calculateProgress), but homeRecentBooksCount is capped at
    // 5, so this is a few hundred ms one-time cost on home enter — small
    // compared to the cover thumbnail generation that happens here too.
    recentBookProgress.push_back(loadRecentBookProgressPercent(book));
  }
  globalStats = GlobalReadingStats::load();
  hasReadingStats = hasAnyBookStats(currentBookStats) || hasAnyGlobalStats(globalStats);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
  // Release the RAM-cached thumbnail bytes; they're per-home-session and
  // can be re-slurped from disk on the next enter.
  recentBookThumbData.clear();
  recentBookThumbData.shrink_to_fit();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  // Tag the buffer with the carousel idx render() snapshotted before drawing,
  // so a subsequent render can detect that we cached for book X even though
  // the user has since scrolled to book Y. pendingCoverBufferBookIdx is set
  // by render() just before invoking the theme; it never reads live
  // selectorIndex here because that may have moved during the draw.
  coverBufferBookIdx = pendingCoverBufferBookIdx;
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = renderer.getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
  coverBufferBookIdx = -1;
  homeMenuIconsCached = false;
}

void HomeActivity::loop() {
  const int recentCount = static_cast<int>(recentBooks.size());

  // Minimal theme: front-button hint slots (MENU/BROWSE/SETTINGS/READ) act
  // as direct actions; pressing MENU opens an overlay containing
  // buildMinimalMenuItems. Upstream's exact pattern is preserved here.
  if (isMinimalTheme()) {
    const int releasedFrontButton = mappedInput.getReleasedFrontButton();

    if (minimalSuppressInitialFrontRelease) {
      if (releasedFrontButton >= 0) {
        minimalSuppressInitialFrontRelease = false;
        return;
      }
      if (!isAnyFrontButtonPressed(mappedInput)) {
        minimalSuppressInitialFrontRelease = false;
      }
    }

    if (minimalMenuOpen) {
      const auto frameHasReadingStats = hasReadingStats || hasAnyGlobalStats(globalStats);
      const auto menuItems = buildMinimalMenuItems(hasOpdsServers, frameHasReadingStats, hasBookmarks);
      const int menuCount = static_cast<int>(menuItems.size());
      if (menuCount <= 0) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }
      if (minimalMenuIndex >= menuCount) {
        minimalMenuIndex = menuCount - 1;
      }
      buttonNavigator.onPreviousPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::previousIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      buttonNavigator.onNextPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::nextIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        switch (menuItems[minimalMenuIndex].action) {
          case HomeMenuAction::BrowseFiles: onFileBrowserOpen(); break;
          case HomeMenuAction::RecentBooks: onRecentsOpen(); break;
          case HomeMenuAction::OpdsBrowser: onOpdsBrowserOpen(); break;
          case HomeMenuAction::ReadingStats: onReadingStatsOpen(); break;
          case HomeMenuAction::Bookmarks: onBookmarksOpen(); break;
          case HomeMenuAction::FileTransfer: onFileTransferOpen(); break;
          case HomeMenuAction::Settings: onSettingsOpen(); break;
        }
      }
      return;
    }

    // Home screen with the 4-slot front-button hint row. Index 0 = MENU,
    // 1 = BROWSE, 2 = SETTINGS, 3 = READ (only present when there's a book).
    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      minimalHomeNavIndex = minimalHomeNavIndex < 0
                                ? homeNavCount - 1
                                : ButtonNavigator::previousIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      minimalHomeNavIndex =
          minimalHomeNavIndex < 0 ? 0 : ButtonNavigator::nextIndex(minimalHomeNavIndex, homeNavCount);
      requestUpdate();
      return;
    }

    auto activateMinimalHomeNav = [this](int index) {
      switch (index) {
        case 0:
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          requestUpdate();
          break;
        case 1: onFileBrowserOpen(); break;
        case 2: onSettingsOpen(); break;
        case 3: onContinueReading(); break;
      }
    };

    if (releasedFrontButton == HalGPIO::BTN_BACK) {
      minimalHomeNavIndex = 0;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_CONFIRM) {
      minimalHomeNavIndex = 1;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_LEFT) {
      minimalHomeNavIndex = 2;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_RIGHT) {
      if (!recentBooks.empty()) {
        minimalHomeNavIndex = 3;
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (minimalHomeNavIndex >= 0) {
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    return;
  }

  // Non-Flow themes use upstream CrossInk's vertical-list navigation: a
  // single ButtonNavigator cycles selectorIndex through (bookCount + menu
  // items), Confirm dispatches by action. This block returns early so the
  // rest of loop() — which is Flow-specific (carousel + horizontal strip
  // split) — is skipped on those themes.
  if (!isFlowTheme()) {
    const auto frameHasReadingStats = hasReadingStats || hasAnyGlobalStats(globalStats);
    const auto items = buildHomeMenuItems(hasOpdsServers, frameHasReadingStats, hasBookmarks);
    const int totalCount = recentCount + static_cast<int>(items.size());
    if (totalCount > 0) {
      buttonNavigator.onNextRelease([this, totalCount] {
        selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), totalCount);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this, totalCount] {
        selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), totalCount);
        requestUpdate();
      });
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (static_cast<int>(selectorIndex) < recentCount) {
        onSelectBook(recentBooks[selectorIndex].path);
        return;
      }
      const int menuIdx = static_cast<int>(selectorIndex) - recentCount;
      if (menuIdx < 0 || menuIdx >= static_cast<int>(items.size())) return;
      switch (items[menuIdx].action) {
        case HomeMenuAction::BrowseFiles: onFileBrowserOpen(); break;
        case HomeMenuAction::RecentBooks: onRecentsOpen(); break;
        case HomeMenuAction::OpdsBrowser: onOpdsBrowserOpen(); break;
        case HomeMenuAction::ReadingStats: onReadingStatsOpen(); break;
        case HomeMenuAction::Bookmarks: onBookmarksOpen(); break;
        case HomeMenuAction::FileTransfer: onFileTransferOpen(); break;
        case HomeMenuAction::Settings: onSettingsOpen(); break;
      }
    }
    return;
  }

  const bool inCarousel = static_cast<int>(selectorIndex) < recentCount;

  // X3 Flow theme: bottom rocker (Left/Right) cycles through the horizontal
  // menu strip; side rocker (Up/Down) navigates the carousel. Pressing
  // Left/Right from inside the carousel jumps to the menu (first or last
  // icon); once in the menu, Left/Right wrap within the 6 icons. Up/Down
  // brings the user back to the centered book.
  constexpr int kHomeMenuCount = 6;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (inCarousel) {
      lastBookIndex = selectorIndex;
      selectorIndex = recentCount;  // first menu icon
    } else {
      const int menuIdx = static_cast<int>(selectorIndex) - recentCount;
      selectorIndex = recentCount + (menuIdx + 1) % kHomeMenuCount;
    }
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (inCarousel) {
      lastBookIndex = selectorIndex;
      selectorIndex = recentCount + kHomeMenuCount - 1;  // last menu icon
    } else {
      const int menuIdx = static_cast<int>(selectorIndex) - recentCount;
      selectorIndex = recentCount + (menuIdx - 1 + kHomeMenuCount) % kHomeMenuCount;
    }
    requestUpdate();
  }

  // Side rocker (Up/Down) → carousel navigation. From the menu, the first
  // press drops back to the last book the user was on. Inside the carousel
  // it wraps end-to-end.
  //
  // The cover-buffer cache is invalidated only on the "scroll within
  // carousel" branch — menu↔carousel transitions keep the same centered
  // book (lastBookIndex), so the cached cover + chrome stay valid and the
  // next render skips its SD I/O. This is the main lag fix.
  if (recentCount > 0 && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (!inCarousel) {
      selectorIndex = lastBookIndex < recentCount ? lastBookIndex : 0;
    } else {
      selectorIndex = (selectorIndex + 1) % recentCount;
      coverRendered = false;
      coverBufferStored = false;
      // The theme's next storeCoverBuffer will overwrite our cache without
      // the menu icons baked in, so mark the icons as no longer cached and
      // force the next render to re-paint them.
      homeMenuIconsCached = false;
    }
    requestUpdate();
  }
  if (recentCount > 0 && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (!inCarousel) {
      selectorIndex = lastBookIndex < recentCount ? lastBookIndex : 0;
    } else {
      selectorIndex = (selectorIndex == 0) ? recentCount - 1 : selectorIndex - 1;
      coverRendered = false;
      coverBufferStored = false;
      homeMenuIconsCached = false;
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Indices match the kHomeMenuItems array in render(): Recent / Stats /
    // FileTransfer / Browse / Bookmarks-or-OPDS / Settings. Slot 4 is dynamic
    // — it shows the OPDS browser when the user has any servers configured
    // (kHomeMenuItems mirrors this), otherwise it opens Bookmarks. Default
    // is Bookmarks.
    const int menuIdx = static_cast<int>(selectorIndex) - static_cast<int>(recentBooks.size());
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      switch (menuIdx) {
        case 0: onRecentsOpen(); break;
        case 1: onReadingStatsOpen(); break;
        case 2: onFileTransferOpen(); break;
        case 3: onFileBrowserOpen(); break;
        case 4: hasOpdsServers ? onOpdsBrowserOpen() : onBookmarksOpen(); break;
        case 5: onSettingsOpen(); break;
        default: break;
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Resolve which book is centered in the carousel for this frame. Same
  // value whether the cursor is on the carousel (`selectorIndex` IS the
  // book idx) or down in the menu (`lastBookIndex` is the book that was
  // centered when the cursor left the carousel). Using a single
  // mode-invariant index here means the cover-buffer cache key doesn't flip
  // when the cursor crosses between the two — toggling carousel↔menu
  // preserves the cache, and the carousel area doesn't repaint.
  const int recentCountInt = static_cast<int>(recentBooks.size());
  const bool inMenuForCarousel = static_cast<int>(selectorIndex) >= recentCountInt;
  const int centeredBookIdx =
      (inMenuForCarousel && lastBookIndex >= 0 && lastBookIndex < recentCountInt)
          ? lastBookIndex
          : static_cast<int>(selectorIndex);

  // If the cached cover buffer was captured for a different book than the one
  // we're about to draw, drop it. This catches the race where loop() (main
  // task) advanced selectorIndex while the render task was mid-draw and the
  // theme's end-of-render coverRendered=true overwrote the loop's
  // coverRendered=false.
  if (coverBufferStored && coverBufferBookIdx != centeredBookIdx) {
    freeCoverBuffer();  // also resets coverBufferStored / coverBufferBookIdx
    coverRendered = false;
  }
  pendingCoverBufferBookIdx = centeredBookIdx;

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Home-page-only battery percentage under the battery top bar, right-aligned
  // at the same screenMargin inset the bar uses. The left/center status texts
  // we briefly tried were too cluttered, so this stays a solo element.
  // Skipped on the Minimal theme: Minimal's home page is intentionally text-
  // light, and the top-edge battery bar alone is enough.
  if (!isMinimalTheme()) {
    const uint16_t batteryPct = std::min<uint16_t>(powerManager.getBatteryPercentage(), 100);
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", static_cast<unsigned>(batteryPct));
    int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
    renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                     &orientedMarginLeft);
    const int rightInset = orientedMarginRight + SETTINGS.screenMargin;
    const int pctWidth = renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
    // Battery bar top is y=12 on X3, y=18 on X4 (per BaseTheme::drawBatteryTopBar).
    // Bar is 3 px tall, so its bottom row sits at barTopY+2. Place the
    // percentage text 3 px below the bar bottom on both devices so the gap
    // is identical — the X4 just gets a larger absolute offset because the
    // bar itself is lower.
    const int pctY = gpio.deviceIsX4() ? 24 : 18;
    renderer.drawText(SMALL_FONT_ID, pageWidth - rightInset - pctWidth, pctY, pctBuf, true);
  }

  // (centeredBookIdx / recentCountInt / inMenuForCarousel computed at top of
  // render() so the cover-buffer-staleness check can run before
  // restoreCoverBuffer.)
  // Stats for the book currently centered in the carousel — Flow theme uses
  // this to render an "Xh Ym" indicator under the cover.
  const BookReadingStats* centeredBookStats =
      (centeredBookIdx >= 0 && centeredBookIdx < static_cast<int>(recentBookStats.size()))
          ? &recentBookStats[centeredBookIdx]
          : nullptr;
  const float centeredBookProgress =
      (centeredBookIdx >= 0 && centeredBookIdx < static_cast<int>(recentBookProgress.size()))
          ? recentBookProgress[centeredBookIdx]
          : -1.0f;
  // Horizontal icon strip — 6 fixed items, drawn just above the button hints.
  // Selected icon gets a light-gray rounded cell. Cycle order matches the
  // dispatch in loop() — keep them in sync. The bookmark item has a null
  // icon pointer because we draw a 5-point ribbon polygon for it instead of
  // blitting a stored bitmap (matches the bookmark glyph used elsewhere).
  struct HomeMenuItem {
    const uint8_t* icon;  // nullptr for special items rendered as polygons
    bool isBookmark;
    const char* label;
  };
  // Slot 4 swaps Bookmarks → OPDS Servers when the user has any OPDS server
  // configured. Default stays at Bookmarks so devices with no servers keep
  // the original 6-item menu. Couldn't verify against upstream v1.2.11.1
  // CrossInk lyra carousel, so the swap is unconditional on hasOpdsServers
  // — flag if you want a different rule (e.g., only swap when there are also
  // no bookmarks saved).
  const HomeMenuItem slot4 = hasOpdsServers
                                 ? HomeMenuItem{LibraryIcon, false, tr(STR_OPDS_SERVERS)}
                                 : HomeMenuItem{nullptr, true, tr(STR_BOOKMARKS)};
  const HomeMenuItem kHomeMenuItems[6] = {
      {RecentIcon, false, tr(STR_MENU_RECENT_BOOKS)}, {ChartIcon, false, tr(STR_READING_STATS)},
      {TransferIcon, false, tr(STR_FILE_TRANSFER)},   {FolderIcon, false, tr(STR_BROWSE_FILES)},
      slot4,                                          {Settings2Icon, false, tr(STR_SETTINGS_TITLE)},
  };
  constexpr int kHomeMenuCount = 6;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, centeredBookIdx, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this), centeredBookStats,
                          centeredBookProgress, &recentBookThumbData, &recentBookDecodedThumbs);

  const int menuSelectedIndex =
      (selectorIndex >= recentBooks.size()) ? static_cast<int>(selectorIndex - recentBooks.size()) : -1;

  if (isMinimalTheme()) {
    // Upstream Minimal home: 4 front-button-mapped hint slots at the bottom
    // (MENU/BROWSE/SETTINGS/READ); pressing MENU opens an overlay with the
    // full Minimal menu items list.
    if (minimalMenuOpen) {
      const auto frameHasReadingStats = hasReadingStats || hasAnyGlobalStats(globalStats);
      const auto menuItems = buildMinimalMenuItems(hasOpdsServers, frameHasReadingStats, hasBookmarks);
      GUI.drawButtonMenu(
          renderer, Rect{0, metrics.homeTopPadding, pageWidth, pageHeight - metrics.homeTopPadding},
          static_cast<int>(menuItems.size()), minimalMenuIndex,
          [&menuItems](int index) -> std::string { return menuItems[index].label; },
          [&menuItems](int index) -> UIIcon { return menuItems[index].icon; });
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    // Highlight whichever front-button slot the user navigated to last.
    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }
    MinimalTheme::setHomeButtonHintSelection(minimalHomeNavIndex);
    GUI.drawButtonHints(renderer, tr(STR_RECENT), tr(STR_BROWSE), tr(STR_SETTINGS_TITLE),
                        recentBooks.empty() ? "" : tr(STR_READ));
    renderer.displayBuffer();
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
    }
    return;
  }

  if (!isFlowTheme()) {
    // Non-Flow themes defer the home menu to the theme's own drawButtonMenu
    // implementation (vertical tiles for Lyra, etc.) and use the dynamic
    // upstream item list. Skip the Flow-only horizontal strip + label/subtitle
    // overlay below.
    const auto frameHasReadingStats = hasReadingStats || hasAnyGlobalStats(globalStats);
    const auto items = buildHomeMenuItems(hasOpdsServers, frameHasReadingStats, hasBookmarks);
    const int menuTopY = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing;
    const int menuHeight =
        pageHeight -
        (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 + metrics.buttonHintsHeight);
    GUI.drawButtonMenu(
        renderer, Rect{0, menuTopY, pageWidth, menuHeight}, static_cast<int>(items.size()),
        menuSelectedIndex,
        [&items](int index) -> std::string { return items[index].label; },
        [&items](int index) -> UIIcon { return items[index].icon; });
    const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
    }
    return;
  }

  // When the user is on a menu icon (not in the carousel), overlay a menu
  // label + dynamic subtitle in the same title/author band the theme uses for
  // the centered book — keeps the chrome symmetric whether the cursor is on a
  // book or a menu item. The title-area top derives from the same constants
  // LyraFlowTheme uses (kCoverTopOffset=48, centerCoverHeight=392, 8 px
  // cover→bar gap, 3 px bar, 6 px bar→time gap); the bottom is the rect
  // bottom, which is laid out to align with the icon strip top. Subtitles
  // are Option D — live counts / state for each menu item. These constants
  // MUST stay in sync with LyraFlowTheme's progressBarTopY / timeReadY math
  // so the menu-mode wipe lands exactly on the carousel-mode title area.
  //
  // Wrapped in a lambda and invoked AFTER the icon-strip cache below so the
  // cache holds covers + book title/author + icons but NOT this overlay (whose
  // content varies per menu selection). On a future render the cache restore
  // brings back the book's title/author; if we're in menu mode we then overlay
  // the menu label on top, and if we're not, the book title/author is already
  // correctly visible from the restore — no per-mode invalidation needed.
  auto drawMenuOverlay = [&]() {
    if (menuSelectedIndex < 0 || menuSelectedIndex >= kHomeMenuCount) return;
    const int rectTop = metrics.homeTopPadding;
    const int rectBottom = rectTop + metrics.homeCoverTileHeight;
    // Runtime-branched to match LyraFlowTheme's two carousel layouts. MUST
    // stay in sync with LAYOUT_3 / LAYOUT_5 in that file. (The cleaner fix
    // is to expose these via ThemeMetrics so they're a single source of
    // truth; not done here to keep the change contained.)
    const bool fiveCoverFlow = SETTINGS.flowCarouselSize == CrossPointSettings::CAROUSEL_5;
    const int kCoverTopOffset = fiveCoverFlow ? 48 : 28;
    const int kCenterCoverHeight = fiveCoverFlow ? 392 : 480;
    const int progressBarTopY = rectTop + kCoverTopOffset + kCenterCoverHeight + 8;
    constexpr int kProgressBarVisualHeight = 3;
    const int timeReadY = progressBarTopY + kProgressBarVisualHeight + 6;
    const int smallLh = renderer.getLineHeight(SMALL_FONT_ID);
    const int textAreaTop = timeReadY + smallLh + 1;
    const int textAreaHeight = std::max(0, rectBottom - textAreaTop);
    const int titleLh = renderer.getLineHeight(UI_12_FONT_ID);
    const int subtitleLh = renderer.getLineHeight(UI_10_FONT_ID);
    const int titleBlockHeight = titleLh + 1 + subtitleLh;
    const int labelY = textAreaTop + std::max(0, (textAreaHeight - titleBlockHeight) / 2);
    const int subtitleY = labelY + titleLh + 1;

    // Wipe the entire title area first so the book title/author the theme
    // drew is replaced cleanly. Anything outside this band stays untouched.
    renderer.fillRect(0, textAreaTop, pageWidth, std::max(0, rectBottom - textAreaTop), false);

    char subBuf[24];
    const char* subtitle = "";
    switch (menuSelectedIndex) {
      case 0:
        // Use the true RecentBooksStore count, not the visible-window-capped
        // recentBooks.size() — otherwise the subtitle reads "3 books" or
        // "5 books" matching the carousel's display cap rather than the
        // actual number of recent books the user has.
        snprintf(subBuf, sizeof(subBuf), "%u books", static_cast<unsigned>(RECENT_BOOKS.getCount()));
        subtitle = subBuf;
        break;
      case 1: {
        const uint32_t hours = globalStats.totalReadingSeconds / 3600;
        snprintf(subBuf, sizeof(subBuf), "%uh read", static_cast<unsigned>(hours));
        subtitle = subBuf;
        break;
      }
      case 2:
        subtitle = "Wireless";
        break;
      case 3:
        subtitle = "Library";
        break;
      case 4:
        // Subtitle tracks whichever slot-4 mode is active (see kHomeMenuItems).
        subtitle = hasOpdsServers ? "Servers" : (hasBookmarks ? "Saved" : "None");
        break;
      case 5:
        subtitle = "v" CROSSPOINT_VERSION;
        break;
    }

    const char* label = kHomeMenuItems[menuSelectedIndex].label;
    const std::string truncLabel =
        renderer.truncatedText(UI_12_FONT_ID, label, pageWidth - 40, EpdFontFamily::BOLD);
    const int labelW = renderer.getTextWidth(UI_12_FONT_ID, truncLabel.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, (pageWidth - labelW) / 2, labelY, truncLabel.c_str(), true,
                      EpdFontFamily::BOLD);

    if (subtitle != nullptr && subtitle[0] != '\0') {
      const std::string truncSub = renderer.truncatedText(UI_10_FONT_ID, subtitle, pageWidth - 40);
      const int subW = renderer.getTextWidth(UI_10_FONT_ID, truncSub.c_str());
      renderer.drawText(UI_10_FONT_ID, (pageWidth - subW) / 2, subtitleY, truncSub.c_str(), true);
    }
  };
  // Icon strip sizing (kHomeMenuItems / kHomeMenuCount defined above).
  constexpr int kIconSrcSize = 32;   // raw bitmap dimensions (assets are 32×32)
  constexpr int iconSize = 40;       // visual size; drawIconScaled resamples 32→40
  constexpr int iconCellSize = 56;
  constexpr int sideMargin = 32;

  const int hintsTopY = pageHeight - metrics.buttonHintsHeight;
  const int iconCellBottomY = hintsTopY - 16;  // 16 px gap above the hints row
  const int iconCellTopY = iconCellBottomY - iconCellSize;
  const int totalIconArea = pageWidth - 2 * sideMargin;
  const int iconPitch = totalIconArea / kHomeMenuCount;

  auto iconCellCenterX = [&](int i) { return sideMargin + iconPitch / 2 + i * iconPitch; };

  // One icon draw — no selection-cell background — extracted so we can call
  // it from two places: (1) the fresh pass that paints all 6 icons into the
  // cache, and (2) the per-frame redraw of the selected icon over the cell
  // background (the dithered LightGray fill erases icon pixels underneath).
  auto drawSingleMenuIcon = [&](int i) {
    const int cellCenterX = iconCellCenterX(i);
    const int iconX = cellCenterX - iconSize / 2;
    const int iconY = iconCellTopY + (iconCellSize - iconSize) / 2;
    if (kHomeMenuItems[i].isBookmark) {
      // Outlined bookmark, drawn at the same 32×32 logical source resolution
      // as the other home-strip icons and nearest-neighbor blitted up to the
      // 40-px cell. Drawing in source space (not screen space) keeps the
      // stroke weight matched to the line-art icons on either side — at
      // 4 source-px the stroke ends up ~5 screen-px after the 32→40 scale.
      // The shape extends to the full cell height (kTop=0, kBottom=31) and
      // the notch tip sits at kNotchTipY=18 so the bottom-V is tall enough
      // not to read as a squat triangle.
      constexpr int kLeft = 5;
      constexpr int kRight = 26;
      constexpr int kTop = 2;
      constexpr int kBottom = 28;
      constexpr int kNotchTipX = 15;
      constexpr int kNotchTipY = 16;       // depth = 12, ~same fraction of height as before — V stays tall
      constexpr int kStroke = 2;

      auto blitBlock = [&](int sx, int sy) {
        if (sx < 0 || sx >= kIconSrcSize || sy < 0 || sy >= kIconSrcSize) return;
        const int dstXStart = (sx * iconSize) / kIconSrcSize;
        const int dstXEnd = ((sx + 1) * iconSize) / kIconSrcSize;
        const int dstYStart = (sy * iconSize) / kIconSrcSize;
        const int dstYEnd = ((sy + 1) * iconSize) / kIconSrcSize;
        const int blockW = dstXEnd - dstXStart;
        const int blockH = dstYEnd - dstYStart;
        if (blockW > 0 && blockH > 0) {
          renderer.fillRect(iconX + dstXStart, iconY + dstYStart, blockW, blockH, true);
        }
      };
      auto fillSpan = [&](int x0, int x1, int sy) {
        for (int sx = x0; sx <= x1; ++sx) blitBlock(sx, sy);
      };

      // Top edge — stepped 2-px inset on the outermost row, 1-px inset on the
      // next row, so the top-left/right corners read as a 3-step diagonal
      // staircase (after 32→40 scale that's a clearly visible round, not
      // the previous 1-px-clip subtle round).
      for (int sy = kTop; sy < kTop + kStroke; ++sy) {
        int inset = 0;
        if (sy == kTop) inset = 2;
        else if (sy == kTop + 1) inset = 1;
        fillSpan(kLeft + inset, kRight - inset, sy);
      }
      // Left and right verticals — same inset at the top two rows so they
      // don't refill the pixels we just clipped on the corner staircase.
      for (int sy = kTop; sy <= kBottom; ++sy) {
        int inset = 0;
        if (sy == kTop) inset = 2;
        else if (sy == kTop + 1) inset = 1;
        fillSpan(kLeft + inset, kLeft + kStroke - 1, sy);
        fillSpan(kRight - kStroke + 1, kRight - inset, sy);
      }
      // Right slant (NT → BR): 4-px x-band ending at the polygon centerline,
      // so the band sits inside the polygon and joins the right vertical at BR.
      for (int sy = kNotchTipY; sy <= kBottom; ++sy) {
        const int xc = kNotchTipX + (kRight - kNotchTipX) * (sy - kNotchTipY) / (kBottom - kNotchTipY);
        fillSpan(xc - kStroke + 1, xc, sy);
      }
      // Left slant (NT → BL): mirror — band starts at centerline and extends right.
      for (int sy = kNotchTipY; sy <= kBottom; ++sy) {
        const int xc = kNotchTipX - (kNotchTipX - kLeft) * (sy - kNotchTipY) / (kBottom - kNotchTipY);
        fillSpan(xc, xc + kStroke - 1, sy);
      }
    } else if (kHomeMenuItems[i].icon != nullptr) {
      drawIconScaled(renderer, kHomeMenuItems[i].icon, kIconSrcSize, iconX, iconY, iconSize);
    }
  };

  // Pass 1: paint every icon (no selection cell), then snapshot the
  // framebuffer so subsequent menu↔carousel toggles can restoreCoverBuffer()
  // and skip this loop entirely. The theme's earlier storeCoverBuffer caught
  // the framebuffer *before* these icons were drawn; we re-cache here so the
  // icons are baked in too.
  //
  // homeMenuIconsCached gates the redraw: it's false on first render and
  // after any cache invalidation (book change, freeCoverBuffer); once we
  // succeed below it stays true until the next invalidation event.
  if (!homeMenuIconsCached || !bufferRestored) {
    for (int i = 0; i < kHomeMenuCount; ++i) {
      drawSingleMenuIcon(i);
    }
    if (storeCoverBuffer()) {
      homeMenuIconsCached = true;
      coverBufferStored = true;
      coverRendered = true;
    }
  }

  // Menu label + subtitle overlay (only when an item is selected). Drawn
  // AFTER the icon cache so the cache stays mode-agnostic — restore brings
  // back the book's title/author, and we overlay the menu label here if
  // needed. When transitioning menu → carousel, this is a no-op and the
  // book's title/author from the restore stays visible.
  drawMenuOverlay();

  // Pass 2: selection cell + redraw of the selected icon. The fill uses a
  // dithered LightGray that overwrites icon pixels underneath, so the icon
  // we just baked into the cache has to be re-blitted on top each frame
  // anyway. Cheap (one icon vs six) and only runs when an item is selected.
  if (menuSelectedIndex >= 0 && menuSelectedIndex < kHomeMenuCount) {
    const int cellCenterX = iconCellCenterX(menuSelectedIndex);
    const int cellX = cellCenterX - iconCellSize / 2;
    renderer.fillRoundedRect(cellX, iconCellTopY, iconCellSize, iconCellSize, 6, Color::LightGray);
    drawSingleMenuIcon(menuSelectedIndex);
  }

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_PREV), tr(STR_DIR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onContinueReading() {
  if (!recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
  }
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onReadingStatsOpen() {
  // Replace home with stats so backing out lands on a fresh home with
  // selectorIndex reset to 0 — same behavior as Browse Files / Recent
  // Books / Settings, instead of the stack-preserved variant we had
  // before. The reader's own stats menu still uses startActivityForResult
  // (defined in EpubReaderActivity), so back from there returns to the
  // reader as expected.
  if (recentBooks.empty()) return;
  activityManager.replaceActivity(std::make_unique<BookStatsActivity>(
      renderer, mappedInput, recentBooks[0].path, recentBooks[0].title, recentBooks[0].coverBmpPath, currentBookStats,
      globalStats, /*backToHome=*/true));
}

void HomeActivity::onBookmarksOpen() {
  startActivityForResult(std::make_unique<BookmarksHomeActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}
