// macOS: the services the shared code reaches through an __APPLE__ branch,
// where the Windows side is a COM or WinHTTP call too large to stand in for
// one name at a time (mac_win32.h covers the plain Win32 calls).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// GET https://host/path into `body` (at most 256 KB); false on any failure,
// within a few seconds when there is no connection (ui/frontend.cpp https_get)
bool mac_https_get(const wchar_t* host, const wchar_t* path, std::string& body);

// Any picture format macOS reads (ImageIO), as BGRA with straight alpha,
// rows top to bottom (ui/frontend.cpp Frontend::load_image)
bool mac_load_image_bgra(const std::wstring& path, std::vector<uint32_t>& px, int& w, int& h);
