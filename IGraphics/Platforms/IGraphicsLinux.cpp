/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

/**
 * @file
 * @brief Linux (X11 / GLX / NanoVG) implementation of the IGraphics platform layer.
 *
 * Everything Xlib-related lives in this translation unit; the header keeps the
 * X11 types opaque so plug-in sources never see <X11/Xlib.h>.
 */

#include "IGraphicsLinux.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <unistd.h>
#include <sys/wait.h>

#include "IPopupMenuControl.h"
#include "IPlugTimer.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <GL/glx.h>
#include <GL/glxext.h>

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

namespace {

// MARK: - XEmbed protocol constants (Xlib does not ship these)

constexpr long kXEmbedVersion = 0;
constexpr long kXEmbedEmbeddedNotify = 0;
constexpr long kXEmbedWindowActivate = 2;
constexpr long kXEmbedRequestFocus = 3;
constexpr long kXEmbedFocusIn = 4;
constexpr long kXEmbedFocusOut = 5;
constexpr long kXEmbedInfoSupportsXEmbed = 1L << 0;
constexpr long kXEmbedInfoHandlesFocus = 1L << 1;

// Used when sending the embed notification to the host's container window, matching
// what Qt/GTK do (XSendEvent masks are a filter, not a subscription).
constexpr long kSelectionRedirectMask = 1L << 21;

constexpr int kDoubleClickIntervalMS = 400;
constexpr int kDoubleClickDistancePx = 6;

// iPlug 2 carries a Windows-style virtual key code in IKeyPress::VK on every platform
// (see IGraphicsWin / IGraphicsWeb), so keysyms are translated to that table here.
int KeysymToVK(KeySym keysym)
{
  switch (keysym)
  {
    case XK_Shift_L: case XK_Shift_R: return 16;
    case XK_Control_L: case XK_Control_R: return 17;
    case XK_Alt_L: case XK_Alt_R: case XK_Menu: return 18;
    case XK_BackSpace: return 8;
    case XK_Tab: return 9;
    case XK_Clear: return 12;
    case XK_Return: case XK_KP_Enter: return 13;
    case XK_Pause: return 19;
    case XK_Caps_Lock: return 20;
    case XK_Escape: return 27;
    case XK_space: return 32;
    case XK_Prior: return 33;
    case XK_Next: return 34;
    case XK_End: return 35;
    case XK_Home: return 36;
    case XK_Left: return 37;
    case XK_Up: return 38;
    case XK_Right: return 39;
    case XK_Down: return 40;
    case XK_Select: return 41;
    case XK_Print: return 42;
    case XK_Execute: return 43;
    case XK_Insert: return 45;
    case XK_Delete: return 46;
    case XK_Help: return 47;
    case XK_Num_Lock: return 144;
    case XK_Scroll_Lock: return 145;
    case XK_KP_Multiply: return 106;
    case XK_KP_Add: return 107;
    case XK_KP_Subtract: return 109;
    case XK_KP_Decimal: return 110;
    case XK_KP_Divide: return 111;
    default:
      break;
  }

  if (keysym >= XK_F1 && keysym <= XK_F12)
    return 112 + static_cast<int>(keysym - XK_F1);

  if (keysym >= XK_KP_0 && keysym <= XK_KP_9)
    return 96 + static_cast<int>(keysym - XK_KP_0);

  if (keysym >= XK_a && keysym <= XK_z)
    return toupper(static_cast<int>(keysym));

  if ((keysym >= XK_A && keysym <= XK_Z) || (keysym >= XK_0 && keysym <= XK_9))
    return static_cast<int>(keysym);

  return 0;
}

// Locate a truetype/openetype file for a system family name without pulling in
// fontconfig. Plug-ins normally ship their fonts, which go through the file path
// overload of LoadPlatformFont instead, so this is a convenience for system faces.
bool FindSystemFontFile(const char* fontName, ETextStyle style, WDL_String& result)
{
  static const char* kRoots[] = { "/usr/share/fonts", "/usr/local/share/fonts", "~/.fonts", "~/.local/share/fonts" };

  WDL_String familyLower(fontName);
  for (int i = 0; i < familyLower.GetLength(); i++)
    familyLower.Get()[i] = static_cast<char>(tolower(static_cast<int>(familyLower.Get()[i])));

  const char* styleHint = style == ETextStyle::Bold ? "bold" : style == ETextStyle::Italic ? "italic" : "";

  for (const char* rootCStr : kRoots)
  {
    WDL_String root(rootCStr);
    if (root.Get()[0] == '~')
    {
      const char* home = getenv("HOME");
      if (!home || !*home)
        continue;
      root.SetFormatted(strlen(home) + root.GetLength(), "%s%s", home, root.Get() + 1);
    }

    WDL_String cmd;
    cmd.SetFormatted(1024, "find \"%s\" \\( -iname \"*%s*%s*.ttf\" -o -iname \"*%s*%s*.otf\" \\) 2>/dev/null | head -n 1",
                     root.Get(), familyLower.Get(), styleHint, familyLower.Get(), styleHint);

    FILE* pipe = popen(cmd.Get(), "r");
    if (!pipe)
      continue;

    char line[1024] = { 0 };
    const bool found = fgets(line, sizeof(line), pipe) && line[0];
    pclose(pipe);

    if (!found)
      continue;

    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';

    if (len)
    {
      result.Set(line);
      return true;
    }
  }

  return false;
}

bool SpawnProcess(const char* program, const char* arg)
{
  const pid_t pid = fork();

  if (pid < 0)
    return false;

  if (pid == 0)
  {
    execlp(program, program, arg, static_cast<char*>(nullptr));
    _exit(127);
  }

  int status = 0;
  waitpid(pid, &status, 0);

  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

// MARK: - Life cycle

IGraphicsLinux::IGraphicsLinux(IGEditorDelegate& dlg, int w, int h, int fps, float scale)
: IGRAPHICS_DRAW_CLASS(dlg, w, h, fps, scale)
{
  DBGMSG("IGraphics Linux (X11 + GLX) @ %i FPS\n", fps);
  mFPS = fps > 0 ? fps : 60;
}

IGraphicsLinux::~IGraphicsLinux()
{
  CloseWindow();

  if (mColormap && mDisplay)
  {
    XFreeColormap(static_cast<Display*>(mDisplay), static_cast<Colormap>(mColormap));
    mColormap = 0;
  }

  if (mDisplay)
  {
    XCloseDisplay(static_cast<Display*>(mDisplay));
    mDisplay = nullptr;
  }
}

// MARK: - Sizing

int IGraphicsLinux::PixelWidth() const
{
  return static_cast<int>(WindowWidth() * GetPlatformWindowScale());
}

int IGraphicsLinux::PixelHeight() const
{
  return static_cast<int>(WindowHeight() * GetPlatformWindowScale());
}

// MARK: - Visual / GLX context

bool IGraphicsLinux::ChooseVisual()
{
  Display* dpy = static_cast<Display*>(mDisplay);

  static int attribs[] = {
    GLX_X_RENDERABLE, True,
    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
    GLX_RENDER_TYPE, GLX_RGBA_BIT,
    GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    GLX_ALPHA_SIZE, 8,
    GLX_DOUBLEBUFFER, True,
    GLX_STENCIL_SIZE, 8,
    // Sans echantillonnage multiple : relire le tampon arriere d'une surface multisamplee
    // renvoie des donnees incoherentes, et NanoVG dessine deja avec un lissage vectoriel.
    GLX_SAMPLE_BUFFERS, 0,
    GLX_SAMPLES, 0,
    None
  };

  int nConfigs = 0;
  GLXFBConfig* configs = glXChooseFBConfig(dpy, mScreen, attribs, &nConfigs);

  // Software renderers and some host containers have no stencil buffer; the NanoVG GL2
  // back end only uses it for NVG_STENCIL_STROKES.
  if (!configs || nConfigs == 0)
  {
    if (configs)
      XFree(configs);

    for (int i = 0; attribs[i] != None; i++)
      if (attribs[i] == GLX_STENCIL_SIZE)
        attribs[i + 1] = 0;

    configs = glXChooseFBConfig(dpy, mScreen, attribs, &nConfigs);
  }

  if (!configs || nConfigs == 0)
  {
    if (configs)
      XFree(configs);

    DBGMSG("IGraphicsLinux: no GLX framebuffer config is available\n");
    return false;
  }

  // glXChooseFBConfig ne promet pas l'ordre : prendre expressement une config sans
  // echantillonnage multiple plutot que la premiere de la liste.
  int chosen = 0;

  for (int i = 0; i < nConfigs; ++i)
  {
    int smp = 0, buf = 0;

    glXGetFBConfigAttrib(dpy, configs[i], GLX_SAMPLES, &smp);
    glXGetFBConfigAttrib(dpy, configs[i], GLX_SAMPLE_BUFFERS, &buf);

    if (smp <= 1 && buf == 0)
    {
      chosen = i;
      break;
    }
  }

  mGLXFBConfig = static_cast<void*>(configs[chosen]);

  XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, static_cast<GLXFBConfig>(mGLXFBConfig));
  XFree(configs);

  if (!vi)
  {
    DBGMSG("IGraphicsLinux: the selected GLX config has no visual\n");
    return false;
  }

  mDepth = vi->depth;
  mColormap = static_cast<unsigned long>(XCreateColormap(dpy, mParentWindow ? mParentWindow : mRootWindow, vi->visual, AllocNone));
  XFree(vi);

  return mColormap != 0;
}

void IGraphicsLinux::CreateGLContext()
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow || !mGLXFBConfig || mGLXContext)
    return;

  // Ask for an explicit GL 2.1 compatibility context when GLX_ARB_create_context is
  // available: that is the profile VoLum requests on Windows.
  PFNGLXCREATECONTEXTATTRIBSARBPROC createAttribs = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
      glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));

  GLXContext ctx = nullptr;

  if (createAttribs)
  {
    int attribs[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 2, GLX_CONTEXT_MINOR_VERSION_ARB, 1,
                      GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, None };
    ctx = createAttribs(dpy, static_cast<GLXFBConfig>(mGLXFBConfig), nullptr, True, attribs);

    if (!ctx)
    {
      // Some servers only hand out a context when no version is requested at all.
      ctx = createAttribs(dpy, static_cast<GLXFBConfig>(mGLXFBConfig), nullptr, True, nullptr);
    }
  }

  if (!ctx)
    ctx = glXCreateNewContext(dpy, static_cast<GLXFBConfig>(mGLXFBConfig), GLX_RGBA_TYPE, None, True);

  if (!ctx)
  {
    DBGMSG("IGraphicsLinux: could not create a GLX context\n");
    return;
  }

  if (!glXMakeCurrent(dpy, static_cast<GLXDrawable>(mXWindow), ctx))
  {
    DBGMSG("IGraphicsLinux: glXMakeCurrent failed\n");
    glXDestroyContext(dpy, ctx);
    return;
  }

  mGLXContext = static_cast<void*>(ctx);
  mGLCurrent = true;
}

