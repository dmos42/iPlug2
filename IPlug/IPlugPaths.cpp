/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

/**
 * @file
 * @brief IPlugPaths implementation for Windows and Linux
 */

#include "IPlugPlatform.h"
#include "IPlugConstants.h"
#include "IPlugPaths.h"

#if defined OS_WEB
#include <emscripten/val.h>
#elif defined OS_WIN
#include <windows.h>
#include <Shlobj.h>
#include <Shlwapi.h>
#elif defined OS_LINUX
#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

BEGIN_IPLUG_NAMESPACE

#if defined OS_WIN
#pragma mark - OS_WIN

 // Helper for getting a known folder in UTF8
void GetKnownFolder(WDL_String &path, int identifier, int flags = 0)
{
  wchar_t wideBuffer[1024];

  SHGetFolderPathW(NULL, identifier, NULL, flags, wideBuffer);
  UTF16ToUTF8(path, wideBuffer);
}

static void GetModulePath(HMODULE hModule, WDL_String& path)
{
  path.Set("");
  char pathCStr[MAX_WIN32_PATH_LEN];
  pathCStr[0] = '\0';
  if (GetModuleFileName(hModule, pathCStr, MAX_WIN32_PATH_LEN))
  {
    int s = -1;
    for (int i = 0; i < strlen(pathCStr); ++i)
    {
      if (pathCStr[i] == '\\')
      {
        s = i;
      }
    }
    if (s >= 0 && s + 1 < strlen(pathCStr))
    {
      path.Set(pathCStr, s + 1);
    }
  }
}

void HostPath(WDL_String& path, const char* bundleID)
{
  GetModulePath(0, path);
}

void PluginPath(WDL_String& path, HMODULE pExtra)
{
  GetModulePath(pExtra, path);
}

void BundleResourcePath(WDL_String& path, HMODULE pExtra)
{
#ifdef VST3_API
  GetModulePath(pExtra, path);
#ifdef ARCH_64BIT
  path.SetLen(path.GetLength() - strlen("x86_64-win/"));
#else
  path.SetLen(path.GetLength() - strlen("x86-win/"));
#endif
  path.Append("Resources\\");
#endif
}

void DesktopPath(WDL_String& path)
{
  GetKnownFolder(path, CSIDL_DESKTOP);
}

void UserHomePath(WDL_String & path)
{
  GetKnownFolder(path, CSIDL_PROFILE);
}

void AppSupportPath(WDL_String& path, bool isSystem)
{
  GetKnownFolder(path, isSystem ? CSIDL_COMMON_APPDATA : CSIDL_LOCAL_APPDATA);
}

void VST3PresetsPath(WDL_String& path, const char* mfrName, const char* pluginName, bool isSystem)
{
  if (!isSystem)
    GetKnownFolder(path, CSIDL_PERSONAL, SHGFP_TYPE_CURRENT);
  else
    AppSupportPath(path, true);
  
  path.AppendFormatted(MAX_WIN32_PATH_LEN, "\\VST3 Presets\\%s\\%s", mfrName, pluginName);
}

void INIPath(WDL_String& path, const char * pluginName)
{
  GetKnownFolder(path, CSIDL_LOCAL_APPDATA);

  path.AppendFormatted(MAX_WIN32_PATH_LEN, "\\%s", pluginName);
}

void WebViewCachePath(WDL_String& path)
{
  GetKnownFolder(path, CSIDL_APPDATA);
  path.Append("\\iPlug2\\WebViewCache"); // tmp
}

static BOOL EnumResNameProc(HANDLE module, LPCTSTR type, LPTSTR name, LONG_PTR param)
{
  if (IS_INTRESOURCE(name)) return true; // integer resources not wanted
  else {
    WDL_String* search = (WDL_String*)param;
    if (search != 0 && name != 0)
    {
      //strip off extra quotes
      WDL_String strippedName(strlwr(name + 1));
      strippedName.SetLen(strippedName.GetLength() - 1);

      if (strcmp(strlwr(search->Get()), strippedName.Get()) == 0) // if we are looking for a resource with this name
      {
        search->SetFormatted(strippedName.GetLength() + 7, "found: %s", strippedName.Get());
        return false;
      }
    }
  }

  return true; // keep enumerating
}

