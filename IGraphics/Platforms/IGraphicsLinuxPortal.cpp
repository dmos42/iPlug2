/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

/**
 * @file
 * @brief Implementation of the XDG desktop portal dialogs (see the header).
 *
 * Deliberately free of IGraphics and WDL types: what comes back from the desktop
 * is a path or three, and the platform layer turns that into the strings iPlug
 * expects. That also keeps this file testable on its own, against a fake portal.
 */

#include "IGraphicsLinuxPortal.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <gio/gio.h>

namespace iplug
{
namespace
{

constexpr char kBusName[] = "org.freedesktop.portal.Desktop";
constexpr char kObjectPath[] = "/org/freedesktop/portal/desktop";
constexpr char kRequestInterface[] = "org.freedesktop.portal.Request";
constexpr char kRequestResponse[] = "Response";

// A person browsing folders takes time, so the wait is open-ended in practice: the
// deadline only exists so that a desktop which vanished cannot hang the host.
constexpr gint64 kAnswerTimeoutUSec = 15 * G_TIME_SPAN_MINUTE;

// Every request asks for a token of its own. The desktop uses it in the object path
// of the Request, which is how a person can have two dialogs open at once without
// their answers getting mixed up between them.
std::atomic<uint32_t> gTokenCounter{0};

struct PortalReply
{
  GMutex mutex;
  gboolean answered = FALSE;
  gboolean busGone = FALSE;
  guint32 code = 2;
  GVariant* results = nullptr; // the vardict of the Response, owned by us
};

/** The answer is written here from the portal's signal handler, which runs on the
 *  thread that is pumping the context (see PortalCall::run) - the same one waiting.
 *  The mutex stays because a GDBus worker thread is what reads the message off the
 *  bus and hands it over, and it is not this thread's business to be halfway through
 *  a field when the waiter looks. */
void onResponseSignal(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* params, gpointer userData)
{
  PortalReply* reply = static_cast<PortalReply*>(userData);
  if (!params || g_variant_n_children(params) < 2)
    return;

  guint32 code = 2;
  GVariant* codeValue = g_variant_get_child_value(params, 0);
  g_variant_get(codeValue, "u", &code);
  g_variant_unref(codeValue);

  GVariant* results = g_variant_get_child_value(params, 1);

  g_mutex_lock(&reply->mutex);
  if (!reply->answered)
  {
    reply->code = code;
    reply->results = g_variant_ref_sink(results);
    reply->answered = TRUE;
  }
  g_mutex_unlock(&reply->mutex);
  g_variant_unref(results);
}

// The session bus going away (logging out, the portal daemon killed) would otherwise
// leave the wait until its deadline.
void onBusClosed(GDBusConnection*, gboolean, GError*, gpointer userData)
{
  PortalReply* reply = static_cast<PortalReply*>(userData);
  g_mutex_lock(&reply->mutex);
  reply->busGone = TRUE;
  g_mutex_unlock(&reply->mutex);
}

int hexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/** The portal speaks in file:// URIs; the plug-in speaks in paths. */
std::string uriToPath(const char* uri)
{
  if (!uri || std::strncmp(uri, "file://", 7) != 0)
    return std::string();

  const char* p = uri + 7;
  if (std::strncmp(p, "localhost/", 10) == 0)
    p += 9; // file://localhost/tmp/x is the same file

  std::string path;
  for (; *p; ++p)
  {
    if (*p == '%' && p[1] && p[2])
    {
      const int hi = hexDigit(p[1]);
      const int lo = hexDigit(p[2]);
      if (hi >= 0 && lo >= 0)
      {
        path += static_cast<char>((hi << 4) | lo);
        p += 2;
        continue;
      }
    }
    path += *p;
  }

  return path;
}

/** One round trip: connect, ask, listen, wait, disconnect. */
class PortalCall
{
public:
  PortalCall(const char* interfaceName, const char* method, const char* title, const char* parentWindow)
    : mInterface(interfaceName)
    , mMethod(method)
    , mTitle(title && *title ? title : "VoLum")
    , mParentWindow(parentWindow && *parentWindow ? parentWindow : "")
  {
    g_mutex_init(&mReply.mutex);
    // The options are filled in by the caller between the constructor and run(), so
    // the builder has to be open from here on: an add_value on a builder that was
    // never initialised is a GLib critical, not an exception.
    g_variant_builder_init(&mOptions, G_VARIANT_TYPE("a{sv}"));
  }