void IGraphicsLinux::DestroyGLContext()
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy)
    return;

  if (mGLXContext)
  {
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, static_cast<GLXContext>(mGLXContext));
    mGLXContext = nullptr;
  }

  mGLCurrent = false;
}

void IGraphicsLinux::ActivateGLContext()
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (dpy && mGLXContext && mXWindow && !mGLCurrent)
  {
    glXMakeCurrent(dpy, static_cast<GLXDrawable>(mXWindow), static_cast<GLXContext>(mGLXContext));
    mGLCurrent = true;
  }
}

void IGraphicsLinux::DeactivateGLContext()
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (dpy && mGLXContext && mGLCurrent)
  {
    glXMakeCurrent(dpy, None, nullptr);
    mGLCurrent = false;
  }
}

// MARK: - Window

// Xlib's default error handler prints a message and calls exit(1): one mismatched request
// inside the plug-in would take the whole host down. Log it and carry on instead.
static int IgnoreXError(Display* dpy, XErrorEvent* ev)
{
  char text[128] = { 0 };
  XGetErrorText(dpy, ev->error_code, text, sizeof(text) - 1);
  // stderr and not DBGMSG: this fires on a release build too, and a plug-in that misuses X11
  // should say so instead of staying quiet.
  fprintf(stderr, "IGraphicsLinux: erreur X11 %d (%s), requete opcode %d mineure %d, ressource invalide 0x%lx\n",
          ev->error_code, text, static_cast<int>(ev->request_code), static_cast<int>(ev->minor_code),
          static_cast<unsigned long>(ev->resourceid));

  return 0;
}

