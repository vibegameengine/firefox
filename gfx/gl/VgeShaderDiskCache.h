/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_GL_VGE_SHADER_DISK_CACHE_H
#define GFX_GL_VGE_SHADER_DISK_CACHE_H

#include "GLTypes.h"

namespace mozilla::gl {

class GLLibraryEGL;

void InstallShaderDiskCache(const GLLibraryEGL& aLib, EGLDisplay aDisplay,
                            const char* aDisplayExtensions);

}  // namespace mozilla::gl

#endif
