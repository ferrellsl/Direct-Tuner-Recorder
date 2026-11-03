/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <string>

namespace dt
{

// Http or https GET, for SiliconDust's guide service. The add-on goes through Kodi for this;
// the recorder has no Kodi, so WinHTTP on Windows and curl elsewhere.
bool FetchUrl(const std::string& url, std::string& body);

} // namespace dt
