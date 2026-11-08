/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

/**
 * @file
 * @brief IGraphics platform layer for Linux: X11 window + GLX context + NanoVG.
 *
 * Design notes (VoLum Linux port)
 * -------------------------------
 * - The plug-in creates its own window inside the host-provided container
 *   (XEMBED) and falls back to a top-level window when no usable parent
 *   is supplied.
 * - All Xlib calls for a window happen on one thread. That thread also owns
 *   the GLX context, so event handling and drawing share a single loop paced
 *   at the graphics class' FPS. This mirrors the Windows layer (window thread
 *   plus vblank pump) and the macOS layer (main run loop).
 * - The drawing backend is fixed by the fork's IGraphics_select.h:
 *   IGRAPHICS_NANOVG with IGRAPHICS_GL2, using the system libGL entry points
 *   (no glad on Linux, so the GLX/Xlib types and the GL types cannot collide).
 */

#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "IPlugPlatform.h" // defines OS_LINUX before IGraphics_select.h picks the GL headers
#include "IGraphics_select.h"

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

/** IGraphics platform class for Linux (X11 / XWayland + GLX).
 * @ingroup PlatformClasses */
class IGraphicsLinux final : public IGRAPHICS_DRAW_CLASS
{
public:
  IGraphicsLinux(IGEditorDelegate& dlg, int w, int h, int fps, float scale);
  ~IGraphicsLinux() override;

  // MARK: window lifecycle
  void* OpenWindow(void* pParent) override;
  void CloseWindow() override;
  bool WindowIsOpen() override { return mXWindow != 0; }
  void* GetWindow() override { return reinterpret_cast<void*>(mXWindow); }
  const char* GetPlatformAPIStr() override { return "x11"; }

  void PlatformResize(bool parentHasResized) override;
  void DrawResize() override;
  float GetPlatformWindowScale() const override { return GetScreenScale(); }

  // MARK: mouse / cursor
  void GetMouseLocation(float& x, float& y) const override;
  void HideMouseCursor(bool hide, bool lock) override;
  void MoveMouseCursor(float x, float y) override;
  ECursor SetMouseCursor(ECursor cursorType) override;

  // MARK: editing, dialogs, misc
  void ForceEndUserEdit() override;
  void UpdateTooltips() override {}
  bool PlatformSupportsMultiTouch() const override { return false; }

  EMsgBoxResult ShowMessageBox(const char* str, const char* caption, EMsgBoxType type,
                               IMsgBoxCompletionHandlerFunc completionHandler) override;
  void PromptForFile(WDL_String& fileName, WDL_String& path, EFileAction action, const char* ext,
                     IFileDialogCompletionHandlerFunc completionHandler) override;
  void PromptForFiles(WDL_String& path, std::vector<WDL_String>& fileNames, const char* ext) override;
  void PromptForDirectory(WDL_String& dir, IFileDialogCompletionHandlerFunc completionHandler) override;
  bool PromptForColor(IColor& color, const char* str, IColorPickerHandlerFunc func) override;
  bool OpenURL(const char* url, const char* msgWindowTitle, const char* confirmMsg,
               const char* errMsgOnFailure) override;
  bool RevealPathInExplorerOrFinder(WDL_String& path, bool select) override;

  // MARK: clipboard
  bool GetTextFromClipboard(WDL_String& str) override;
  bool SetTextInClipboard(const char* str) override;
  bool SetFilePathInClipboard(const char* path) override;

  // MARK: fonts: NanoVG keeps its own glyph atlas, no system font handles needed
  PlatformFontPtr LoadPlatformFont(const char* fontID, const char* fileNameOrResID) override;
  PlatformFontPtr LoadPlatformFont(const char* fontID, const char* fontName, ETextStyle style) override;
  PlatformFontPtr LoadPlatformFont(const char* fontID, void* pData, int dataSize) override;
  void CachePlatformFont(const char* fontID, const PlatformFontPtr& font) override;