  ~PortalCall()
  {
    if (mSubscription)
      g_dbus_connection_signal_unsubscribe(mConnection, mSubscription);
    if (mClosedHandler)
      g_signal_handler_disconnect(mConnection, mClosedHandler);
    if (mReply.results)
      g_variant_unref(mReply.results);
    if (mConnection)
    {
      g_dbus_connection_close_sync(mConnection, nullptr, nullptr);
      g_object_unref(mConnection);
    }
    g_mutex_clear(&mReply.mutex);
    if (mContextPushed)
      g_main_context_pop_thread_default(mContext);
    if (mContext)
      g_main_context_unref(mContext);
    if (!mBuilderConsumed)
      g_variant_builder_clear(&mOptions);
  }

  PortalCall(const PortalCall&) = delete;
  PortalCall& operator=(const PortalCall&) = delete;

  /// The vardict sent as the request's options; add the request-specific keys here.
  GVariantBuilder* options() { return &mOptions; }

  bool run(std::string& errorOut)
  {
    if (!connect(errorOut))
      return false;

    // GDBus runs a signal handler on the thread-default main context of whoever
    // subscribed. So subscribe inside a context of our own, and pump that one below:
    // the default context is left alone, because this thread is already inside the
    // host's event loop and turning its gears is not ours to do.
    mContext = g_main_context_new();
    g_main_context_push_thread_default(mContext);
    mContextPushed = true;

    // Listen before asking. This connection is ours alone and carries exactly one
    // request at a time, so any Response from the portal is the one we are waiting
    // for - the portal sends it to the caller, not to the bus at large. Matching on
    // the object path instead would mean predicting it, and the portal's spelling of
    // a bus name inside a path (":1.7" becomes "1_7") is not something worth
    // depending on.
    mSubscription = g_dbus_connection_signal_subscribe(mConnection, kBusName, kRequestInterface, kRequestResponse, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, onResponseSignal, &mReply, nullptr);
    mClosedHandler = g_signal_connect(mConnection, "closed", G_CALLBACK(onBusClosed), &mReply);

    GVariant* params = nullptr;
    if (std::strcmp(mInterface, "org.freedesktop.portal.Screenshot") == 0)
      params = g_variant_new("(sa{sv})", mParentWindow.c_str(), &mOptions);
    else
      params = g_variant_new("(ssa{sv})", mParentWindow.c_str(), mTitle.c_str(), &mOptions);
    mBuilderConsumed = true; // g_variant_new() closed the builder, call or no call

    GError* error = nullptr;
    GVariant* callReply = g_dbus_connection_call_sync(mConnection, kBusName, kObjectPath, mInterface, mMethod, params, G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!callReply)
    {
      // g_variant_new() took the options builder whether or not the call went out.
      errorOut = error && error->message ? error->message : "the desktop refused the request";
      g_clear_error(&error);
      return false;
    }

    // The handle (an object path) is the portal's side of the handshake: it is the
    // object the Response signal is emitted on. We do not need it here because the
    // subscription above is by interface, but taking it says "yes, this request
    // exists" to the desktop, which is what the portal is waiting for.
    g_variant_unref(callReply);

    return wait(errorOut);
  }

  guint32 code() const { return mReply.code; }

  GVariant* results() const { return mReply.results; }

private:
  bool connect(std::string& errorOut)
  {
    GError* error = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!address)
    {
      errorOut = error && error->message ? error->message : "no session bus";
      g_clear_error(&error);
      return false;
    }

    // A connection of our own rather than the shared one: a shared connection belongs
    // to the whole process and its signals arrive in the main loop of whoever asked
    // for it first, which on a plug-in's UI thread is the host's loop, not ours. Ours
    // is a private connection whose signals we pump ourselves, for exactly as long as
    // the dialog is up, and closing it afterwards leaves nothing behind.
    mConnection = g_dbus_connection_new_for_address_sync(
      address, static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, &error);
    g_free(address);
    if (!mConnection)
    {
      errorOut = error && error->message ? error->message : "cannot reach the session bus";
      g_clear_error(&error);
      return false;
    }