void* IGraphicsLinux::OpenWindow(void* pParent)
{
  if (mXWindow)
    return GetWindow();

  if (!mDisplay)
  {
    XInitThreads();
    mDisplay = XOpenDisplay(nullptr);
    if (!mDisplay)
    {
      DBGMSG("IGraphicsLinux: XOpenDisplay failed (is DISPLAY set? Wayland hosts provide XWayland)\n");
      return nullptr;
    }

    mPrevXErrorHandler = reinterpret_cast<void*>(XSetErrorHandler(IgnoreXError));
  }

  Display* dpy = static_cast<Display*>(mDisplay);
  mScreen = DefaultScreen(dpy);
  mRootWindow = static_cast<unsigned long>(RootWindow(dpy, mScreen));
  mParentWindow = pParent ? static_cast<unsigned long>(reinterpret_cast<uintptr_t>(pParent)) : mRootWindow;
  mEmbedded = (pParent != nullptr) && (mParentWindow != mRootWindow);

  if (!ChooseVisual())
    return nullptr;

  XSetWindowAttributes attrs;
  memset(&attrs, 0, sizeof(attrs));
  attrs.colormap = static_cast<Colormap>(mColormap);
  attrs.border_pixel = BlackPixel(dpy, mScreen);
  attrs.background_pixel = BlackPixel(dpy, mScreen);
  attrs.override_redirect = mEmbedded ? False : False;
  attrs.event_mask = ExposureMask | StructureNotifyMask | VisibilityChangeMask | ButtonPressMask | ButtonReleaseMask
                   | PointerMotionMask | LeaveWindowMask | KeyPressMask | KeyReleaseMask | FocusChangeMask
                   | PropertyChangeMask;

  const int w = PixelWidth();
  const int h = PixelHeight();

  XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, static_cast<GLXFBConfig>(mGLXFBConfig));
  Visual* visual = vi ? static_cast<Visual*>(vi->visual) : static_cast<Visual*>(DefaultVisual(dpy, mScreen));
  const int depth = vi ? vi->depth : DefaultDepth(dpy, mScreen);
  if (vi)
    XFree(vi);

  mXWindow = static_cast<unsigned long>(
      XCreateWindow(dpy, static_cast<Window>(mParentWindow), mXOffset, mYOffset, w > 0 ? w : 1, h > 0 ? h : 1, 0, depth,
                    InputOutput, visual, CWColormap | CWBorderPixel | CWBackPixel | CWEventMask | CWOverrideRedirect,
                    &attrs));

  if (!mXWindow)
  {
    DBGMSG("IGraphicsLinux: XCreateWindow failed\n");
    return nullptr;
  }

  mLastWidth = w;
  mLastHeight = h;

  static Atom xaXEmbedInfo = XInternAtom(dpy, "_XEMBED_INFO", False);
  static Atom xaXEmbed = XInternAtom(dpy, "_XEMBED", False);

  const long embedInfo[2] = { kXEmbedVersion, kXEmbedInfoSupportsXEmbed | kXEmbedInfoHandlesFocus };
  XChangeProperty(dpy, static_cast<Window>(mXWindow), xaXEmbedInfo, xaXEmbedInfo, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(embedInfo), 2);

  if (mEmbedded)
  {
    // The window was created as a child of the container, but XEmbed hosts wait for a
    // ReparentNotify before they acknowledge the embedding, so ask for it explicitly.
    XReparentWindow(dpy, static_cast<Window>(mXWindow), static_cast<Window>(mParentWindow), mXOffset, mYOffset);

    // Tell the container we speak XEmbed before mapping, so it can hand us focus.
    XEvent msg;
    memset(&msg, 0, sizeof(msg));
    msg.xclient.type = ClientMessage;
    msg.xclient.display = dpy;
    msg.xclient.window = static_cast<Window>(mParentWindow);
    msg.xclient.message_type = xaXEmbed;
    msg.xclient.format = 32;
    msg.xclient.data.l[0] = CurrentTime;
    msg.xclient.data.l[1] = kXEmbedEmbeddedNotify;
    msg.xclient.data.l[2] = static_cast<long>(mXWindow);   // window being embedded
    msg.xclient.data.l[3] = static_cast<long>(mParentWindow); // ... into this window
    msg.xclient.data.l[4] = kXEmbedVersion;

    XSendEvent(dpy, static_cast<Window>(mParentWindow), False, StructureNotifyMask | SubstructureNotifyMask, &msg);
  }
  else
  {
    static Atom xaWmDeleteWindow = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, static_cast<Window>(mXWindow), &xaWmDeleteWindow, 1);

    const char* title = "iPlug2 plug-in";
    XStoreName(dpy, static_cast<Window>(mXWindow), title);
    XSetIconName(dpy, static_cast<Window>(mXWindow), title);

    XSizeHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = PSize | PMinSize;
    hints.min_width = hints.min_height = 1;
    XSetWMNormalHints(dpy, static_cast<Window>(mXWindow), &hints);
  }

  // Input method, so non-ASCII typing works and the IM preedit stays out of our way.
  mInputMethod = static_cast<void*>(XOpenIM(dpy, nullptr, nullptr, nullptr));
  if (mInputMethod)
  {
    mInputContext = static_cast<void*>(
        XCreateIC(static_cast<XIM>(mInputMethod), XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                  static_cast<Window>(mXWindow), nullptr));
  }

  XMapWindow(dpy, static_cast<Window>(mXWindow));
  XFlush(dpy);

  // The UI thread takes over the GL context and initializes the view, so a single thread
  // owns both the window and the context for the whole life of the editor.
  mGLInitPending = true;
  mUIThreadRun = true;
  mUIThread = std::thread([this] { UIRoop(); });

  return GetWindow();
}

void IGraphicsLinux::DestroyWindow()
{
  Display* dpy = static_cast<Display*>(mDisplay);

  if (dpy && mXWindow)
  {
    const Window wnd = static_cast<Window>(mXWindow);

    if (mInputContext)
    {
      XDestroyIC(static_cast<XIC>(mInputContext));
      mInputContext = nullptr;
    }

    if (mInputMethod)
    {
      XCloseIM(static_cast<XIM>(mInputMethod));
      mInputMethod = nullptr;
    }

    XUnmapWindow(dpy, wnd);
    XDestroyWindow(dpy, wnd);
    XFlush(dpy);
  }

  mXWindow = 0;
}

void IGraphicsLinux::CloseWindow()
{
  if (mDisplay && mPrevXErrorHandler)
  {
    XSetErrorHandler(reinterpret_cast<XErrorHandler>(mPrevXErrorHandler));
    mPrevXErrorHandler = nullptr;
  }

  if (!mXWindow)
    return;

  StopUIThread();
  OnViewDestroyed();
  DestroyGLContext();
  DestroyWindow();
}

// MARK: - Resize

void IGraphicsLinux::PlatformResize(bool parentHasResized)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow)
    return;

  mLastWidth = PixelWidth();
  mLastHeight = PixelHeight();

  XResizeWindow(dpy, static_cast<Window>(mXWindow), static_cast<unsigned>(mLastWidth), static_cast<unsigned>(mLastHeight));
  XFlush(dpy);
}

