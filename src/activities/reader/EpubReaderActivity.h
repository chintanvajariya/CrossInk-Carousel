#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <optional>
#include <string>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  unsigned long sessionStartMs = 0UL;
  // Set by onAutoSleepImminent (called from ActivityManager::goToSleep when
  // an auto-timeout sleep is about to replace this activity). onExit checks
  // this and discounts the time since the last page turn from the session
  // stats — that idle tail is wall-clock time, not reading time.
  bool autoSleepImminent_ = false;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool longPressMenuHandled = false;
  bool longPowerButtonHandled = false;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  int pageLoadRetryCount = 0;
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
  };
  bool pendingBookmarkFeedback = false;
  BookmarkFeedbackType bookmarkFeedbackType = BookmarkFeedbackType::Added;
  unsigned long bookmarkFeedbackShowTime = 0UL;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  int completionTriggerSpineIndex = -1;
  float completionTriggerSpineProgress = 1.0f;
  bool completionPromptQueued = false;
  bool completionPromptShown = false;
  bool completionTriggerSeenBelow = false;
  bool lastAtOrPastCompletionTrigger = false;
  bool pendingReadFolderMove = false;

  struct ReadFolderMoveParams {
    std::string epubPath;
    std::string dstEpubPath;
    std::string cachePath;
    std::string title;
  };
  static void readFolderMoveTask(void* arg);

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  // Pre-rendered next-page buffer (ported from buffer-crosspoint-reader 1.44).
  // After the current page is fully drawn and displayed, the next text-only
  // page is silently rendered into the frame buffer behind the user's back —
  // the next forward page turn then skips the prewarm+page->render pass and
  // just superimposes the status bar + flushes to the display, saving ~200 ms
  // per page turn. The framebuffer's last actually-displayed contents stay on
  // the e-ink panel unchanged, since e-ink retains the previous image and
  // we never call displayBuffer() during the pre-render pass.
  struct PreRenderedPage {
    bool ready = false;
    int spineIndex = -1;
    int pageIndex = -1;
  };
  PreRenderedPage preRenderedPage;
  // Set by the tail of render() to request a pre-render pass on the next tick.
  bool pendingPreRender = false;
  // Set by pageTurn() when the buffered next page matches the requested page;
  // tells render() the framebuffer already holds content and only status bar
  // + display flush are needed.
  bool usePreRenderedBuffer = false;
  // Render page content (prewarm + clear + page->render) WITHOUT touching the
  // status bar or pushing displayBuffer. Used by the pre-render pass.
  void renderPageContentOnly(const Page& page, int orientedMarginTop, int orientedMarginRight,
                             int orientedMarginBottom, int orientedMarginLeft);
  // Superimpose status bar over the already-populated framebuffer, flush via
  // the refresh cycle, then run the grayscale AA pass for text-only pages.
  void displayPreRenderedPage(const Page& page, int orientedMarginTop, int orientedMarginRight,
                              int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void openFileTransfer();
  void openAutoPageTurnIntervalPicker(bool ignoreInitialConfirmRelease = false);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void reindexCurrentSection();
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action);
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void executeLongPressMenuAction();
  void pageTurn(bool isForwardTurn);
  float getCurrentBookProgressPercent() const;
  void initializeCompletionPromptTrigger();
  bool isAtOrPastCompletionTrigger() const;
  void queueCompletionPromptIfNeeded();
  void setBookCompleted(bool isCompleted);
  void showCompletedFeedback(bool isCompleted);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool isReaderActivity() const override { return true; }
  void onAutoSleepImminent() override { autoSleepImminent_ = true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