    // The bus will not route a reply to a connection that has no name. GLib registers
    // one on a message-bus connection by itself, so ask for Hello only if that has not
    // happened - calling it twice is an error on the bus.
    if (!g_dbus_connection_get_unique_name(mConnection))
    {
      GVariant* hello = g_dbus_connection_call_sync(
        mConnection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", nullptr, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
      if (hello)
        g_variant_unref(hello);
      else
      {
        errorOut = error && error->message ? error->message : "the session bus would not name us";
        g_clear_error(&error);
        return false;
      }
    }
    g_clear_error(&error);

    mToken = "volum" + std::to_string(++gTokenCounter);
    g_variant_builder_add(&mOptions, "{sv}", "handle_token", g_variant_new_string(mToken.c_str()));

    return true;
  }

  bool wait(std::string& errorOut)
  {
    const gint64 deadline = g_get_monotonic_time() + kAnswerTimeoutUSec;

    while (true)
    {
      g_mutex_lock(&mReply.mutex);
      const gboolean answered = mReply.answered;
      const gboolean busGone = mReply.busGone;
      g_mutex_unlock(&mReply.mutex);

      if (answered)
        return true;
      if (busGone)
      {
        errorOut = "the session bus went away while the dialog was open";
        return false;
      }

      if (g_get_monotonic_time() > deadline)
      {
        errorOut = "the dialog was left open too long, so it was given up on";
        return false;
      }

      // One turn of the context: this is where the desktop's answer is actually run.
      // Nothing else is registered on it, so an idle turn means nobody has answered
      // yet and a short sleep costs nothing a person would notice.
      if (!g_main_context_iteration(mContext, FALSE))
        g_usleep(20 * G_TIME_SPAN_MILLISECOND);
    }
  }

  const char* mInterface;
  const char* mMethod;
  std::string mTitle;
  std::string mParentWindow;
  GDBusConnection* mConnection = nullptr;
  GVariantBuilder mOptions;
  PortalReply mReply;
  std::string mToken;
  GMainContext* mContext = nullptr;
  bool mContextPushed = false;
  bool mBuilderConsumed = false;
  gulong mSubscription = 0;
  gulong mClosedHandler = 0;
};

// A dictionary value in the reply, when the desktop chose to send that key at all.
GVariant* lookup(GVariant* results, const char* key) { return results ? g_variant_lookup_value(results, key, nullptr) : nullptr; }

void addFilters(GVariantBuilder* options, const std::vector<PortalFilter>& filters)
{
  if (filters.empty())
    return;

  GVariantBuilder all;
  g_variant_builder_init(&all, G_VARIANT_TYPE("a(sa(us))"));
  for (const PortalFilter& filter : filters)
  {
    if (filter.mPatterns.empty())
      continue;

    GVariantBuilder patterns;
    g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)"));
    for (const std::string& pattern : filter.mPatterns)
      g_variant_builder_add(&patterns, "(us)", 0, pattern.c_str());

    g_variant_builder_add(&all, "(sa(us))", filter.mName.empty() ? "Files" : filter.mName.c_str(), &patterns);
  }
  g_variant_builder_add(options, "{sv}", "filters", g_variant_builder_end(&all));
}

} // namespace

std::vector<PortalFilter> PortalFiltersFromExtensions(const char* ext)
{
  std::vector<PortalFilter> filters;
  if (!ext || !*ext)
    return filters;

  PortalFilter group;
  group.mName = "VoLum files";

  std::istringstream words(ext);
  std::string word;
  while (words >> word)
  {
    if (word.compare(0, 2, "*.") == 0)
      word.erase(0, 2);
    else if (!word.empty() && word.front() == '.')
      word.erase(0, 1);

    if (word.empty() || word == "*")
      return std::vector<PortalFilter>(); // nothing narrows: let the desktop offer all of it

    group.mPatterns.push_back("*." + word);
  }

  if (!group.mPatterns.empty())
    filters.push_back(group);

  return filters;
}

