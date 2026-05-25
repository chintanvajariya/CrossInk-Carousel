#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraFlowTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
constexpr char kWidthPlaceholder[] = "[WIDTH]";
constexpr char kHeightPlaceholder[] = "[HEIGHT]";
constexpr size_t kWidthPlaceholderLength = sizeof(kWidthPlaceholder) - 1;
constexpr size_t kHeightPlaceholderLength = sizeof(kHeightPlaceholder) - 1;

// Returns the first existing `thumb_*.bmp` (or `cover.bmp`) in the book's
// cache directory, or an empty string if none are usable. Used as a last
// resort when neither the exact-dimensions thumb nor the legacy height-only
// thumb exists — picking up a stale thumb from an earlier firmware version
// is strictly better than the solid-black silhouette we'd otherwise draw.
//
// `exactPath` is the WxH thumb path we just confirmed doesn't exist; we
// derive the cache directory from it and scan that directory only — never
// the whole `.crosspoint/` tree, which would be O(books × files).
std::string findFallbackThumbInCacheDir(const std::string& exactPath) {
  const size_t lastSlash = exactPath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) return "";
  const std::string cacheDir = exactPath.substr(0, lastSlash);
  if (!Storage.exists(cacheDir.c_str())) return "";

  // Prefer cover.bmp first if present — it's the full 2-bit cover the reader
  // generates on first open, so it always reflects the current book even if
  // every thumb_*.bmp on disk is from an older firmware's size scheme.
  const std::string coverFallback = cacheDir + "/cover.bmp";
  if (Storage.exists(coverFallback.c_str())) return coverFallback;

  // Otherwise scan for any thumb_*.bmp. Don't pick a specific dimension —
  // the renderer will scale whatever it finds. We just want SOMETHING valid.
  auto dir = Storage.open(cacheDir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return "";
  }
  std::string found;
  char name[64];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const std::string entryName(name);
    file.close();
    // Match `thumb_*.bmp` only — skip `.cover.jpg`/`.cover.png` temp files,
    // book.bin, css_rules.cache, progress.bin, etc.
    if (entryName.rfind("thumb_", 0) == 0 && entryName.size() > 4 &&
        entryName.compare(entryName.size() - 4, 4, ".bmp") == 0) {
      found = cacheDir + "/" + entryName;
      break;
    }
  }
  dir.close();
  return found;
}
}  // namespace

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = Lyra3CoversMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_FLOW:
      LOG_DBG("UI", "Using Lyra Flow theme");
      currentTheme = std::make_unique<LyraFlowTheme>();
      currentMetrics = LyraFlowMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::MINIMAL:
      LOG_DBG("UI", "Using Minimal theme");
      currentTheme = std::make_unique<MinimalTheme>();
      currentMetrics = MinimalMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_CAROUSEL:
    default:
      LOG_ERR("UI", "Unknown / unregistered theme %d, falling back to Classic", static_cast<int>(type));
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = BaseMetrics::values;
      break;
  }

  // X4 ships with a lower top bezel, so the battery top bar lives 6 px farther
  // down (see BaseTheme::drawBatteryTopBar). Push every header rect and the
  // content rows below it down by the same 6 px on X4 so the gap between the
  // bar and the page title/body stays consistent with X3. We bake this into
  // topPadding because every header rect we draw is anchored to topPadding,
  // and every "content below header" calculation chains off topPadding too.
  if (gpio.deviceIsX4()) {
    currentMetrics.topPadding += 6;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int coverHeight) {
  if (coverHeight <= 0) {
    return "";
  }
  // Use int64_t so large heights cannot overflow before division.
  const int coverWidth = static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
  return getCoverThumbPath(coverBmpPath, coverWidth, coverHeight);
}

std::string UITheme::resolveExactCoverThumbPath(const std::string& coverBmpPath, int coverHeight) {
  if (coverHeight <= 0) return "";
  const int coverWidth = static_cast<int>((static_cast<int64_t>(coverHeight) * 3 + 2) / 5);
  // No-placeholder paths are taken as-is — caller already supplied a concrete
  // filename.
  const size_t widthPos = coverBmpPath.find(kWidthPlaceholder);
  const size_t heightPos = coverBmpPath.find(kHeightPlaceholder);
  if (widthPos == std::string::npos && heightPos == std::string::npos) {
    return coverBmpPath;
  }
  if (heightPos == std::string::npos) return "";

  std::string out = coverBmpPath;
  if (widthPos != std::string::npos) {
    out.replace(widthPos, kWidthPlaceholderLength, std::to_string(coverWidth));
  }
  const size_t pos = out.find(kHeightPlaceholder);
  if (pos != std::string::npos) {
    if (widthPos != std::string::npos) {
      // [WIDTH]x[HEIGHT] template → substitute [HEIGHT] with H.
      out.replace(pos, kHeightPlaceholderLength, std::to_string(coverHeight));
    } else {
      // Legacy [HEIGHT]-only template → expand to W×H so loadRecentCovers
      // gets a unique path per requested size (Flow at 392 and Minimal at
      // 583 should each generate their own thumb rather than sharing).
      out.replace(pos, kHeightPlaceholderLength, std::to_string(coverWidth) + "x" + std::to_string(coverHeight));
    }
  }
  return out;
}

