#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Flow ("iPod-style") carousel theme. Inherits LyraTheme wholesale and only
// overrides the home-screen recent-book carousel. Ported from the Lua-fork
// FlowTheme; date / today-clock / per-book reading-time chrome have been
// removed because their stats subsystems do not exist in this firmware.
namespace LyraFlowMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  // X3-tuned values: cover (392 tall) + tight progress bar + title + author.
  // The rect bottom is laid out to match the home menu icon-strip top
  // (HomeActivity computes that at pageHeight − buttonHintsHeight − 16 −
  // iconCellSize = 792 − 40 − 16 − 56 = 680; with topPadding = 41 the rect
  // height that aligns to it is 680 − 41 = 639). Centering title/author
  // inside the rect therefore equals centering them between the time row
  // and the menu icon-strip top.
  v.homeCoverHeight = 392;
  v.homeCoverTileHeight = 639;
  v.homeRecentBooksCount = 5;    // matches the 5 carousel slots visible at once
                                 // (center + 2 sides each direction). Capped at 5 to
                                 // avoid first-boot OOM during sequential thumb gen on
                                 // ESP32-C3 — see HomeActivity::loadRecentCovers.
  v.homeTopPadding = 41;         // header height unchanged from X4 — battery icon at
                                 // y+5 doesn't need more vertical room on the wider
                                 // panel, and keeping the header tight preserves
                                 // menu space below the (taller) carousel.
  v.homeMenuTopOffset = 16;      // No more time-read line under the cover, so
                                 // this gap collapses to a standard 16 px
                                 // (matches metrics.verticalSpacing).
  return v;
}();
}  // namespace LyraFlowMetrics

class LyraFlowTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f,
                           const std::vector<std::vector<uint8_t>>* thumbDataBuffers = nullptr,
                           const std::vector<DecodedThumb>* decodedThumbs = nullptr) const override;
  // Flow-only override of the home menu. Two-anchor pagination with a
  // sticky bit so the second page stays in view as the cursor scrolls
  // back up through the overlap zone — switches back to page 1 only
  // when the cursor crosses page 2's top boundary.
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

 private:
  // Tracks "is page 2 currently shown" across renders. mutable because
  // drawButtonMenu is a const method (theme contract). State is purely
  // a UX hint; resets itself whenever the cursor lands unambiguously
  // inside page 1's exclusive zone.
  mutable bool stickyMenuPage2 = false;

  // Cached geometry of the most recently rendered center cover. Set inside
  // the !coverRendered branch of drawRecentBookCover; reused outside that
  // branch to redraw the selection border per frame without re-running the
  // SD-heavy cover load. mutable for the same reason as stickyMenuPage2.
  mutable int cachedCenterCoverX = 0;
  mutable int cachedCenterCoverY = 0;
  mutable int cachedActualCoverWidth = 0;
  mutable int cachedActualCoverHeight = 0;
};
