#pragma once

// The few names that differ between the SDK the mod is built against normally (ZHMModSDK 4.2.1,
// game 3.290.1) and the one the separate legacy build uses (4.0.0-rc.1, game 3.240.x), so the
// code using them reads the same in both builds.

#include <Glacier/ZScene.h>
#include <Glacier/ZLevelManager.h>

#if COOP_LEGACY_SDK
// That SDK has no game-state hook or enum: the legacy build works the state out itself (loading
// from a scene load, playing once 47 is in it, clearing from a scene clear), with these values.
enum class CoopGameState {
    EGS_Disabled = 0,
    EGS_PreloadAssets = 1,
    EGS_WaitingForLoadVideo = 2,
    EGS_Precaching = 3,
    EGS_Preparing = 4,
    EGS_WaitingForPrecache = 5,
    EGS_LoadSaveGame = 6,
    EGS_Activating = 7,
    EGS_ActivatedStart = 8,
    EGS_Activated = 9,
    EGS_Playing = 10,
    EGS_Deactivating = 11
};

// Same layout as 4.2.1's SSceneInitParameters, older member names.
using CoopSceneParams = ZSceneData;
inline const ZString& SceneResourceOf(const CoopSceneParams& p) { return p.m_sceneName; }
inline const ZString& SceneCodeNameOf(const CoopSceneParams& p) { return p.m_codeNameHint; }
inline const TArray<ZString>& SceneBricksOf(const CoopSceneParams& p) { return p.m_sceneBricks; }
#else
using CoopGameState = ZLevelManager::EGameState;
using CoopSceneParams = SSceneInitParameters;
inline const ZString& SceneResourceOf(const CoopSceneParams& p) { return p.m_SceneResource; }
inline const ZString& SceneCodeNameOf(const CoopSceneParams& p) { return p.m_CodeNameHint; }
inline const TArray<ZString>& SceneBricksOf(const CoopSceneParams& p) { return p.m_aAdditionalBrickResources; }
#endif