void IGraphicsLinux::DrawResize()
{
  // Base class re-creates NanoVG's main framebuffer at the new size.
  IGRAPHICS_DRAW_CLASS::DrawResize();

  if (mGLCurrent)
    glViewport(0, 0, static_cast<GLsizei>(PixelWidth()), static_cast<GLsizei>(PixelHeight()));
}

// MARK: - UI thread and event pump

void IGraphicsLinux::PostToUIThread(std::function<void()> task)
{
  if (!task || !mUIThreadRun)
    return;

  std::lock_guard<std::mutex> lock(mUITaskMutex);
  mUITasks.push_back(std::move(task));
}

void IGraphicsLinux::DrainUITasks()
{
  for (;;)
  {
    std::function<void()> task;

    {
      std::lock_guard<std::mutex> lock(mUITaskMutex);

      if (mUITasks.empty())
        return;

      task = std::move(mUITasks.front());
      mUITasks.pop_front();
    }

    task();
  }
}

void IGraphicsLinux::RegisterTimerDispatcher(bool registerDispatcher)
{
  if (mTimerDispatcherRegistered.exchange(registerDispatcher) == registerDispatcher)
    return;

  mTimerDispatcherRegistered = registerDispatcher;

  // One global slot: two plug-in windows at once would fight over it, which the other
  // platform layers avoid by delivering each tick on their own message thread.
  if (registerDispatcher)
  {
    mTimerTickHandoff = false;

    Timer_impl::SetThreadDispatcher([this](std::function<void()> task) {
      // Seeing one posted tick proves the timer thread has no tick running right now: it
      // handles one tick at a time, so everything started before this point is finished.
      mTimerTickHandoff = true;
      PostToUIThread(std::move(task));
    });
  }
  else
  {
    Timer_impl::SetThreadDispatcher(nullptr);

    std::lock_guard<std::mutex> lock(mUITaskMutex);
    mUITasks.clear();
  }
}

void IGraphicsLinux::StopUIThread()
{
  mUIThreadRun = false;

  if (mUIThread.joinable() && mUIThread.get_id() != std::this_thread::get_id())
    mUIThread.join();
  else if (mUIThread.get_id() == std::this_thread::get_id())
    mUIThread.detach();

  RegisterTimerDispatcher(false);
}

void IGraphicsLinux::UIRoop()
{
  const auto framePeriod = std::chrono::milliseconds(1000 / (mFPS > 0 ? mFPS : 60));

  Display* dpy = static_cast<Display*>(mDisplay);
  mUIThreadId = std::this_thread::get_id();

  // From here on the iPlug2 idle timer's ticks run on this thread, as they do on the
  // message thread under Windows (SetTimer posts WM_TIMER to the thread that created the
  // editor) and the main run loop under macOS. Drawing and OnIdle() must never overlap:
  // OnIdle() creates, deletes and rewrites controls, and drawing walks that same list.
  RegisterTimerDispatcher(true);

  // A tick that was already executing on the timer thread when the dispatcher was installed
  // cannot be recalled, so hold off touching the UI until a tick arrives through the queue.
  const auto handoffDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);

  while (!mTimerTickHandoff && std::chrono::steady_clock::now() < handoffDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

  if (!mTimerTickHandoff)
    DBGMSG("IGraphicsLinux: no idle tick has been handed over yet, the editor may race with the timer\n");

  if (dpy)
    XLockDisplay(dpy);

  if (mGLInitPending)
  {
    CreateGLContext();

    if (mGLXContext)
    {
      OnViewInitialized(nullptr);

      // Same order as IGraphicsWin::CreateWindow(): the delegate lays the control tree out
      // here (IGEditorDelegate::LayoutUI -> mLayoutFunc), so it must happen before the first
      // draw, and SetScreenScale() is what sizes the draw context.
      SetScreenScale(GetScreenScale());
      GetDelegate()->LayoutUI(this);

      SetAllControlsDirty();
    }

    mGLInitPending = false;
    DeactivateGLContext();
  }

  if (dpy)
    XUnlockDisplay(dpy);

  while (mUIThreadRun)
  {
    PumpEvents();
    OnDisplayTimer();
    std::this_thread::sleep_for(framePeriod);
  }
}

void IGraphicsLinux::PumpEvents()
{
  DrainUITasks();

  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy)
    return;

  XEvent ev;

  // Never block: the host drives focus and timing, so the pump only drains what is queued.
  while (mUIThreadRun && XPending(dpy) > 0)
  {
    XNextEvent(dpy, &ev);

    if (!HandleEvent(&ev))
      break;
  }
}

void IGraphicsLinux::OnDisplayTimer()
{
  Display* dpy = static_cast<Display*>(mDisplay);

  if (!dpy || !mXWindow || !mGLXContext)
    return;

  IRECTList rects;
  if (!IsDirty(rects))
    return;

  ActivateGLContext();
  Draw(rects);
  CaptureFrameIfNeeded();
  glXSwapBuffers(dpy, static_cast<GLXDrawable>(mXWindow));
  DeactivateGLContext();
}

// Optional frame dump for the Linux test harness: VOLUM_LINUX_CAPTURE=/tmp/frame writes
// /tmp/frame.1.ppm, .2.ppm and .3.ppm. A screenshot of a GL window is black on a compositing
// server (XWayland redirects the window and never sees the DRI buffer), so the back buffer is
// read here, before the swap, instead.
void IGraphicsLinux::CaptureFrameIfNeeded()
{
  static const char* prefix = getenv("VOLUM_LINUX_CAPTURE");
  static const int kFramesToGrab[3] = { 20, 60, 120 };
  static int nextGrab = 0;
  static int drawnFrames = 0;

  if (!prefix || !prefix[0] || nextGrab >= 3)
    return;

  if (++drawnFrames < kFramesToGrab[nextGrab])
    return;

  // Le dessin se fait dans la surface GLX : ce sont ses dimensions reelles qu'il faut relire,
  // pas la taille demandee de la vue.
  unsigned int glxW = 0, glxH = 0;
  glXQueryDrawable(static_cast<Display*>(mDisplay), static_cast<GLXDrawable>(mXWindow), GLX_WIDTH, &glxW);
  glXQueryDrawable(static_cast<Display*>(mDisplay), static_cast<GLXDrawable>(mXWindow), GLX_HEIGHT, &glxH);

  const int w = glxW > 0 ? glxW : PixelWidth();
  const int h = glxH > 0 ? glxH : PixelHeight();

  fprintf(stderr, "[capture] vue %d x %d, surface GLX %d x %d, buffer lu %d x %d\n", PixelWidth(), PixelHeight(), glxW,
          glxH, w, h);

  if (w <= 0 || h <= 0)
    return;

  std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

  char path[512];
  snprintf(path, sizeof(path), "%s.%d.ppm", prefix, nextGrab + 1);

  if (FILE* f = fopen(path, "wb"))
  {
    fprintf(f, "P6\n%d %d\n255\n", w, h);

    // OpenGL rend 4 octets par pixel, un PPM « P6 » en attend 3 : il faut retirer l'alpha,
    // sinon chaque ligne deplacee de trois octets donne une image entirement decalee.
    std::vector<unsigned char> row(static_cast<size_t>(w) * 3);

    for (int y = h - 1; y >= 0; --y) // OpenGL's origin is bottom left, a PPM's is top left
    {
      const unsigned char* src = &pixels[static_cast<size_t>(y) * static_cast<size_t>(w) * 4];

      for (int x = 0; x < w; ++x)
      {
        row[static_cast<size_t>(x) * 3 + 0] = src[static_cast<size_t>(x) * 4 + 0];
        row[static_cast<size_t>(x) * 3 + 1] = src[static_cast<size_t>(x) * 4 + 1];
        row[static_cast<size_t>(x) * 3 + 2] = src[static_cast<size_t>(x) * 4 + 2];
      }

      fwrite(row.data(), 1, row.size(), f);
    }

    fclose(f);
    fprintf(stderr, "[capture] %s ecrite (%d x %d)\n", path, w, h);
  }
  else
    fprintf(stderr, "[capture] impossible d'ecrire %s\n", path);

  ++nextGrab;
}