std::string UITheme::getCoverThumbPath(const std::string& coverBmpPath, int width, int height) {
  if (width <= 0 || height <= 0) {
    return "";
  }
  const size_t initialWidthPos = coverBmpPath.find(kWidthPlaceholder, 0);
  const size_t initialHeightPos = coverBmpPath.find(kHeightPlaceholder, 0);
  const bool hasWidthPlaceholder = initialWidthPos != std::string::npos;
  const bool hasHeightPlaceholder = initialHeightPos != std::string::npos;

  if (!hasWidthPlaceholder && !hasHeightPlaceholder) {
    return coverBmpPath;
  }
  if ((hasWidthPlaceholder &&
       coverBmpPath.find(kWidthPlaceholder, initialWidthPos + kWidthPlaceholderLength) != std::string::npos) ||
      (hasHeightPlaceholder &&
       coverBmpPath.find(kHeightPlaceholder, initialHeightPos + kHeightPlaceholderLength) != std::string::npos)) {
    return "";
  }
  if (!hasHeightPlaceholder) {
    return "";
  }

  // Build the exact W×H path (the preferred file the renderer will look for).
  std::string thumbPath = coverBmpPath;
  if (hasWidthPlaceholder) {
    const size_t widthPos = thumbPath.find(kWidthPlaceholder, 0);
    thumbPath.replace(widthPos, kWidthPlaceholderLength, std::to_string(width));
  }
  const size_t pos = thumbPath.find(kHeightPlaceholder, 0);
  if (pos != std::string::npos) {
    if (hasWidthPlaceholder) {
      // Template was `…thumb_[WIDTH]x[HEIGHT].bmp` → substitute `[HEIGHT]`.
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(height));
    } else {
      // Template was legacy `…thumb_[HEIGHT].bmp` → substitute as W×H so the
      // current build's exact path is generated (legacy form acts as fallback
      // below).
      thumbPath.replace(pos, kHeightPlaceholderLength, std::to_string(width) + "x" + std::to_string(height));
    }
  }

  // Also build the legacy `thumb_<H>.bmp` path so we can pick it up if the
  // exact W×H file isn't on disk yet. Reading from coverBmpPath directly
  // (rather than mutating the already-substituted thumbPath) makes this
  // robust regardless of which template style the book was saved with.
  std::string legacyPath = coverBmpPath;
  if (hasWidthPlaceholder) {
    // Drop "[WIDTH]x" (or "[WIDTH]" if there's no separator) so the legacy
    // filename collapses to `thumb_<H>.bmp`. Only consume the 'x' when it's
    // actually present, so unrelated templates aren't corrupted.
    const size_t widthPos = legacyPath.find(kWidthPlaceholder, 0);
    size_t eraseLen = kWidthPlaceholderLength;
    if (widthPos + kWidthPlaceholderLength < legacyPath.size() &&
        legacyPath[widthPos + kWidthPlaceholderLength] == 'x') {
      eraseLen += 1;
    }
    legacyPath.erase(widthPos, eraseLen);
  }
  const size_t legacyHeightPos = legacyPath.find(kHeightPlaceholder, 0);
  if (legacyHeightPos != std::string::npos) {
    legacyPath.replace(legacyHeightPos, kHeightPlaceholderLength, std::to_string(height));
  }

  // Resolution chain: exact W×H → legacy H-only → any thumb in cache dir →
  // cover.bmp (full-size 2-bit) → exact path (for generation).
  if (Storage.exists(thumbPath.c_str())) return thumbPath;
  if (legacyPath != thumbPath && Storage.exists(legacyPath.c_str())) return legacyPath;
  const std::string fallback = findFallbackThumbInCacheDir(thumbPath);
  if (!fallback.empty()) return fallback;
  return thumbPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

// The new reader progress bar floats 15 px above the screen bottom (track at
// y=screenH-13, fill at y=screenH-15), so we reserve 15 px of vertical space
// for it instead of the old thickness-driven calculation.
namespace {
constexpr int kFloatingProgressBarReserve = 15;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
                             SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                             SETTINGS.statusBarBattery;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) + (showProgressBar ? kFloatingProgressBarReserve : 0);
}

int UITheme::getProgressBarHeight() {
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return showProgressBar ? kFloatingProgressBarReserve : 0;
}