  // MARK: platform widgets
  IPopupMenu* CreatePlatformPopupMenu(IPopupMenu& menu, const IRECT bounds, bool& isAsync) override;
  void CreatePlatformTextEntry(int paramIdx, const IText& text, const IRECT& bounds, int length,
                               const char* str) override;

  // MARK: GL context
  void ActivateGLContext() override;
  void DeactivateGLContext() override;

private:
  bool ChooseVisual();
  void CreateGLContext();
  void DestroyGLContext();
  void DestroyWindow();
  void StopUIThread();

  /** Window thread body: take over the GL context, pump X events, then redraw when dirty. */
  void UIRoop();
  void PumpEvents();
  bool HandleEvent(void* xevent);
  void OnDisplayTimer();
  void CaptureFrameIfNeeded();          // VOLUM_LINUX_CAPTURE=/tmp/frame -> .N.ppm

  /** Run a task on the window thread (the thread that owns the GL context). Tasks posted
   * from other threads - the iPlug2 idle timer, for one - are drained by PumpEvents(). */
  void PostToUIThread(std::function<void()> task);
  void DrainUITasks();
  void RegisterTimerDispatcher(bool registerDispatcher);

  int PixelWidth() const;
  int PixelHeight() const;

  IMouseInfo GetMouseInfo(float pixelX, float pixelY, unsigned int keyMask) const;
  unsigned long CursorFor(ECursor cursorType);

  /** SelectionRequest event (XSelectionRequestEvent*), served from mClipText. */
  void ServeClipboardRequest(void* pRequestEvent);
  /** SelectionNotify event for our clipboard scratch window. */
  void ReadClipboardReply(void* pEvent);

  // X11 / GLX state kept opaque here so Xlib headers stay out of plug-in sources.
  void* mDisplay = nullptr;          // Display*
  unsigned long mXWindow = 0;        // Window
  unsigned long mParentWindow = 0;   // Window
  unsigned long mRootWindow = 0;     // Window
  unsigned long mColormap = 0;       // Colormap
  void* mGLXFBConfig = nullptr;      // GLXFBConfig
  void* mGLXContext = nullptr;       // GLXContext
  void* mInputMethod = nullptr;      // XIM
  void* mInputContext = nullptr;     // XIC
  bool mEmbedded = false;            // reparented into a host container
  int mScreen = 0;
  int mDepth = 24;
  int mXOffset = 0;                  // top-level window position
  int mYOffset = 0;

  int mFPS = 60;
  int mLastWidth = 0;
  int mLastHeight = 0;
  bool mGLCurrent = false;
  bool mGLInitPending = false;

  std::thread mUIThread;
  std::atomic<bool> mUIThreadRun { false };

  // Tasks handed over by other threads, run on the window thread.
  std::deque<std::function<void()>> mUITasks;
  std::mutex mUITaskMutex;
  std::atomic<bool> mTimerDispatcherRegistered { false };
  std::atomic<bool> mTimerTickHandoff { false }; // first tick delivered through the UI queue
  std::thread::id mUIThreadId {};                // thread that owns the window and GL context

  ECursor mCursorType = ECursor::ARROW;
  bool mCursorHidden = false;
  void* mPrevXErrorHandler = nullptr; // Xlib error handler owned by the host, restored on close
  unsigned long mBlankCursor = 0;    // Cursor
  std::unordered_map<int, unsigned long> mCursors;
  mutable float mMouseX = 0.f;
  mutable float mMouseY = 0.f;

  // Manual double-click detection: X11 reports the second press as a plain press too,
  // and iPlug2 wants OnMouseDblClick instead of a second OnMouseDown.
  unsigned int mLastClickTime = 0;
  int mLastClickButton = 0;
  int mLastClickX = 0;
  int mLastClickY = 0;

  // CLIPBOARD ownership + the scratch window used when we request the selection.
  std::string mClipText;
  unsigned long mClipWindow = 0;     // Window
  std::string mClipReply;
  bool mClipReplyDone = false;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
