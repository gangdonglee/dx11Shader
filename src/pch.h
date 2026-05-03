// Precompiled header — kept narrow so a header touch doesn't blow the
// PCH and force a full rebuild. Includes the heavy Windows / DX11 / STL
// headers that nearly every translation unit pulls in.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Core/MathTypes.h"
