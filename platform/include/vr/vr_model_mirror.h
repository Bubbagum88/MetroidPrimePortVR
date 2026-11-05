// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "vr/vr_view.h"
#include "dolphin/gx/GXAurora.h"

namespace PortVr {
class ModelMirrorScope {
    u8 previous;
public:
    explicit ModelMirrorScope(bool mirrored = VrMirrorWeaponModels())
        : previous(AuroraSetCullMirrored(mirrored ? 1 : 0)) {}
    ~ModelMirrorScope() { AuroraSetCullMirrored(previous); }
    ModelMirrorScope(const ModelMirrorScope&) = delete;
    ModelMirrorScope& operator=(const ModelMirrorScope&) = delete;
};

// The arm and cannon are modelled for the fixed first-person view and have no back faces, so a
// tracked hand turned away showed through them. In VR they draw both sides of every face.
class ModelTwoSidedScope {
    u8 previous;
public:
    explicit ModelTwoSidedScope(bool twoSided = VrImmersive())
        : previous(AuroraSetCullTwoSided(twoSided ? 1 : 0)) {}
    ~ModelTwoSidedScope() { AuroraSetCullTwoSided(previous); }
    ModelTwoSidedScope(const ModelTwoSidedScope&) = delete;
    ModelTwoSidedScope& operator=(const ModelTwoSidedScope&) = delete;
};
}
