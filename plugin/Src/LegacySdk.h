#pragma once

// Included first in every file of the separate legacy build (COOP_LEGACY_SDK: ZHMModSDK
// 4.0.0-rc.1, for players still on game 3.240.x), and only there.
//
// That SDK's ZString.h includes <minmax.h>, which defines min()/max() macros whatever NOMINMAX
// says, and they break every std::min/std::max. Its include guard is set here so it adds nothing,
// and ZString.h's own unqualified min() call gets std::min instead.
#define _INC_MINMAX
#include <algorithm>
using std::max;
using std::min;
