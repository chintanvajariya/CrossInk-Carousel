#pragma once
#include <GfxRenderer.h>

#include <functional>
#include <vector>

#include "../Activity.h"
#include "../reader/BookReadingStats.h"
#include "../reader/GlobalReadingStats.h"
#include "./FileBrowserActivity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  // Remembered carousel position so Left/Right re-entry from the menu lands
  // on the same book the user was last on, not back at index 0.
  int lastBookIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsUrl = false;
  bool hasReadingStats = false;
  bool hasBookmarks = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  // Which carousel book the cached buffer was captured for. Render runs on a
  // separate FreeRTOS task from loop(); if the user scrolls (mutating
  // selectorIndex) while a render is in flight, the framebuffer that gets
  // cached at the end of that render is for the OLD book. We tag the buffer
  // with the idx it captured and invalidate at the start of the next render
  // when it doesn't match the book about to be drawn.
  int coverBufferBookIdx = -1;
  int pendingCoverBufferBookIdx = -1;
  // True when the cover buffer cache currently includes the home menu icon
  // strip pixels (drawn by render() AFTER the theme's drawRecentBookCover).
  // The theme caches covers+chrome only; HomeActivity re-caches over that
  // capture with the icons baked in, so subsequent menu↔carousel toggles can
  // restoreCoverBuffer once and skip the per-frame icon redraw.
  // Reset by freeCoverBuffer() so any invalidation forces a fresh icon pass.
  bool homeMenuIconsCached = false;
  // Minimal-theme home UX state (upstream parity). The Minimal home shows a
  // 4-slot front-button hint row (MENU / BROWSE / SETTINGS / READ); pressing
  // MENU opens an overlay containing buildMinimalMenuItems(). On theme switch
  // away from Minimal these flags are harmlessly inert.
  bool minimalMenuOpen = false;
  int minimalMenuIndex = 0;
  int minimalHomeNavIndex = -1;
  bool minimalSuppressInitialFrontRelease = false;
  float currentBookProgressPercent = -1.0f;
  BookReadingStats currentBookStats;
  GlobalReadingStats globalStats;
  std::vector<RecentBook> recentBooks;
  // Per-book stats for the carousel covers. Populated in onEnter() so the
  // Flow theme's "hours read" indicator can update as the user scrolls
  // sideways through the carousel without doing file I/O per render.
  // Same index as recentBooks; default-constructed when no stats file exists.
  std::vector<BookReadingStats> recentBookStats;
  // Per-book progress percent, indexed parallel to recentBooks. -1.0f when
  // we couldn't resolve a progress.bin for a book. Eagerly loaded in onEnter
  // so the Flow theme's per-book progress bar can update as the user scrolls
  // the carousel without on-demand file I/O.
  std::vector<float> recentBookProgress;
  // RAM-cached thumbnail BMP bytes, indexed parallel to recentBooks. Slurped
  // off the SD card once at the end of loadRecentCovers so the per-render
  // carousel render uses a memory-backed Bitmap and avoids 5 file opens +
  // sequential reads on every scroll. Empty when caching failed (file too
  // large, OOM, etc.) — the theme falls back to file-backed reads in that
  // case. Cap per-entry size with kMaxThumbCacheBytes to bound RAM use.
  std::vector<std::vector<uint8_t>> recentBookThumbData;
  // Pre-decoded 1-bit pixel grids, indexed parallel to recentBookThumbData.
  // Each entry is the result of parsing the BMP + unpacking 1-bit pixels into
  // a flat MSB-first grid. The theme renders directly from the grid via
  // drawBitmap1BitFromGrid / drawPerspectiveFromGrid, skipping the BMP header
  // parse + per-row 2-bit-quantization that drawBitmap / drawPerspectiveBitmap
  // would otherwise pay on every carousel scroll. Empty (invalid()) when decode
  // failed — the theme falls back to the byte cache or file path in that case.
  std::vector<DecodedThumb> recentBookDecodedThumbs;
  // Per-thumb cap. Lowered from 32 KB to 24 KB so even a tightly fragmented
  // heap at first-home-enter has a reasonable shot at the contiguous block.
  static constexpr size_t kMaxThumbCacheBytes = 24 * 1024;
  void loadRecentThumbsToRam(int coverHeight);
  void onSelectBook(const std::string& path);
  void onContinueReading();
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onReadingStatsOpen();
  void onBookmarksOpen();

  int getMenuItemCount() const;
  // Visible/navigable recent-books count. Caps at 3 in Flow's 3-cover mode so
  // selectorIndex math (carousel↔menu transitions, Confirm dispatch, render's
  // centeredBookIdx) stays consistent — otherwise selectorIndex 3-4 would
  // resolve as "book index 3/4" in some code paths and "first/second menu
  // icon" in others. Returns recentBooks.size() in all other configurations.
  int navigableRecentCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Home", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