IMouseInfo IGraphicsLinux::GetMouseInfo(float pixelX, float pixelY, unsigned int keyMask) const
{
  const float scale = GetTotalScale();

  IMouseInfo info;
  info.x = pixelX / scale;
  info.y = pixelY / scale;
  info.ms = IMouseMod((keyMask & Button1Mask) != 0, (keyMask & Button3Mask) != 0, (keyMask & ShiftMask) != 0,
                      (keyMask & ControlMask) != 0, (keyMask & Mod1Mask) != 0);

  return info;
}

bool IGraphicsLinux::HandleEvent(void* pEvent)
{
  XEvent& ev = *static_cast<XEvent*>(pEvent);
  Display* dpy = static_cast<Display*>(mDisplay);

  if (!dpy || !mXWindow)
    return false;

  switch (ev.type)
  {
    case Expose:
    case MapNotify:
      SetAllControlsDirty();
      break;

    case VisibilityNotify:
      if (ev.xvisibility.state != VisibilityUnobscured)
        SetAllControlsDirty();
      break;

    case ConfigureNotify:
    {
      const int w = ev.xconfigure.width;
      const int h = ev.xconfigure.height;

      if (w > 0 && h > 0 && (w != mLastWidth || h != mLastHeight))
      {
        mLastWidth = w;
        mLastHeight = h;

        // The host sized our window: push that through the graphics class without asking
        // the platform to resize the window again (Resize()'s platform step would loop).
        Resize(static_cast<int>(w / GetPlatformWindowScale()), static_cast<int>(h / GetPlatformWindowScale()),
               GetDrawScale(), false);
      }
      break;
    }

    case FocusIn:
      if (mInputContext)
        XSetICFocus(static_cast<XIC>(mInputContext));
      SetAllControlsDirty();
      break;

    case FocusOut:
      if (mInputContext)
        XUnsetICFocus(static_cast<XIC>(mInputContext));
      break;

    case ButtonPress:
    {
      if (ev.xbutton.button >= Button4) // wheel: 4/5 vertical, 6/7 horizontal
      {
#ifdef Button6
        const bool reverse = ev.xbutton.button == Button5 || ev.xbutton.button == Button7;
#else
        const bool reverse = ev.xbutton.button == Button5;
#endif
        const float delta = reverse ? -1.f : 1.f;

        const IMouseInfo info = GetMouseInfo(static_cast<float>(ev.xbutton.x), static_cast<float>(ev.xbutton.y), ev.xbutton.state);
        OnMouseWheel(info.x, info.y, info.ms, delta);
        break;
      }

      const bool doubleClick = ev.xbutton.button == mLastClickButton && (ev.xbutton.time - mLastClickTime) <= kDoubleClickIntervalMS
                            && abs(ev.xbutton.x - mLastClickX) <= kDoubleClickDistancePx
                            && abs(ev.xbutton.y - mLastClickY) <= kDoubleClickDistancePx;

      mLastClickTime = ev.xbutton.time;
      mLastClickButton = ev.xbutton.button;
      mLastClickX = ev.xbutton.x;
      mLastClickY = ev.xbutton.y;

      IMouseInfo info = GetMouseInfo(static_cast<float>(ev.xbutton.x), static_cast<float>(ev.xbutton.y), ev.xbutton.state);
      mMouseX = info.x;
      mMouseY = info.y;

      if (doubleClick)
      {
        OnMouseDblClick(info.x, info.y, info.ms);
      }
      else
      {
        std::vector<IMouseInfo> list{ info };
        OnMouseDown(list);
      }
      break;
    }

    case ButtonRelease:
    {
      if (ev.xbutton.button >= Button4)
        break;

      IMouseInfo info = GetMouseInfo(static_cast<float>(ev.xbutton.x), static_cast<float>(ev.xbutton.y), ev.xbutton.state);
      mMouseX = info.x;
      mMouseY = info.y;

      std::vector<IMouseInfo> list{ info };
      OnMouseUp(list);
      break;
    }

    case MotionNotify:
    {
      IMouseInfo info = GetMouseInfo(static_cast<float>(ev.xmotion.x), static_cast<float>(ev.xmotion.y), ev.xmotion.state);
      info.dX = info.x - mMouseX;
      info.dY = info.y - mMouseY;
      mMouseX = info.x;
      mMouseY = info.y;

      std::vector<IMouseInfo> list{ info };
      OnMouseDrag(list);
      break;
    }

    case LeaveNotify:
      if (ev.xcrossing.mode != NotifyGrab && ev.xcrossing.mode != NotifyUngrab)
        OnMouseOut();
      break;

    case KeyPress:
    case KeyRelease:
    {
      char utf8[8] = { 0 };
      KeySym keysym = NoSymbol;
      int len = 0;

      if (mInputContext)
        len = Xutf8LookupString(static_cast<XIC>(mInputContext), &ev.xkey, utf8, sizeof(utf8) - 1, &keysym, nullptr);
      else
        len = XLookupString(&ev.xkey, utf8, sizeof(utf8) - 1, &keysym, nullptr);

      if (keysym == NoSymbol)
        keysym = XLookupKeysym(&ev.xkey, (ev.xkey.state & ShiftMask) ? 1 : 0);

      if (!len && !keysym)
        break;

      utf8[sizeof(utf8) - 1] = '\0';

      IKeyPress key(utf8, KeysymToVK(keysym), (ev.xkey.state & ShiftMask) != 0, (ev.xkey.state & ControlMask) != 0,
                    (ev.xkey.state & Mod1Mask) != 0);

      if (ev.type == KeyPress)
        OnKeyDown(mMouseX, mMouseY, key);
      else
        OnKeyUp(mMouseX, mMouseY, key);
      break;
    }

    case ClientMessage:
    {
      static Atom xaXEmbed = XInternAtom(dpy, "_XEMBED", False);

      if (ev.xclient.message_type == xaXEmbed)
      {
        switch (ev.xclient.data.l[0])
        {
          case kXEmbedRequestFocus:
            XSetInputFocus(dpy, static_cast<Window>(mXWindow), RevertToParent, CurrentTime);
            break;
          case kXEmbedFocusIn:
            if (mInputContext)
              XSetICFocus(static_cast<XIC>(mInputContext));
            SetAllControlsDirty();
            break;
          case kXEmbedFocusOut:
            if (mInputContext)
              XUnsetICFocus(static_cast<XIC>(mInputContext));
            break;
          case kXEmbedWindowActivate:
            SetAllControlsDirty();
            break;
          default:
            break;
        }
        return true;
      }
      break;
    }

    case SelectionRequest:
      ServeClipboardRequest(&ev.xselectionrequest);
      break;

    case SelectionNotify:
      if (mClipWindow && ev.xselection.requestor == static_cast<Window>(mClipWindow))
        ReadClipboardReply(&ev);
      break;

    case DestroyNotify:
      if (ev.xdestroywindow.window == static_cast<Window>(mXWindow))
      {
        // The host tore the editor down under us: drop the handle, the GL context goes
        // with the window, and stop pumping.
        mXWindow = 0;
        mGLXContext = nullptr;
        mGLCurrent = false;
        mUIThreadRun = false;
        return false;
      }
      break;

    default:
      break;
  }

  return true;
}