EResourceLocation LocateResource(const char* name, const char* type, WDL_String& result, const char*, void* pHInstance, const char*)
{
  if (CStringHasContents(name))
  {
    WDL_String search(name);
    WDL_String typeUpper(type);

    HMODULE hInstance = static_cast<HMODULE>(pHInstance);

    EnumResourceNames(hInstance, _strupr(typeUpper.Get()), (ENUMRESNAMEPROC)EnumResNameProc, (LONG_PTR)&search);

    if (strstr(search.Get(), "found: ") != 0)
    {
      result.SetFormatted(MAX_PATH, "\"%s\"", search.Get() + 7, search.GetLength() - 7); // 7 = strlen("found: ")
      return EResourceLocation::kWinBinary;
    }
    else
    {
      if (PathFileExists(name))
      {
        result.Set(name);
        return EResourceLocation::kAbsolutePath;
      }
    }
  }
return EResourceLocation::kNotFound;
}

const void* LoadWinResource(const char* resid, const char* type, int& sizeInBytes, void* pHInstance)
{
  WDL_String typeUpper(type);

  HMODULE hInstance = static_cast<HMODULE>(pHInstance);

  HRSRC hResource = FindResource(hInstance, resid, _strupr(typeUpper.Get()));

  if (!hResource)
    return NULL;

  DWORD size = SizeofResource(hInstance, hResource);

  if (size < 8)
    return NULL;

  HGLOBAL res = LoadResource(hInstance, hResource);

  const void* pResourceData = LockResource(res);

  if (!pResourceData)
  {
    sizeInBytes = 0;
    return NULL;
  }
  else
  {
    sizeInBytes = size;
    return pResourceData;
  }
}

#elif defined OS_WEB
#pragma mark - OS_WEB

void AppSupportPath(WDL_String& path, bool isSystem)
{
  path.Set("Settings");
}

void DesktopPath(WDL_String& path)
{
  path.Set("");
}

void VST3PresetsPath(WDL_String& path, const char* mfrName, const char* pluginName, bool isSystem)
{
  path.Set("Presets");
}

EResourceLocation LocateResource(const char* name, const char* type, WDL_String& result, const char*, void*, const char*)
{
  if (CStringHasContents(name))
  {
    WDL_String plusSlash;
    WDL_String path(name);
    const char* file = path.get_filepart();
      
    bool foundResource = false;
    
    //TODO: FindResource is not sufficient here
    
    if(strcmp(type, "png") == 0) { //TODO: lowercase/uppercase png
      plusSlash.SetFormatted(strlen("/resources/img/") + strlen(file) + 1, "/resources/img/%s", file);
      foundResource = emscripten::val::global("preloadedImages").call<bool>("hasOwnProperty", std::string(plusSlash.Get()));
    }
    else if(strcmp(type, "ttf") == 0) { //TODO: lowercase/uppercase ttf
      plusSlash.SetFormatted(strlen("/resources/fonts/") + strlen(file) + 1, "/resources/fonts/%s", file);
      foundResource = true; // TODO: check ttf
    }
    else if(strcmp(type, "svg") == 0) { //TODO: lowercase/uppercase svg
      plusSlash.SetFormatted(strlen("/resources/img/") + strlen(file) + 1, "/resources/img/%s", file);
      foundResource = true; // TODO: check svg
    }
    
    if(foundResource)
    {
      result.Set(plusSlash.Get());
      return EResourceLocation::kAbsolutePath;
    }
  }
  return EResourceLocation::kNotFound;
}

#elif defined OS_LINUX
#pragma mark - OS_LINUX

