#pragma once

// Forwarding header, on the Android include path ahead of ports/ios/d3d9.
//
// ports/ios/d3d9/d3d9_apple.cpp is the Direct3D 9 object model: resource
// lifetimes, LockRect/UnlockRect, format conversion, surface aliasing,
// query objects. None of that is Apple-specific despite the file name, and
// it is the part of the renderer most likely to grow bugs, so the Android
// build compiles the same translation unit rather than forking it.
//
// That file contains exactly two lines that bind it to a backend:
//
//     #include "metal/metal_backend.h"
//     namespace gpu = kisak::metal;
//
// This header satisfies both. Adding `kisak::vk` as the real namespace and
// aliasing `kisak::metal` onto it keeps the Vulkan backend's own code honest
// about what it is, while letting the shared D3D9 layer resolve `gpu::`
// against it unchanged.
//
// If the two interfaces ever drift, this breaks at compile time in the D3D9
// layer, which is the right place to find out.

#include "../../gpu_backend.h"

namespace kisak {
namespace metal = vk;
}