// MARK: - Mouse cursor

void IGraphicsLinux::GetMouseLocation(float& x, float& y) const
{
  Display* dpy = static_cast<Display*>(mDisplay);

  if (dpy && mXWindow)
  {
    Window rootReturn = 0, childReturn = 0;
    int rootX = 0, rootY = 0, winX = 0, winY = 0;
    unsigned int mask = 0;

    if (XQueryPointer(dpy, static_cast<Window>(mXWindow), &rootReturn, &childReturn, &rootX, &rootY, &winX, &winY, &mask))
    {
      mMouseX = static_cast<float>(winX) / GetTotalScale();
      mMouseY = static_cast<float>(winY) / GetTotalScale();
    }
  }

  x = mMouseX;
  y = mMouseY;
}

unsigned long IGraphicsLinux::CursorFor(ECursor cursorType)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy)
    return 0;

  const int key = static_cast<int>(cursorType);
  auto it = mCursors.find(key);
  if (it != mCursors.end())
    return it->second;

  unsigned int shape = XC_left_ptr;

  switch (cursorType)
  {
    case ECursor::ARROW: shape = XC_left_ptr; break;
    case ECursor::IBEAM: shape = XC_xterm; break;
    case ECursor::WAIT: case ECursor::APPSTARTING: shape = XC_watch; break;
    case ECursor::UPARROW: shape = XC_center_ptr; break;
    case ECursor::HELP: shape = XC_question_arrow; break;
    case ECursor::CROSS: shape = XC_crosshair; break;
    case ECursor::SIZENS: shape = XC_sb_v_double_arrow; break;
    case ECursor::SIZEWE: shape = XC_sb_h_double_arrow; break;
    case ECursor::SIZENWSE: shape = XC_top_left_corner; break;
    case ECursor::SIZENESW: shape = XC_top_right_corner; break;
    case ECursor::SIZEALL: shape = XC_fleur; break;
    case ECursor::INO: shape = XC_cross; break;
    case ECursor::HAND: shape = XC_hand2; break;
    default: shape = XC_left_ptr; break;
  }

  const unsigned long cursor = static_cast<unsigned long>(XCreateFontCursor(dpy, shape));
  mCursors[key] = cursor;

  return cursor;
}

ECursor IGraphicsLinux::SetMouseCursor(ECursor cursorType)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  const ECursor previous = mCursorType;
  mCursorType = cursorType;

  if (dpy && mXWindow && !mCursorHidden)
  {
    XDefineCursor(dpy, static_cast<Window>(mXWindow), static_cast<Cursor>(CursorFor(cursorType)));
    XFlush(dpy);
  }

  return previous;
}

void IGraphicsLinux::HideMouseCursor(bool hide, bool lock)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow || mCursorHidden == hide)
    return;

  const Window wnd = static_cast<Window>(mXWindow);

  if (hide)
  {
    if (!mBlankCursor)
    {
      static char blankBits[1] = { 0 };
      XColor blank{ 0, 0, 0, 0, 0, 0 };
      const Pixmap pixmap = XCreateBitmapFromData(dpy, wnd, blankBits, 1, 1);
      mBlankCursor = static_cast<unsigned long>(XCreatePixmapCursor(dpy, pixmap, pixmap, &blank, &blank, 0, 0));
      XFreePixmap(dpy, pixmap);
    }

    XDefineCursor(dpy, wnd, static_cast<Cursor>(mBlankCursor));
    if (lock)
      XGrabPointer(dpy, wnd, True, 0, GrabModeAsync, GrabModeAsync, wnd, None, CurrentTime);
  }
  else
  {
    XDefineCursor(dpy, wnd, static_cast<Cursor>(CursorFor(mCursorType)));
    if (lock)
      XUngrabPointer(dpy, CurrentTime);
  }

  mCursorHidden = hide;
  XFlush(dpy);
}

void IGraphicsLinux::MoveMouseCursor(float x, float y)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow)
    return;

  XWarpPointer(dpy, None, static_cast<Window>(mXWindow), 0, 0, 0, 0, static_cast<int>(x * GetTotalScale()),
               static_cast<int>(y * GetTotalScale()));
  XFlush(dpy);
}

// MARK: - Editing

void IGraphicsLinux::ForceEndUserEdit()
{
  ReleaseMouseCapture();
}

// MARK: - Dialogs and URL launching

EMsgBoxResult IGraphicsLinux::ShowMessageBox(const char* str, const char* caption, EMsgBoxType type,
                                             IMsgBoxCompletionHandlerFunc completionHandler)
{
  // TODO(linux): show a native (Qt/KDE) or in-app message box. Reporting a result keeps
  // the plug-in's state machine moving instead of blocking on the missing dialog.
  DBGMSG("IGraphicsLinux::ShowMessageBox [%s] %s\n", caption ? caption : "", str ? str : "");

  const EMsgBoxResult result = (kMB_YESNO == type || kMB_YESNOCANCEL == type) ? kYES : kOK;

  if (completionHandler)
    completionHandler(result);

  return result;
}