static bool PathExists(const char* path)
{
  struct stat st;
  return path && *path && stat(path, &st) == 0;
}

static bool IsADirectory(const char* path)
{
  struct stat st;
  return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/** Append a path part, keeping exactly one separator. */
static void AppendPathPart(WDL_String& path, const char* part)
{
  if (!part || !*part)
    return;

  if (!path.GetLength())
  {
    path.Set(part);
    return;
  }

  const int len = path.GetLength();

  if (path.Get()[len - 1] == '/')
    path.Append(part);
  else if (*part == '/')
    path.Append(part);
  else
  {
    path.Append("/");
    path.Append(part);
  }
}

/** Drop the file name from a full path, keeping the folder and its trailing slash. */
static void StripFileName(WDL_String& path)
{
  const char* file = path.get_filepart();

  if (!file)
    return;

  if (file == path.Get()) // no folder part at all
    path.Set("./");
  else
    path.SetLen(static_cast<int>(file - path.Get()));
}

static void EnvOrHome(WDL_String& path, const char* envVar, const char* homeRelative)
{
  const char* env = getenv(envVar);

  if (env && *env)
  {
    path.Set(env);
    return;
  }

  UserHomePath(path);
  AppendPathPart(path, homeRelative);
}

void HostPath(WDL_String& path, const char*)
{
  char buf[PATH_MAX];
  const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);

  if (len > 0)
  {
    buf[len] = '\0';
    path.Set(buf);
  }
  else
    path.Set("");
}

/** Resolve the file a running piece of code was loaded from: the plug-in module when
 * pExtra is a pointer inside it, otherwise the current process executable. */
static void GetModulePath(void* pExtra, WDL_String& path)
{
  Dl_info info;

  if (pExtra && dladdr(pExtra, &info) && info.dli_fname && *info.dli_fname)
    path.Set(info.dli_fname);
  else if (dladdr((void*) &GetModulePath, &info) && info.dli_fname && *info.dli_fname)
    path.Set(info.dli_fname);
  else
    HostPath(path, nullptr);
}

void PluginPath(WDL_String& path, PluginIDType pExtra)
{
  GetModulePath(pExtra, path);
}

/** Inside a VST3 bundle on Linux the module lives at
 * Bundle.vst3/Contents/<arch>/module.so, and shared resources sit in
 * Bundle.vst3/Contents/Resources, which matches the Windows layout. */
void BundleResourcePath(WDL_String& path, PluginIDType pExtra)
{
  GetModulePath(pExtra, path);

  if (!path.GetLength())
    return;

  const char* contents = strstr(path.Get(), "/Contents/");

  if (contents)
  {
    path.SetLen(static_cast<int>(contents - path.Get()) + strlen("/Contents/"));
    path.Append("Resources/");
    return;
  }

  // Standalone or loose build: resources beside the module.
  StripFileName(path);
  AppendPathPart(path, "Resources/");
}

void DesktopPath(WDL_String& path)
{
  const char* xdg = getenv("XDG_DESKTOP_DIR");

  if (xdg && *xdg)
    path.Set(xdg);
  else
  {
    UserHomePath(path);
    AppendPathPart(path, "Desktop");

    if (!IsADirectory(path.Get()))
      UserHomePath(path);
  }
}

void UserHomePath(WDL_String& path)
{
  const char* home = getenv("HOME");
  path.Set((home && *home) ? home : "/");
}

void AppSupportPath(WDL_String& path, bool isSystem)
{
  if (isSystem)
    path.Set("/usr/local/share");
  else
    EnvOrHome(path, "XDG_DATA_HOME", ".local/share");
}

void VST3PresetsPath(WDL_String& path, const char* mfrName, const char* pluginName, bool isSystem)
{
  if (isSystem)
    path.Set("/usr/local/share/vst3-presets");
  else
    EnvOrHome(path, "XDG_DATA_HOME", ".local/share");

  AppendPathPart(path, "VST3 Presets");
  AppendPathPart(path, mfrName);
  AppendPathPart(path, pluginName);
}

