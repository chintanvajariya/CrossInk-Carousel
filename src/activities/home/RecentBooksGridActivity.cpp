#include "RecentBooksGridActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "fontIds.h"
#include "util/ProgressRingUtil.h"

void RecentBooksGridActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), MAX_BOOKS));

  for (const auto& book : books) {
    if (static_cast<int>(recentBooks.size()) >= MAX_BOOKS) break;
    // Skip if file is gone — keeps stale entries from showing up as
    // permanently-broken cover slots. (We don't delete from the store
    // because RecentBooksStore has no public remove API.)
    if (!Storage.exists(book.path.c_str())) continue;
    recentBooks.push_back(book);
  }
}

void RecentBooksGridActivity::loadPageCovers(int pageStart) {
  const int pageEnd = std::min(pageStart + BOOKS_PER_PAGE, static_cast<int>(recentBooks.size()));

  // Cheap pre-pass: if every visible thumb already exists on disk, skip the
  // heavy Epub/Xtc load entirely (no popup, no flicker).
  bool needsGeneration = false;
  for (int i = pageStart; i < pageEnd; ++i) {
    if (recentBooks[i].coverBmpPath.empty()) continue;
    // Exact-size existence check (no fallback chain) — same rationale as
    // HomeActivity::loadRecentCovers. We want regen to fire when the thumb
    // at this view's COVER_HEIGHT isn't on disk, not when ANY thumb size
    // happens to be on disk.
    const std::string thumbPath = UITheme::resolveExactCoverThumbPath(recentBooks[i].coverBmpPath, COVER_HEIGHT);
    if (!Storage.exists(thumbPath.c_str())) {
      needsGeneration = true;
      break;
    }
  }
  if (!needsGeneration) {
    loadedPageStart = pageStart;
    return;
  }

  bool showingLoading = false;
  Rect popupRect;
  const int totalToProcess = pageEnd - pageStart;
  int processedCount = 0;

  for (int i = pageStart; i < pageEnd; ++i) {
    RecentBook& book = recentBooks[i];
    // Self-heal: if a prior pass cleared coverBmpPath, re-derive the template
    // so this pass can retry generation. Mirrors the same recovery branch in
    // HomeActivity::loadRecentCovers — the two paths must agree or one of
    // them strands books in the no-cover state forever.
    if (book.coverBmpPath.empty()) {
      RecentBook refreshed = RECENT_BOOKS.getDataFromBook(book.path);
      if (!refreshed.coverBmpPath.empty()) {
        book.coverBmpPath = refreshed.coverBmpPath;
        (void)RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
      }
    }
    // Drive the regen decision off the exact requested size; the render
    // path below still goes through getCoverThumbPath with its fallback
    // chain so we don't show solid-black during regeneration.
    const std::string coverPath =
        book.coverBmpPath.empty() ? "" : UITheme::resolveExactCoverThumbPath(book.coverBmpPath, COVER_HEIGHT);
    if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
      if (FsHelpers::hasEpubExtension(book.path)) {
        Epub epub(book.path, "/.crosspoint");
        // load(false, true) = skip CSS, metadata only — same flags HomeActivity uses.
        if (epub.load(false, true)) {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + processedCount * (90 / totalToProcess));
          if (!epub.generateThumbBmp(COVER_HEIGHT)) {
            // Keep coverBmpPath intact so the next visit retries. A single
            // failure was previously persisted as coverBmpPath="" and left
            // the book showing the no-cover placeholder forever.
            LOG_ERR("RBG", "generateThumbBmp(%d) failed for %s; will retry on next visit",
                    static_cast<int>(COVER_HEIGHT), book.path.c_str());
          }
        }
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        Xtc xtc(book.path, "/.crosspoint");
        if (xtc.load()) {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + processedCount * (90 / totalToProcess));
          if (!xtc.generateThumbBmp(COVER_HEIGHT)) {
            LOG_ERR("RBG", "xtc generateThumbBmp(%d) failed for %s; will retry on next visit",
                    static_cast<int>(COVER_HEIGHT), book.path.c_str());
          }
        }
      }
    }
    processedCount++;
  }

  loadedPageStart = pageStart;
  if (showingLoading) {
    // Force a redraw so the freshly-generated covers replace the popup
    // and any placeholder slots painted earlier.
    requestUpdate();
  }
}

void RecentBooksGridActivity::onEnter() {
  Activity::onEnter();
  loadRecentBooks();
  selectorIndex = 0;
  loadedPageStart = -1;
  requestUpdate();
}

void RecentBooksGridActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksGridActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < recentBooks.size()) {
      LOG_DBG("RBGA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  // Continuous-press jumps a full row (3 covers) for fast grid traversal,
  // matching the Lua reference.
  constexpr int ROW_STEP = 3;
  buttonNavigator.onNextContinuous([this, listSize] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, ROW_STEP);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, ROW_STEP);
    requestUpdate();
  });
}

void RecentBooksGridActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header doubles as the selected-book label. Pass "" so drawHeader still
  // clears the rect, paints the battery top bar, and lays down the underline,
  // but renders no title text — we draw the title (bold) and author (regular,
  // same UI_12 size) inline below so they share a single line.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "");

  if (selectorIndex < recentBooks.size()) {
    const auto& sel = recentBooks[selectorIndex];
    const int titleX = metrics.contentSidePadding;
    // Title shifted up 1 px from the LyraTheme default (+3) — visually the
    // text was sitting closer to the underline than to the battery bar above
    // it. drawText interprets `y` as the top of the bbox, but ascender +
    // descender + leading make the visible glyph center sit slightly below
    // that, so a -1 nudge brings the optical center to the middle.
    const int titleY = metrics.topPadding + metrics.batteryBarHeight + 2;

    // Pie-style progress ring on the right edge, mirrored padding from the
    // right of the screen so it sits opposite the title's left padding.
    if (cachedProgressIndex != selectorIndex) {
      cachedProgressPercent = RecentBookProgress::loadPercent(sel);
      cachedProgressIndex = selectorIndex;
    }
    // Center the ring against the line (advanceY) rather than the ascender
    // height — ascender misses the descender + leading, which would pull the
    // ring's optical center up by ~2 px and make it sit closer to the top
    // bar than the bottom bar. Diameter bumped by +4 (radius +2) and band
    // thickness +2 to a 7-px band, both per the design tweak.
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    const int discRadius = std::max(6, (lineH - 2) / 2 + 1);
    constexpr int kDiscGap = 6;  // breathing room between text and disc
    const int discRightEdge = pageWidth - metrics.contentSidePadding;
    const int discCx = discRightEdge - discRadius;
    const int discCy = titleY + lineH / 2;
    ProgressRingUtil::drawProgressRing(renderer, discCx, discCy, discRadius, cachedProgressPercent);

    // Keep the title/author starting at contentSidePadding (unchanged). Just
    // pull the text-width budget in by the disc footprint so long titles can't
    // truncate into the disc.
    const int budget = (pageWidth - metrics.contentSidePadding * 2) - (2 * discRadius + kDiscGap);
    constexpr int gap = 12;  // separation between title and author

    const int authorRawW = sel.author.empty()
                               ? 0
                               : renderer.getTextWidth(UI_12_FONT_ID, sel.author.c_str(), EpdFontFamily::REGULAR);
    const int authorReserved = sel.author.empty() ? 0 : (gap + authorRawW);
    // Reserve at least 1/3 of the budget for the title so a very long author
    // name can't squeeze the title down to 0 px (in which case the author
    // would visually span the whole row and the title would vanish). Author
    // gets truncated instead. When the author is short, the title still
    // expands to fill whatever space the author doesn't use.
    const int titleMinW = sel.author.empty() ? 0 : (budget / 3);
    const int titleMaxW = std::min(budget, std::max(titleMinW, budget - authorReserved));

    const std::string truncTitle =
        renderer.truncatedText(UI_12_FONT_ID, sel.title.c_str(), titleMaxW, EpdFontFamily::BOLD);
    const int truncTitleW = renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, titleX, titleY, truncTitle.c_str(), true, EpdFontFamily::BOLD);

    if (!sel.author.empty()) {
      const int authorMaxW = std::max(0, budget - truncTitleW - gap);
      // Bail out of drawing the author if there's no usable space — better
      // than crowding a single-letter ellipsis next to the title.
      if (authorMaxW > 20) {
        const std::string truncAuthor =
            renderer.truncatedText(UI_12_FONT_ID, sel.author.c_str(), authorMaxW, EpdFontFamily::REGULAR);
        renderer.drawText(UI_12_FONT_ID, titleX + truncTitleW + gap, titleY, truncAuthor.c_str(), true);
      }
    }
  } else {
    // Fallback: empty list — render the screen heading we'd otherwise replace.
    const int titleX = metrics.contentSidePadding;
    const int titleY = metrics.topPadding + metrics.batteryBarHeight + 3;
    renderer.drawText(UI_12_FONT_ID, titleX, titleY, tr(STR_MENU_RECENT_BOOKS), true, EpdFontFamily::BOLD);
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  constexpr int columns = 3;
  // 14 px gap reused for column AND row spacing — sized in tandem with the
  // 136×204 COVER_WIDTH/COVER_HEIGHT so the total grid width stays at
  // 3·136 + 2·14 = 436 (identical to the previous 140-wide + 8-gap layout).
  // Covers come out a touch smaller; the extra 6 px goes to breathing room
  // between them.
  constexpr int gridSpacing = 14;
  const int rowSpacing = gridSpacing;
  const int totalGridWidth = columns * COVER_WIDTH + (columns - 1) * gridSpacing;
  const int startXOffset = (pageWidth - totalGridWidth) / 2;

  const int totalBooks = static_cast<int>(recentBooks.size());
  const int totalPages = (totalBooks + BOOKS_PER_PAGE - 1) / BOOKS_PER_PAGE;
  const int currentPage = (totalBooks > 0) ? (static_cast<int>(selectorIndex) / BOOKS_PER_PAGE) : 0;
  const int pageStart = currentPage * BOOKS_PER_PAGE;
  const int pageCount = std::min(BOOKS_PER_PAGE, totalBooks - pageStart);

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    // Tracks the last loaded book's actual rendered dimensions so empty
    // grid slots on the same page can match (otherwise empty boxes drawn
    // at COVER_WIDTH × COVER_HEIGHT look bigger than the aspect-fit covers).
    int sampleBw = COVER_WIDTH;
    int sampleBh = COVER_HEIGHT;
    bool haveSample = false;

    for (int i = 0; i < pageCount; ++i) {
      const int bookIdx = pageStart + i;
      const int col = i % columns;
      const int row = i / columns;
      const int x = startXOffset + col * (COVER_WIDTH + gridSpacing);
      const int y = contentTop + row * (COVER_HEIGHT + rowSpacing);

      // Per-cell render bounds. Default to full cell for the placeholder
      // path; if a real cover loads we shrink these to the cover's true
      // aspect so the outline + selection border hug the cover and don't
      // leave a white sliver on books that aren't 123:180.
      int bx = x;
      int by = y;
      int bw = COVER_WIDTH;
      int bh = COVER_HEIGHT;
      bool drawn = false;
      const std::string thumbPath =
          recentBooks[bookIdx].coverBmpPath.empty()
              ? ""
              : UITheme::getCoverThumbPath(recentBooks[bookIdx].coverBmpPath, COVER_HEIGHT);
      if (!thumbPath.empty() && Storage.exists(thumbPath.c_str())) {
        FsFile file;
        if (Storage.openFileForRead("RBGA", thumbPath, file)) {
          Bitmap bmp(file);
          if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
            bw = std::min(COVER_WIDTH, bmp.getWidth());
            bh = std::min(COVER_HEIGHT, bmp.getHeight());
            bx = x + (COVER_WIDTH - bw) / 2;
            by = y + (COVER_HEIGHT - bh) / 2;
            renderer.drawBitmap(bmp, bx, by, bw, bh);
            renderer.drawRect(bx, by, bw, bh, 2, true);
            drawn = true;
            sampleBw = bw;
            sampleBh = bh;
            haveSample = true;
          }
          file.close();
        }
      }
      if (!drawn) {
        // Empty cover slot: outlined box with a centered book icon.
        renderer.drawRect(bx, by, bw, bh, 2, true);
        renderer.fillRect(bx + 2, by + 2, bw - 4, bh - 4, false);
        renderer.drawIcon(BookIcon, bx + (bw - 32) / 2, by + (bh - 32) / 2, 32, 32);
      }
      if (bookIdx == static_cast<int>(selectorIndex)) {
        renderer.drawRect(bx - 2, by - 2, bw + 4, bh + 4, 3, true);
      }
    }

    // Empty grid slots get a thinner 1 px border (so the actual books'
    // 2 px border reads as the dominant element). Sized to match the
    // sampled book footprint so empty boxes don't look bigger than the
    // aspect-fit covers around them; falls back to a 0.66 ratio if no
    // book on this page rendered.
    if (!haveSample) {
      sampleBh = COVER_HEIGHT;
      sampleBw = (COVER_HEIGHT * 66) / 100;
    }
    const int sampleXInset = (COVER_WIDTH - sampleBw) / 2;
    const int sampleYInset = (COVER_HEIGHT - sampleBh) / 2;
    for (int i = pageCount; i < BOOKS_PER_PAGE; ++i) {
      const int col = i % columns;
      const int row = i / columns;
      const int px = startXOffset + col * (COVER_WIDTH + gridSpacing) + sampleXInset;
      const int py = contentTop + row * (COVER_HEIGHT + rowSpacing) + sampleYInset;
      renderer.drawRect(px, py, sampleBw, sampleBh, 1, true);
    }

    if (totalPages > 1) {
      constexpr int dotSize = 10;
      constexpr int dotSpacing = 8;
      const int totalDotWidth = totalPages * dotSize + (totalPages - 1) * dotSpacing;
      const int dotsStartX = (pageWidth - totalDotWidth) / 2;
      const int dotY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
      constexpr int dotRadius = dotSize / 2;  // 5 → fully-circular bullet on 10x10
      for (int p = 0; p < totalPages; p++) {
        const int dx = dotsStartX + p * (dotSize + dotSpacing);
        if (p == currentPage) {
          renderer.fillRoundedRect(dx, dotY, dotSize, dotSize, dotRadius, Color::Black);
        } else {
          renderer.drawRoundedRect(dx, dotY, dotSize, dotSize, 1, dotRadius, true);
        }
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_PREV), tr(STR_DIR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  // Defer cover generation until AFTER displayBuffer so the user sees
  // placeholder slots immediately when paging — then the popup appears and
  // covers stream in. Same ordering as the Lua reference.
  if (!recentBooks.empty() && loadedPageStart != pageStart) {
    loadPageCovers(pageStart);
  }
}