void IGraphicsLinux::PromptForFile(WDL_String& fileName, WDL_String& path, EFileAction action, const char* ext,
                                   IFileDialogCompletionHandlerFunc completionHandler)
{
  // TODO(linux): org.freedesktop.portal.FileChooser over D-Bus (works on Wayland and
  // X11, no toolkit dependency in-process). Cancelling reports empty strings.
  DBGMSG("IGraphicsLinux::PromptForFile is not implemented on Linux yet (%s, ext '%s')\n",
          action == EFileAction::Save ? "save" : "open", ext ? ext : "");

  fileName.Set("");
  path.Set("");

  if (completionHandler)
    completionHandler(fileName, path);
}

void IGraphicsLinux::PromptForFiles(WDL_String& path, std::vector<WDL_String>& fileNames, const char* ext)
{
  DBGMSG("IGraphicsLinux::PromptForFiles is not implemented on Linux yet (ext '%s')\n", ext ? ext : "");

  path.Set("");
  fileNames.clear();
}

void IGraphicsLinux::PromptForDirectory(WDL_String& dir, IFileDialogCompletionHandlerFunc completionHandler)
{
  DBGMSG("IGraphicsLinux::PromptForDirectory is not implemented on Linux yet\n");

  dir.Set("");

  if (completionHandler)
    completionHandler(dir, dir);
}

bool IGraphicsLinux::PromptForColor(IColor& color, const char* str, IColorPickerHandlerFunc func)
{
  DBGMSG("IGraphicsLinux::PromptForColor is not implemented on Linux yet (%s)\n", str ? str : "");
  return false;
}

bool IGraphicsLinux::OpenURL(const char* url, const char* msgWindowTitle, const char* confirmMsg, const char* errMsgOnFailure)
{
  if (!url || !*url)
    return false;

  if (SpawnProcess("xdg-open", url))
    return true;

  DBGMSG("IGraphicsLinux::OpenURL failed for '%s'\n", url);
  return false;
}

bool IGraphicsLinux::RevealPathInExplorerOrFinder(WDL_String& path, bool select)
{
  WDL_String folder(path);
  const char* filePart = folder.get_filepart();

  if (filePart && *filePart)
    folder.SetLen(static_cast<int>(filePart - folder.Get()));

  return SpawnProcess("xdg-open", folder.Get());
}

// MARK: - Clipboard

void IGraphicsLinux::ServeClipboardRequest(void* pRequestEvent)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy)
    return;

  XSelectionRequestEvent& req = *static_cast<XSelectionRequestEvent*>(pRequestEvent);

  XEvent reply;
  memset(&reply, 0, sizeof(reply));
  reply.xselection.type = SelectionNotify;
  reply.xselection.display = dpy;
  reply.xselection.requestor = req.requestor;
  reply.xselection.selection = req.selection;
  reply.xselection.target = req.target;
  reply.xselection.property = None;
  reply.xselection.time = req.time;

  static Atom xaTargets = XInternAtom(dpy, "TARGETS", False);
  static Atom xaUtf8String = XInternAtom(dpy, "UTF8_STRING", False);
  static Atom xaString = XA_STRING;

  if (req.property != None && !mClipText.empty())
  {
    if (req.target == xaTargets)
    {
      const Atom targets[3] = { xaTargets, xaUtf8String, xaString };
      XChangeProperty(dpy, req.requestor, req.property, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<const unsigned char*>(targets), 3);
    }
    else if (req.target == xaUtf8String || req.target == xaString || req.target == AnyPropertyType)
    {
      const Atom type = req.target == AnyPropertyType ? xaString : req.target;
      XChangeProperty(dpy, req.requestor, req.property, type, 8, PropModeReplace,
                      reinterpret_cast<const unsigned char*>(mClipText.data()), static_cast<int>(mClipText.size()));
    }
    else
    {
      XSendEvent(dpy, req.requestor, False, 0, &reply); // property None = refused
      XFlush(dpy);
      return;
    }

    reply.xselection.property = req.property;
  }

  XSendEvent(dpy, req.requestor, False, 0, &reply);
  XFlush(dpy);
}

bool IGraphicsLinux::SetTextInClipboard(const char* str)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow || !str)
    return false;

  mClipText.assign(str);

  static Atom xaClipboard = XInternAtom(dpy, "CLIPBOARD", False);
  XSetSelectionOwner(dpy, xaClipboard, static_cast<Window>(mXWindow), CurrentTime);
  XFlush(dpy);

  return XGetSelectionOwner(dpy, xaClipboard) == static_cast<Window>(mXWindow);
}

void IGraphicsLinux::ReadClipboardReply(void* pEvent)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  XEvent& ev = *static_cast<XEvent*>(pEvent);

  mClipReplyDone = true;

  if (ev.xselection.property == None || !mClipWindow)
    return;

  Atom actualType = None;
  int actualFormat = 0;
  unsigned long nItems = 0, bytesAfter = 0;
  unsigned char* data = nullptr;

  const Status ok = XGetWindowProperty(dpy, static_cast<Window>(mClipWindow), ev.xselection.property, 0, 65536, True,
                                       AnyPropertyType, &actualType, &actualFormat, &nItems, &bytesAfter, &data);

  if (ok == Success && data)
  {
    const unsigned long bytes = nItems * (static_cast<unsigned long>(actualFormat) / 8);
    if (bytes)
      mClipReply.assign(reinterpret_cast<const char*>(data), bytes);
    XFree(data);
  }

  XDeleteProperty(dpy, static_cast<Window>(mClipWindow), ev.xselection.property);
}