void INIPath(WDL_String& path, const char* pluginName)
{
  EnvOrHome(path, "XDG_CONFIG_HOME", ".config");
  AppendPathPart(path, pluginName);
}

void WebViewCachePath(WDL_String& path)
{
  EnvOrHome(path, "XDG_CACHE_HOME", ".cache");
  AppendPathPart(path, "iPlug2/WebViewCache");
}

/** Look for a file in the usual places: as an absolute path, next to the module, in the
 * bundle's Resources folders, then in the XDG data directories. */
EResourceLocation LocateResource(const char* name, const char* type, WDL_String& result, const char*, void* pHInstance,
                                 const char* sharedResourcesSubPath)
{
  if (!CStringHasContents(name))
    return EResourceLocation::kNotFound;

  if (PathExists(name))
  {
    result.Set(name);
    return EResourceLocation::kAbsolutePath;
  }

  WDL_String modulePath;
  GetModulePath(pHInstance, modulePath);
  StripFileName(modulePath);

  WDL_String bundleResources;
  BundleResourcePath(bundleResources, pHInstance);

  WDL_String candidate;

  auto tryPath = [&](const WDL_String& dir) -> bool {
    candidate.Set(dir.Get());
    AppendPathPart(candidate, name);

    if (PathExists(candidate.Get()))
    {
      result.Set(candidate.Get());
      return true;
    }
    return false;
  };

  WDL_String dir(bundleResources);

  if (sharedResourcesSubPath && *sharedResourcesSubPath)
  {
    WDL_String sub(dir);
    AppendPathPart(sub, sharedResourcesSubPath);
    if (tryPath(sub))
      return EResourceLocation::kAbsolutePath;
  }

  if (tryPath(dir))
    return EResourceLocation::kAbsolutePath;

  // The mac layout puts images and fonts in Resources subfolders.
  {
    WDL_String sub(dir);
    const char* file = name;
    const char* filePart = nullptr;
    WDL_String tmp(name);
    filePart = tmp.get_filepart();

    if (filePart && *filePart)
    {
      auto sameExt = [](const char* a, const char* b) {
        while (*a && *b)
        {
          if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b)))
            return false;
          ++a;
          ++b;
        }
        return *a == *b;
      };

      // The image extension is whatever the caller passed - "png", "jpg", "svg" - and fonts use
      // the same Resources subfolders, so key the subfolder off the extension, not one name.
      if (sameExt(type, "png") || sameExt(type, "jpg") || sameExt(type, "jpeg") || sameExt(type, "svg") ||
          sameExt(type, "ktx"))
        AppendPathPart(sub, "img");
      else if (sameExt(type, "ttf") || sameExt(type, "otf") || sameExt(type, "woff") || sameExt(type, "woff2"))
        AppendPathPart(sub, "fonts");

      candidate.Set(sub.Get());
      AppendPathPart(candidate, filePart);

      if (PathExists(candidate.Get()))
      {
        result.Set(candidate.Get());
        return EResourceLocation::kAbsolutePath;
      }
    }
  }

  if (tryPath(modulePath))
    return EResourceLocation::kAbsolutePath;

  {
    WDL_String sub(modulePath);
    AppendPathPart(sub, "Resources");
    if (tryPath(sub))
      return EResourceLocation::kAbsolutePath;
  }

  {
    WDL_String appSupport;
    AppSupportPath(appSupport, false);
    if (tryPath(appSupport))
      return EResourceLocation::kAbsolutePath;
  }

  {
    WDL_String sys;
    AppSupportPath(sys, true);
    if (tryPath(sys))
      return EResourceLocation::kAbsolutePath;
  }

  return EResourceLocation::kNotFound;
}

const void* LoadWinResource(const char*, const char*, int& sizeInBytes, void*)
{
  sizeInBytes = 0;
  return nullptr;
}

#endif

END_IPLUG_NAMESPACE