EPortalResult PortalChooseFiles(const PortalFileRequest& request, std::vector<std::string>& pathsOut, std::string& errorOut)
{
  const char* method = request.mMethod;

  pathsOut.clear();
  errorOut.clear();
  if (!method || !*method)
    return EPortalResult::Failed;

  PortalCall call("org.freedesktop.portal.FileChooser", method, request.mTitle.c_str(), request.mParentWindow.c_str());
  GVariantBuilder* options = call.options();

  if (request.mMultiple)
    g_variant_builder_add(options, "{sv}", "multiple", g_variant_new_boolean(TRUE));
  if (request.mDirectory)
    g_variant_builder_add(options, "{sv}", "directory", g_variant_new_boolean(TRUE));
  addFilters(options, request.mFilters);
  if (!request.mInitialPath.empty())
    g_variant_builder_add(options, "{sv}", "current_folder", g_variant_new_bytestring(request.mInitialPath.c_str()));
  if (!request.mInitialName.empty())
    g_variant_builder_add(options, "{sv}", "current_name", g_variant_new_string(request.mInitialName.c_str()));

  if (!call.run(errorOut))
    return EPortalResult::Failed;

  // The portal's own codes: 0 done, 1 cancelled, anything else an error.
  if (call.code() == 1u)
    return EPortalResult::Cancelled;
  if (call.code() != 0u)
  {
    errorOut = "the desktop reported problem code " + std::to_string(call.code());
    return EPortalResult::Failed;
  }

  GVariant* uris = lookup(call.results(), "uris");
  if (uris)
  {
    GVariantIter iter;
    gchar* uri = nullptr;
    for (g_variant_iter_init(&iter, uris); g_variant_iter_next(&iter, "s", &uri);)
    {
      const std::string path = uriToPath(uri);
      g_free(uri);
      if (!path.empty())
        pathsOut.push_back(path);
    }
    g_variant_unref(uris);
  }

  if (pathsOut.empty())
  {
    errorOut = std::string(method) + " came back with no path";
    return EPortalResult::Failed;
  }

  return EPortalResult::Chosen;
}

EPortalResult PortalPickColor(double rgbOut[3], const char* parentWindow, const char* title, std::string& errorOut)
{
  errorOut.clear();
  if (!rgbOut)
    return EPortalResult::Failed;

  PortalCall call("org.freedesktop.portal.Screenshot", "PickColor", title, parentWindow);

  if (!call.run(errorOut))
    return EPortalResult::Failed;

  if (call.code() == 1u)
    return EPortalResult::Cancelled;
  if (call.code() != 0u)
  {
    errorOut = "the desktop reported problem code " + std::to_string(call.code());
    return EPortalResult::Failed;
  }

  // The current spec answers with "color", three sRGB doubles in [0,1]. Older
  // desktops answer with "rgb", three integers of 0-255 - and the spec once spelled
  // that type "(ii)", so the children are read one at a time instead of through a
  // signature the desktop may or may not match.
  GVariant* color = lookup(call.results(), "color");
  if (color)
  {
    if (!g_variant_is_of_type(color, G_VARIANT_TYPE("(ddd)")))
    {
      g_variant_unref(color);
      errorOut = "\"color\" in the reply is not three numbers";
      return EPortalResult::Failed;
    }
    g_variant_get(color, "(ddd)", &rgbOut[0], &rgbOut[1], &rgbOut[2]);
    g_variant_unref(color);
    return EPortalResult::Chosen;
  }

  GVariant* rgb = lookup(call.results(), "rgb");
  if (rgb)
  {
    GVariantIter iter;
    g_variant_iter_init(&iter, rgb);
    GVariant* child = nullptr;
    int index = 0;

    while (index < 3 && (child = g_variant_iter_next_value(&iter)) != nullptr)
    {
      double value = -1.0;
      if (g_variant_is_of_type(child, G_VARIANT_TYPE_INT32))
        value = g_variant_get_int32(child) / 255.0;
      else if (g_variant_is_of_type(child, G_VARIANT_TYPE_UINT32))
        value = static_cast<double>(g_variant_get_uint32(child)) / 255.0;
      else if (g_variant_is_of_type(child, G_VARIANT_TYPE_DOUBLE))
        value = g_variant_get_double(child);

      g_variant_unref(child);
      if (value < 0.0)
        break;

      rgbOut[index++] = value > 1.0 ? 1.0 : value;
    }

    g_variant_unref(rgb);
    if (index == 3)
      return EPortalResult::Chosen;
  }

  errorOut = "the colour picker reply carried no colour";
  return EPortalResult::Failed;
}

} // namespace iplug