bool IGraphicsLinux::GetTextFromClipboard(WDL_String& str)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  str.Set("");

  if (!dpy)
    return false;

  static Atom xaClipboard = XInternAtom(dpy, "CLIPBOARD", False);
  static Atom xaUtf8String = XInternAtom(dpy, "UTF8_STRING", False);

  const Window owner = XGetSelectionOwner(dpy, xaClipboard);

  if (owner == None)
    return false;

  if (owner == static_cast<Window>(mXWindow))
  {
    str.Set(mClipText.c_str());
    return !mClipText.empty();
  }

  // Request the conversion on a scratch window: the answer arrives as an event, which
  // the UI thread routes to ReadClipboardReply(). Calling this from the UI thread pumps
  // the reply inline; from any other thread the UI thread does it, so nothing deadlocks.
  const Window scratch = XCreateSimpleWindow(dpy, static_cast<Window>(mRootWindow), 0, 0, 1, 1, 0, 0, 0);
  if (scratch == None)
    return false;

  const Atom property = XInternAtom(dpy, "IPLUG2_CLIPBOARD_TMP", False);

  mClipReply.clear();
  mClipReplyDone = false;
  mClipWindow = static_cast<unsigned long>(scratch);

  XConvertSelection(dpy, xaClipboard, xaUtf8String, property, scratch, CurrentTime);
  XFlush(dpy);

  const bool onUIThread = mUIThreadRun && mUIThread.get_id() == std::this_thread::get_id();

  for (int attempt = 0; attempt < 40 && !mClipReplyDone; ++attempt)
  {
    if (onUIThread)
      PumpEvents();
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (mClipReplyDone && mClipReply.empty())
  {
    // The owner may only offer STRING; retry once with the legacy target.
    mClipReplyDone = false;
    XConvertSelection(dpy, xaClipboard, XA_STRING, property, scratch, CurrentTime);
    XFlush(dpy);

    for (int attempt = 0; attempt < 20 && !mClipReplyDone; ++attempt)
    {
      if (onUIThread)
        PumpEvents();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  XDestroyWindow(dpy, scratch);
  XFlush(dpy);
  mClipWindow = 0;

  str.Set(mClipReply.c_str());

  return !mClipReply.empty();
}

bool IGraphicsLinux::SetFilePathInClipboard(const char* path)
{
  // X11 has no separate "file drop" clipboard: text is what paste targets expect.
  return SetTextInClipboard(path);
}

// MARK: - Fonts

namespace {

/** Loads a font from a file on disk, picking the face that matches a style name. */
class LinuxFileFont final : public PlatformFont
{
public:
  LinuxFileFont(const char* path, const char* styleName)
  : PlatformFont(false), mPath(path ? path : ""), mStyleName(styleName && *styleName ? styleName : "")
  {
  }

  IFontDataPtr GetFontData() override
  {
    IFontDataPtr fontData(new IFontData());

    FILE* fp = fopen(mPath.Get(), "rb");
    if (!fp)
    {
      DBGMSG("IGraphicsLinux: cannot open font file '%s'\n", mPath.Get());
      return fontData;
    }

    fseek(fp, 0, SEEK_END);
    const int size = static_cast<int>(ftell(fp));
    fseek(fp, 0, SEEK_SET);

    fontData = std::make_unique<IFontData>(size);

    if (!fontData->GetSize())
    {
      fclose(fp);
      return fontData;
    }

    const size_t readSize = fread(fontData->Get(), 1, static_cast<size_t>(fontData->GetSize()), fp);
    fclose(fp);

    if (!readSize || readSize != static_cast<size_t>(fontData->GetSize()))
      return fontData;

    int faceIdx = GetFaceIdx(fontData->Get(), fontData->GetSize(), mStyleName.Get());
    if (faceIdx < 0 && mStyleName.GetLength())
      faceIdx = GetFaceIdx(fontData->Get(), fontData->GetSize(), ""); // fall back to the first face

    if (faceIdx >= 0)
      fontData->SetFaceIdx(faceIdx);

    return fontData;
  }

private:
  WDL_String mPath;
  WDL_String mStyleName;
};

/** Keeps a copy of font data handed to the graphics class. */
class LinuxMemoryFont final : public PlatformFont
{
public:
  LinuxMemoryFont(const void* pData, int dataSize)
  : PlatformFont(false)
  {
    if (pData && dataSize > 0)
      mData.Set(static_cast<const uint8_t*>(pData), dataSize);
  }

  IFontDataPtr GetFontData() override
  {
    if (!mData.GetSize())
      return IFontDataPtr(new IFontData());

    return IFontDataPtr(new IFontData(mData.Get(), mData.GetSize(), 0));
  }

private:
  WDL_TypedBuf<uint8_t> mData;
};

} // namespace

PlatformFontPtr IGraphicsLinux::LoadPlatformFont(const char* fontID, const char* fileNameOrResID)
{
  WDL_String fullPath;
  const EResourceLocation location = LocateResource(fileNameOrResID, "ttf", fullPath, GetBundleID(), nullptr, nullptr);

  if (location == EResourceLocation::kNotFound)
  {
    DBGMSG("IGraphicsLinux: font resource '%s' was not found\n", fileNameOrResID ? fileNameOrResID : "");
    return nullptr;
  }

  return PlatformFontPtr(new LinuxFileFont(fullPath.Get(), nullptr));
}

PlatformFontPtr IGraphicsLinux::LoadPlatformFont(const char* fontID, const char* fontName, ETextStyle style)
{
  WDL_String path;
  if (!fontName || !*fontName || !FindSystemFontFile(fontName, style, path))
  {
    DBGMSG("IGraphicsLinux: system font '%s' could not be located\n", fontName ? fontName : "");
    return nullptr;
  }

  const char* styleName = style == ETextStyle::Bold ? "Bold" : style == ETextStyle::Italic ? "Italic" : "";

  return PlatformFontPtr(new LinuxFileFont(path.Get(), styleName));
}

PlatformFontPtr IGraphicsLinux::LoadPlatformFont(const char* fontID, void* pData, int dataSize)
{
  return PlatformFontPtr(new LinuxMemoryFont(pData, dataSize));
}

void IGraphicsLinux::CachePlatformFont(const char* fontID, const PlatformFontPtr& font)
{
  // NanoVG keeps its own glyph atlas and iPlug 2 already caches the IFontData, so there
  // is nothing platform specific to hold on to.
}

// MARK: - Platform widgets

IPopupMenu* IGraphicsLinux::CreatePlatformPopupMenu(IPopupMenu& menu, const IRECT bounds, bool& isAsync)
{
  // There is no native X11 menu worth embedding in a plug-in window, so menus are drawn by
  // iPlug 2's in-app popup control with NanoVG (same look as the macOS/Windows menus).
  if (!GetPopupMenuControl())
    AttachPopupMenuControl();

  IPopupMenuControl* pPopupControl = GetPopupMenuControl();
  if (!pPopupControl)
    return nullptr;

  isAsync = true; // the value is applied when the user picks an item

  pPopupControl->CreatePopupMenu(menu, bounds);

  return nullptr;
}

void IGraphicsLinux::CreatePlatformTextEntry(int paramIdx, const IText& text, const IRECT& bounds, int length, const char* str)
{
  // VoLum attaches iPlug 2's in-app ITextEntryControl, so this is not reached in practice.
  DBGMSG("IGraphicsLinux::CreatePlatformTextEntry is not implemented on Linux yet\n");
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
