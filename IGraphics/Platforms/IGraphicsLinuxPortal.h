/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

/**
 * @file
 * @brief Native desktop dialogs for the Linux IGraphics backend, through the XDG
 *        desktop portal (org.freedesktop.portal.* on the session bus).
 *
 * A plug-in has no business embedding a toolkit, and on Wayland it is not even
 * allowed to open its own file chooser: the dialog belongs to the desktop. The
 * portal is the one interface that works on X11 and Wayland alike, with the
 * desktop's own look and its own process.
 *
 * The protocol is the same for every request here: a method call returns the
 * object path of a Request, and the answer arrives later as a Response signal on
 * that object. Waiting for that signal is what makes these dialogs as modal as
 * the Win32 and Cocoa ones.
 *
 * The build decides whether any of this exists: it has to compile this file, link
 * GLib's gio, and define VOLUM_HAVE_PORTAL. Without that define the platform layer
 * keeps its stubs, so a machine with no glib headers still builds a plug-in - one
 * that simply has no dialogs, which is what every Linux build used to be.
 */

#ifndef _IPLUG_IGRAPHICSLINUXPORTAL_
#define _IPLUG_IGRAPHICSLINUXPORTAL_

#include <string>
#include <vector>

namespace iplug
{

/** What the desktop did with a request. */
enum class EPortalResult
{
  Chosen,    ///< the user picked something and the results are filled in
  Cancelled, ///< the user closed the dialog without picking
  Failed,    ///< no portal, no session bus, or the desktop refused the request
};

/** One entry of the "type" menu of a file chooser. Patterns are glob patterns,
 *  exactly as the portal spells them: "*.nam", not "nam". */
struct PortalFilter
{
  std::string mName;
  std::vector<std::string> mPatterns;
};

/** Turn iPlug's space separated extension list ("nam", ".wav .aif", "*.png") into
 *  portal patterns. An empty result means "offer everything", which is also what a
 *  list containing a bare "*" means: it is not a type menu, it is the desktop not
 *  narrowing anything. */
std::vector<PortalFilter> PortalFiltersFromExtensions(const char* ext);

/** What to ask the desktop's file chooser for. A struct rather than a list of
 *  arguments, because a chooser carries a dozen optional hints and each one would
 *  otherwise be another position in a nine-argument call. */
struct PortalFileRequest
{
  const char* mMethod = "OpenFile";   ///< OpenFile, SaveFile or SaveFiles
  std::string mTitle;                 ///< heading of the dialog
  std::string mParentWindow;          ///< "x11:0x…" of the editor, or "" to let the desktop place the dialog
  std::string mInitialPath;           ///< folder to open on; "" leaves that to the desktop
  std::string mInitialName;           ///< name to suggest in a save dialog; "" for none
  std::vector<PortalFilter> mFilters; ///< type menu; an empty list offers everything
  bool mMultiple = false;             ///< let the user pick several files at once
  bool mDirectory = false;            ///< pick a folder instead of files (portal >= 3; ignored below that)
};

/** Ask the desktop for files, or for a directory. The call blocks until the dialog
 *  is answered, so it belongs on a UI thread and never on the audio thread.
 *
 * @param pathsOut Filesystem paths, decoded from the file:// URIs the portal returns.
 * @param errorOut Set to something readable when the result is Failed. */
EPortalResult PortalChooseFiles(const PortalFileRequest& request, std::vector<std::string>& pathsOut, std::string& errorOut);

/** Ask the desktop for a colour (org.freedesktop.portal.Screenshot.PickColor).
 *  @param rgbOut sRGB components in [0,1]. */
EPortalResult PortalPickColor(double rgbOut[3], const char* parentWindow, const char* title, std::string& errorOut);

} // namespace iplug

#endif
