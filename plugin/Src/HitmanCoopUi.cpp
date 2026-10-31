// Everything HITMAN Co-op draws: the overlay over the game (name tag, marks, radar, compass, HUD,
// notifications, countdown, mission summary) and the CO-OP window. Render thread; what it shows of
// the game thread's state is copied under m_StatusMutex.
#include "HitmanCoop.h"
#include "CoopText.h"
#include "CrashGuards.h"
#include "GamePause.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "IconsMaterialDesign.h"

namespace {
    const ImVec4 c_Red(0.86f, 0.08f, 0.14f, 1.f);
    const ImVec4 c_Gold(1.f, 0.78f, 0.25f, 1.f);
    const ImVec4 c_Chat(0.45f, 0.8f, 1.f, 1.f);
    const ImVec4 c_White(1.f, 1.f, 1.f, 1.f);

    // Good and bad news: green and red, or blue and orange for colour-blind players.
    struct Palette {
        ImVec4 good, bad;
        ImU32 danger;
    };

    Palette Colours(bool p_ColourBlind) {
        if (p_ColourBlind)
            return { ImVec4(0.35f, 0.65f, 1.f, 1.f), ImVec4(1.f, 0.55f, 0.1f, 1.f), IM_COL32(255, 140, 20, 255) };

        return { ImVec4(0.35f, 0.85f, 0.45f, 1.f), c_Red, IM_COL32(255, 70, 60, 255) };
    }

    ImU32 WithAlpha(ImU32 p_Colour, int p_Alpha) { return (p_Colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(p_Alpha, 0, 255)) << IM_COL32_A_SHIFT); }

    ImU32 FromRgb(uint32_t p_Rgb, int p_Alpha) { return IM_COL32((p_Rgb >> 16) & 0xFF, (p_Rgb >> 8) & 0xFF, p_Rgb & 0xFF, std::clamp(p_Alpha, 0, 255)); }

    // Where a world point is on screen. Off screen (or behind the camera, found by the point
    // mirrored through the camera, which projects the opposite way) it still gives the direction.
    bool PlaceOnScreen(const coop::Vec3& p_At, const coop::Vec3& p_Camera, ImVec2& p_Out, bool& p_OnScreen) {
        const ImGuiViewport* s_View = ImGui::GetMainViewport();
        const ImVec2 s_Centre(s_View->Pos.x + s_View->Size.x * 0.5f, s_View->Pos.y + s_View->Size.y * 0.5f);
        SVector2 s_Screen;

        if (SDK()->WorldToScreen(SVector3(p_At.x, p_At.y, p_At.z), s_Screen)) {
            p_Out = ImVec2(s_View->Pos.x + s_Screen.x, s_View->Pos.y + s_Screen.y);
            p_OnScreen = p_Out.x > s_View->Pos.x + 24.f && p_Out.x < s_View->Pos.x + s_View->Size.x - 24.f &&
                p_Out.y > s_View->Pos.y + 24.f && p_Out.y < s_View->Pos.y + s_View->Size.y - 24.f;
            return true;
        }

        const coop::Vec3 s_Mirror = p_Camera + (p_Camera - p_At);

        if (!SDK()->WorldToScreen(SVector3(s_Mirror.x, s_Mirror.y, s_Mirror.z), s_Screen))
            return false;

        p_Out = ImVec2(s_Centre.x - (s_View->Pos.x + s_Screen.x - s_Centre.x), s_Centre.y - (s_View->Pos.y + s_Screen.y - s_Centre.y));
        p_OnScreen = false;
        return true;
    }

    // Text centred above a point, with a shadow, at a size (1: the UI's).
    void TextShadowed(ImDrawList* p_List, ImVec2 p_At, ImU32 p_Colour, const std::string& p_Text, float p_Scale = 1.f) {
        ImFont* s_Font = ImGui::GetFont();
        const float s_Size = ImGui::GetFontSize() * p_Scale;
        const ImVec2 s_Extent = s_Font->CalcTextSizeA(s_Size, FLT_MAX, 0.f, p_Text.c_str());
        const ImVec2 s_Pos(p_At.x - s_Extent.x * 0.5f, p_At.y - s_Extent.y);
        p_List->AddText(s_Font, s_Size, ImVec2(s_Pos.x + 1.f, s_Pos.y + 1.f), IM_COL32(0, 0, 0, (p_Colour >> IM_COL32_A_SHIFT) & 0xFF), p_Text.c_str());
        p_List->AddText(s_Font, s_Size, s_Pos, p_Colour, p_Text.c_str());
    }

    // An arrow on the screen's edge, pointing the way to something off screen. Kept off the top 120
    // px, where the game lists the objectives (and the HUD starts).
    void EdgeArrow(ImDrawList* p_List, ImVec2 p_Towards, ImU32 p_Colour, const std::string& p_Text, float p_Scale) {
        const ImGuiViewport* s_View = ImGui::GetMainViewport();
        const ImVec2 s_Centre(s_View->Pos.x + s_View->Size.x * 0.5f, s_View->Pos.y + s_View->Size.y * 0.5f);
        ImVec2 s_Dir(p_Towards.x - s_Centre.x, p_Towards.y - s_Centre.y);
        const float s_Len = std::sqrt(s_Dir.x * s_Dir.x + s_Dir.y * s_Dir.y);
        s_Dir = s_Len < 1.f ? ImVec2(0.f, 1.f) : ImVec2(s_Dir.x / s_Len, s_Dir.y / s_Len);

        const float s_HalfW = s_View->Size.x * 0.5f - 56.f;
        const float s_HalfH = s_Dir.y < 0.f ? s_View->Size.y * 0.5f - 120.f : s_View->Size.y * 0.5f - 56.f;
        const float s_T = std::min(s_HalfW / std::max(std::fabs(s_Dir.x), 1e-3f), s_HalfH / std::max(std::fabs(s_Dir.y), 1e-3f));
        const ImVec2 s_At(s_Centre.x + s_Dir.x * s_T, s_Centre.y + s_Dir.y * s_T);
        const ImVec2 s_Side(-s_Dir.y, s_Dir.x);
        const float s_Size = 10.f * p_Scale;

        p_List->AddTriangleFilled(ImVec2(s_At.x + s_Dir.x * s_Size * 1.6f, s_At.y + s_Dir.y * s_Size * 1.6f),
            ImVec2(s_At.x - s_Dir.x * s_Size * 0.8f + s_Side.x * s_Size, s_At.y - s_Dir.y * s_Size * 0.8f + s_Side.y * s_Size),
            ImVec2(s_At.x - s_Dir.x * s_Size * 0.8f - s_Side.x * s_Size, s_At.y - s_Dir.y * s_Size * 0.8f - s_Side.y * s_Size), p_Colour);
        TextShadowed(p_List, ImVec2(s_At.x - s_Dir.x * 22.f * p_Scale, s_At.y - s_Dir.y * 22.f * p_Scale + 8.f), p_Colour, p_Text, p_Scale);
    }

    // The partner's colour: theirs (or gold) while calm, orange when NPCs are wary of them, red when it's serious.
    ImU32 PartnerColour(const coop::SessionStatus& p_Status, int p_Alpha) {
        const int s_Threat = p_Status.peerStateKnown ? p_Status.peerState.threat : 0;

        if (s_Threat >= coop::Threat_Hunted)
            return IM_COL32(255, 80, 70, p_Alpha);

        if (s_Threat >= coop::Threat_AlertedLow)
            return IM_COL32(255, 150, 50, p_Alpha);

        const uint32_t s_Picked = p_Status.peerStateKnown ? p_Status.peerState.colour : 0;
        return s_Picked ? FromRgb(s_Picked, p_Alpha) : IM_COL32(255, 200, 64, p_Alpha);
    }

    // The game menu named by a player's status (EGameUIMenu + 1), or "".
    const char* MenuName(int p_Menu) {
        return p_Menu > 0 && p_Menu <= static_cast<int>(std::size(c_MenuNames)) ? c_MenuNames[p_Menu - 1] : "";
    }

    // Signal icons, over the partner's head.
    constexpr const char* c_SignalIcons[Signal_Count] = { ICON_MD_THUMB_UP, ICON_MD_PAN_TOOL, ICON_MD_SOS, ICON_MD_VISIBILITY };

    // A big button the width of its column.
    bool WideButton(const char* p_Label, float p_Height = 0.f) {
        return ImGui::Button(p_Label, ImVec2(ImGui::GetContentRegionAvail().x, p_Height));
    }

    void HelpMarker(const char* p_Text) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_MD_HELP_OUTLINE);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", p_Text);
    }

    void SectionTitle(const char* p_Text) {
        ImGui::Spacing();
        ImGui::TextColored(c_Gold, "%s", p_Text);
        ImGui::Separator();
    }
}

std::string HitmanCoop::Hide(const std::string& p_Secret) const {
    return m_Streamer && !p_Secret.empty() ? std::string("(hidden)") : p_Secret;
}

// ---- Over the game ------------------------------------------------------------------------------

void HitmanCoop::DrawOverlay(const SdkWorld::Overlay& p_Overlay, const coop::SessionStatus& p_Status) {
    if (!p_Overlay.inMission || p_Overlay.hidden || !p_Status.synced)
        return;

    ImDrawList* s_List = ImGui::GetBackgroundDrawList();
    const float s_Scale = m_OverlayScale;
    const Palette s_Palette = Colours(m_ColourBlind);
    const auto Dist = [&](float p_Metres) {
        char s_Text[24];
        std::snprintf(s_Text, sizeof(s_Text), m_Feet ? "%.0f ft" : "%.0f m", m_Feet ? p_Metres * 3.28084f : p_Metres);
        return std::string(s_Text);
    };

    // The partner: their name over their head, or an arrow to them when they are off screen. Shown
    // even with name tags off while one of us looks for the other.
    if ((m_ShowNameTag || p_Overlay.pulse) && p_Overlay.partnerShown) {
        ImVec2 s_At;
        bool s_OnScreen = false;

        if (PlaceOnScreen(p_Overlay.partnerHead, p_Overlay.camera, s_At, s_OnScreen)) {
            std::string s_Text = p_Status.peerName + "  " + Dist(p_Overlay.partnerDistance);

            // A floor or more apart: which way.
            if (p_Overlay.partnerHeight > 2.5f)
                s_Text += "  " + Dist(p_Overlay.partnerHeight) + " up";
            else if (p_Overlay.partnerHeight < -2.5f)
                s_Text += "  " + Dist(-p_Overlay.partnerHeight) + " down";

            if (p_Status.peerStateKnown && (p_Status.peerState.flags & coop::PlayerFlag_Trespassing))
                s_Text += "  (trespassing)";

            if (p_Status.peerStateKnown && (p_Status.peerState.flags & coop::PlayerFlag_Away))
                s_Text += "  (away)";

            const float s_Pulse = p_Overlay.pulse ? 0.5f + 0.5f * std::sin(static_cast<float>(Now()) * 8.f) : 0.f;

            if (s_OnScreen) {
                // Faint up close, where the partner is plain to see.
                const int s_Alpha = p_Overlay.partnerDistance < 2.5f && !p_Overlay.pulse ? 110 : 235;
                const ImU32 s_Colour = PartnerColour(p_Status, s_Alpha);
                const float s_Size = (6.f + 6.f * s_Pulse) * s_Scale;
                s_List->AddTriangleFilled(ImVec2(s_At.x - s_Size, s_At.y - 8.f - s_Size), ImVec2(s_At.x + s_Size, s_At.y - 8.f - s_Size),
                    ImVec2(s_At.x, s_At.y), s_Colour);
                TextShadowed(s_List, ImVec2(s_At.x, s_At.y - 10.f - s_Size), s_Colour, s_Text, s_Scale);
                float s_Above = s_At.y - 10.f - s_Size - ImGui::GetFontSize() * s_Scale - 2.f;

                if (!p_Overlay.partnerDisguise.empty() && p_Overlay.partnerDisguise != "their suit" && p_Overlay.partnerDistance > 2.5f) {
                    TextShadowed(s_List, ImVec2(s_At.x, s_Above), IM_COL32(220, 220, 220, s_Alpha * 3 / 4), "as " + p_Overlay.partnerDisguise, s_Scale * 0.9f);
                    s_Above -= ImGui::GetFontSize() * s_Scale;
                }

                // Their signal, a big icon that rises and fades.
                if (p_Overlay.signal >= 0 && p_Overlay.signal < Signal_Count) {
                    const int s_SignalAlpha = static_cast<int>(255.f * std::clamp(4.f - p_Overlay.signalAge, 0.f, 1.f));
                    const ImU32 s_SignalColour = p_Overlay.signal == Signal_Help ? WithAlpha(s_Palette.danger, s_SignalAlpha) :
                        PartnerColour(p_Status, s_SignalAlpha);
                    TextShadowed(s_List, ImVec2(s_At.x, s_Above - 6.f - p_Overlay.signalAge * 6.f), s_SignalColour,
                        std::string(c_SignalIcons[p_Overlay.signal]) + " " + c_SignalWords[p_Overlay.signal], s_Scale * 1.6f);
                }

                if (p_Overlay.pulse)
                    s_List->AddCircle(s_At, (18.f + 30.f * s_Pulse) * s_Scale, PartnerColour(p_Status, static_cast<int>(200 * (1.f - s_Pulse))), 32, 3.f);
            }
            else
                EdgeArrow(s_List, s_At, PartnerColour(p_Status, 235), s_Text, s_Scale);
        }
    }

    if (!m_ShowMarks)
        return;

    const uint32_t s_Ours = m_ColourSetting;

    for (const auto& s_Mark : p_Overlay.marks) {
        ImVec2 s_At;
        bool s_OnScreen = false;

        if (!PlaceOnScreen(s_Mark.pos, p_Overlay.camera, s_At, s_OnScreen))
            continue;

        // Fades over its last 3 s; pulses as it appears (a danger mark keeps pulsing).
        const float s_Left = p_Overlay.markSeconds - s_Mark.age;
        const int s_Alpha = static_cast<int>(255.f * std::clamp(s_Left / 3.f, 0.f, 1.f));
        const ImU32 s_Colour = s_Mark.danger ? WithAlpha(s_Palette.danger, s_Alpha) : s_Mark.ours ? FromRgb(s_Ours ? s_Ours : 0x6EC8FF, s_Alpha) :
            PartnerColour(p_Status, s_Alpha);
        const std::string s_Text = std::string(s_Mark.danger ? "DANGER: " : s_Mark.target ? "TARGET: " : "") + s_Mark.label + "  " + Dist(s_Mark.distance);

        if (!s_OnScreen) {
            EdgeArrow(s_List, s_At, s_Colour, s_Text, s_Scale);
            continue;
        }

        const float s_Beat = s_Mark.danger ? 4.f * std::fabs(std::sin(s_Mark.age * 6.f)) : 0.f;
        const float s_Size = (9.f + s_Beat + (s_Mark.age < 1.f ? 8.f * (1.f - s_Mark.age) : 0.f)) * s_Scale;
        s_List->AddQuadFilled(ImVec2(s_At.x, s_At.y - s_Size), ImVec2(s_At.x + s_Size, s_At.y), ImVec2(s_At.x, s_At.y + s_Size),
            ImVec2(s_At.x - s_Size, s_At.y), s_Colour);
        s_List->AddQuad(ImVec2(s_At.x, s_At.y - s_Size - 3.f), ImVec2(s_At.x + s_Size + 3.f, s_At.y), ImVec2(s_At.x, s_At.y + s_Size + 3.f),
            ImVec2(s_At.x - s_Size - 3.f, s_At.y), IM_COL32(0, 0, 0, s_Alpha / 2), 2.f);

        if (s_Mark.target)
            s_List->AddCircle(s_At, s_Size + 7.f, s_Colour, 24, 2.f);

        TextShadowed(s_List, ImVec2(s_At.x, s_At.y - s_Size - 4.f), s_Colour, s_Text, s_Scale);
    }
}

// A small radar by the HUD: the partner, marks (and targets, if asked) around our 47, turned with the camera.
void HitmanCoop::DrawRadar(const SdkWorld::Overlay& p_Overlay, const coop::SessionStatus& p_Status, float p_Top) {
    if (!m_ShowRadar || !p_Overlay.inMission || p_Overlay.hidden || !p_Status.synced)
        return;

    // Flat directions: forward (up on the radar) and the screen's right.
    coop::Vec3 s_Fwd { p_Overlay.cameraForward.x, p_Overlay.cameraForward.y, 0.f };
    coop::Vec3 s_Right { p_Overlay.cameraRight.x, p_Overlay.cameraRight.y, 0.f };
    const float s_FwdLen = coop::Length(s_Fwd), s_RightLen = coop::Length(s_Right);

    if (s_FwdLen < 1e-3f || s_RightLen < 1e-4f)
        return;

    s_Fwd = s_Fwd * (1.f / s_FwdLen);
    s_Right = s_Right * (1.f / s_RightLen);

    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    const float s_Radius = 64.f * m_HudScale * m_RadarSize;
    const bool s_Left = m_HudCorner == 1 || m_HudCorner == 3;
    const bool s_Bottom = m_HudCorner >= 2;
    const ImVec2 s_Centre(s_Left ? s_View->Pos.x + 20.f + s_Radius : s_View->Pos.x + s_View->Size.x - 20.f - s_Radius,
        s_Bottom ? p_Top - 12.f - s_Radius : p_Top + 12.f + s_Radius);
    static constexpr float c_Ranges[] = { 25.f, 50.f, 100.f };
    const float s_Range = c_Ranges[std::clamp(m_RadarRange, 0, 2)];
    const Palette s_Palette = Colours(m_ColourBlind);

    ImDrawList* s_List = ImGui::GetBackgroundDrawList();
    const int s_Bg = static_cast<int>(255.f * std::clamp(m_HudOpacity + 0.1f, 0.f, 1.f));
    s_List->AddCircleFilled(s_Centre, s_Radius, IM_COL32(10, 10, 10, s_Bg), 48);
    s_List->AddCircle(s_Centre, s_Radius, IM_COL32(220, 30, 40, 200), 48, 1.5f);
    s_List->AddCircle(s_Centre, s_Radius * 0.5f, IM_COL32(255, 255, 255, 40), 32, 1.f);
    // Our 47: a small arrow pointing the way we look.
    s_List->AddTriangleFilled(ImVec2(s_Centre.x, s_Centre.y - 6.f), ImVec2(s_Centre.x - 4.f, s_Centre.y + 4.f),
        ImVec2(s_Centre.x + 4.f, s_Centre.y + 4.f), IM_COL32(255, 255, 255, 230));

    char s_RangeText[24];
    std::snprintf(s_RangeText, sizeof(s_RangeText), m_Feet ? "%.0f ft" : "%.0f m", m_Feet ? s_Range * 3.28084f : s_Range);
    s_List->AddText(ImVec2(s_Centre.x - s_Radius * 0.3f, s_Centre.y + s_Radius - ImGui::GetFontSize() - 2.f), IM_COL32(255, 255, 255, 90), s_RangeText);

    const auto Plot = [&](const coop::Vec3& p_At, ImU32 p_Colour, float p_Size, int p_Shape) {
        const coop::Vec3 s_D { p_At.x - p_Overlay.local.x, p_At.y - p_Overlay.local.y, 0.f };
        float s_X = coop::Dot(s_D, s_Right) / s_Range, s_Y = coop::Dot(s_D, s_Fwd) / s_Range;
        const float s_Len = std::sqrt(s_X * s_X + s_Y * s_Y);

        // Out of range: held on the rim, smaller.
        if (s_Len > 0.92f) {
            s_X *= 0.92f / s_Len;
            s_Y *= 0.92f / s_Len;
            p_Size *= 0.7f;
        }

        const ImVec2 s_P(s_Centre.x + s_X * s_Radius, s_Centre.y - s_Y * s_Radius);

        if (p_Shape == 1)
            s_List->AddQuadFilled(ImVec2(s_P.x, s_P.y - p_Size), ImVec2(s_P.x + p_Size, s_P.y), ImVec2(s_P.x, s_P.y + p_Size),
                ImVec2(s_P.x - p_Size, s_P.y), p_Colour);
        else if (p_Shape == 2)
            s_List->AddCircle(s_P, p_Size, p_Colour, 16, 2.f);
        else
            s_List->AddCircleFilled(s_P, p_Size, p_Colour, 16);

        return s_P;
    };

    for (const auto& s_Target : p_Overlay.targets)
        Plot(s_Target, IM_COL32(230, 30, 40, 230), 5.f, 2);

    const uint32_t s_Ours = m_ColourSetting;

    for (const auto& s_Mark : p_Overlay.marks)
        Plot(s_Mark.ground, s_Mark.danger ? s_Palette.danger : s_Mark.ours ? FromRgb(s_Ours ? s_Ours : 0x6EC8FF, 240) : PartnerColour(p_Status, 240), 4.5f, 1);

    if (p_Overlay.partnerShown) {
        const ImVec2 s_P = Plot(p_Overlay.partnerFeet, PartnerColour(p_Status, 255), 5.f, 0);

        // Above or below us: a tick that says which.
        if (p_Overlay.partnerHeight > 2.5f)
            s_List->AddTriangleFilled(ImVec2(s_P.x, s_P.y - 12.f), ImVec2(s_P.x - 4.f, s_P.y - 7.f), ImVec2(s_P.x + 4.f, s_P.y - 7.f),
                IM_COL32(255, 255, 255, 220));
        else if (p_Overlay.partnerHeight < -2.5f)
            s_List->AddTriangleFilled(ImVec2(s_P.x, s_P.y + 12.f), ImVec2(s_P.x - 4.f, s_P.y + 7.f), ImVec2(s_P.x + 4.f, s_P.y + 7.f),
                IM_COL32(255, 255, 255, 220));
    }
}

// A compass strip at the top of the screen: which way the partner and the marks are.
void HitmanCoop::DrawCompass(const SdkWorld::Overlay& p_Overlay, const coop::SessionStatus& p_Status) {
    if (!m_ShowCompass || !p_Overlay.inMission || p_Overlay.hidden || !p_Status.synced)
        return;

    coop::Vec3 s_Fwd { p_Overlay.cameraForward.x, p_Overlay.cameraForward.y, 0.f };
    coop::Vec3 s_Right { p_Overlay.cameraRight.x, p_Overlay.cameraRight.y, 0.f };

    if (coop::Length(s_Fwd) < 1e-3f || coop::Length(s_Right) < 1e-4f)
        return;

    s_Fwd = s_Fwd * (1.f / coop::Length(s_Fwd));
    s_Right = s_Right * (1.f / coop::Length(s_Right));

    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    const float s_Width = 420.f * m_HudScale, s_Height = 22.f * m_HudScale;
    const ImVec2 s_Min(s_View->Pos.x + (s_View->Size.x - s_Width) * 0.5f, s_View->Pos.y + 14.f);
    const ImVec2 s_Max(s_Min.x + s_Width, s_Min.y + s_Height);
    ImDrawList* s_List = ImGui::GetBackgroundDrawList();
    s_List->AddRectFilled(s_Min, s_Max, IM_COL32(10, 10, 10, static_cast<int>(255.f * m_HudOpacity)), 4.f);
    s_List->AddLine(ImVec2((s_Min.x + s_Max.x) * 0.5f, s_Min.y), ImVec2((s_Min.x + s_Max.x) * 0.5f, s_Max.y), IM_COL32(255, 255, 255, 120), 1.f);

    // Bearing, -90 to 90 degrees across the strip; behind us held at its ends.
    const auto Place = [&](const coop::Vec3& p_At, ImU32 p_Colour, const char* p_Label) {
        const coop::Vec3 s_D { p_At.x - p_Overlay.local.x, p_At.y - p_Overlay.local.y, 0.f };
        const float s_Angle = std::atan2(coop::Dot(s_D, s_Right), coop::Dot(s_D, s_Fwd));
        const float s_T = std::clamp(s_Angle / 3.14159265f * 2.f, -1.f, 1.f);
        const ImVec2 s_P((s_Min.x + s_Max.x) * 0.5f + s_T * (s_Width * 0.5f - 10.f), (s_Min.y + s_Max.y) * 0.5f);
        s_List->AddCircleFilled(s_P, 5.f * m_HudScale, p_Colour, 16);

        if (p_Label)
            TextShadowed(s_List, ImVec2(s_P.x, s_Max.y + ImGui::GetFontSize() + 2.f), p_Colour, p_Label);
    };

    for (const auto& s_Mark : p_Overlay.marks)
        Place(s_Mark.ground, s_Mark.danger ? Colours(m_ColourBlind).danger : s_Mark.ours ? FromRgb(m_ColourSetting ? m_ColourSetting.load() : 0x6EC8FF, 230) :
            PartnerColour(p_Status, 230), nullptr);

    if (p_Overlay.partnerShown)
        Place(p_Overlay.partnerFeet, PartnerColour(p_Status, 255), p_Status.peerName.c_str());
}

// The 3-2-1 countdown, big in the middle of the screen, then GO.
void HitmanCoop::DrawCountdown() {
    double s_End;

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_End = m_OverlayCountdownEnd;
    }

    const double s_Left = s_End - Now();

    if (s_End < 0.0 || s_Left < -1.2 || s_Left > 10.0)
        return;

    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    const bool s_Go = s_Left <= 0.0;
    const std::string s_Text = s_Go ? "GO!" : std::to_string(static_cast<int>(std::ceil(s_Left)));
    const float s_Phase = static_cast<float>(s_Go ? -s_Left : std::ceil(s_Left) - s_Left); // 0 as each number appears
    const int s_Alpha = static_cast<int>(255.f * std::clamp(s_Go ? 1.2f - s_Phase : 1.f, 0.f, 1.f));
    TextShadowed(ImGui::GetForegroundDrawList(), ImVec2(s_View->Pos.x + s_View->Size.x * 0.5f, s_View->Pos.y + s_View->Size.y * 0.38f),
        s_Go ? IM_COL32(120, 255, 140, s_Alpha) : IM_COL32(255, 210, 80, s_Alpha), s_Text, 5.f - 1.5f * s_Phase);
}

// The mission's end: both players side by side, for half a minute.
// What's new in the version the mod just updated itself to: for a while on its first start.
void HitmanCoop::DrawWhatsNew() {
    std::string s_Notes;

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_Notes = m_WhatsNew;
    }

    if (s_Notes.empty() || m_HideOverlay)
        return;

    if (m_WhatsNewAt < 0.0)
        m_WhatsNewAt = Now();

    if (Now() - m_WhatsNewAt > 45.0)
        return;

    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(s_View->Pos.x + s_View->Size.x * 0.5f, s_View->Pos.y + 60.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(std::min(640.f * m_HudScale, s_View->Size.x - 40.f), s_View->Size.y * 0.6f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    if (ImGui::Begin("##HitmanCoopWhatsNew", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * m_HudScale);
        ImGui::TextColored(c_Red, ICON_MD_SYSTEM_UPDATE " HITMAN CO-OP UPDATED TO %s", c_ModVersion);
        ImGui::PushTextWrapPos(std::min(620.f * m_HudScale, s_View->Size.x - 60.f));
        ImGui::TextUnformatted(s_Notes.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("Also in CO-OP > About.");
        ImGui::PopFont();
    }

    ImGui::End();
}

void HitmanCoop::DrawSummary() {
    Summary s_Summary;

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_Summary = m_Summary;
    }

    if (Now() - s_Summary.at > 30.0 || m_HideOverlay)
        return;

    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(s_View->Pos.x + s_View->Size.x * 0.5f, s_View->Pos.y + 60.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.8f);

    if (ImGui::Begin("##HitmanCoopSummary", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * m_HudScale);
        ImGui::TextColored(c_Red, ICON_MD_FLAG " MISSION COMPLETE");
        ImGui::SameLine();
        ImGui::TextDisabled("in %s", Clock(s_Summary.seconds).c_str());

        if (ImGui::BeginTable("##HitmanCoopSummaryTable", s_Summary.theirsKnown ? 3 : 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("You");

            if (s_Summary.theirsKnown)
                ImGui::TableSetupColumn(s_Summary.partner.c_str());

            ImGui::TableHeadersRow();

            const auto Row = [&](const char* p_Name, uint16_t coop::PlayerStats::*p_Field) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p_Name);
                ImGui::TableNextColumn();
                ImGui::Text("%u", s_Summary.ours.stats.*p_Field);

                if (s_Summary.theirsKnown) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", s_Summary.theirs.stats.*p_Field);
                }
            };

            Row("Targets", &coop::PlayerStats::targets);
            Row("Other kills", &coop::PlayerStats::nonTargetKills);
            Row("Knocked out", &coop::PlayerStats::knockouts);
            Row("Spotted", &coop::PlayerStats::spotted);
            Row("Bodies found", &coop::PlayerStats::bodiesFound);
            Row("Challenges", &coop::PlayerStats::challenges);
            ImGui::EndTable();
        }

        const Palette s_Palette = Colours(m_ColourBlind);

        if (s_Summary.silentAssassin.empty())
            ImGui::TextColored(s_Palette.good, ICON_MD_STAR " Team Silent Assassin, as far as co-op can tell");
        else
            ImGui::TextColored(c_Gold, ICON_MD_STAR_BORDER " No Silent Assassin: %s", s_Summary.silentAssassin.c_str());

        ImGui::PopFont();
    }

    ImGui::End();
}

void HitmanCoop::DrawHud(const coop::SessionStatus& p_Status, bool p_InMission, const SdkWorld::Overlay& p_Overlay, bool p_Reconnecting) {
    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    const bool s_Left = m_HudCorner == 1 || m_HudCorner == 3;
    const bool s_Bottom = m_HudCorner >= 2;
    // Below the corner (where arrows to marks off the top of the screen go), or above the bottom one.
    m_HudBottom = s_Bottom ? s_View->Pos.y + s_View->Size.y - 140.f : s_View->Pos.y + 96.f;

    if (!m_ShowHud || m_HideOverlay || (p_Status.role == coop::SessionRole::None && !p_Reconnecting))
        return;

    coop::MsgPlayerState s_Ours;
    bool s_Waiting;
    double s_WaitLeft, s_Together, s_Still;
    int s_Ready;

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_Ours = m_OverlayOurs;
        s_Waiting = m_OverlayWaiting;
        s_WaitLeft = m_OverlayWaitLeft;
        s_Together = m_OverlayTogether;
        s_Ready = m_OverlayReady;
        s_Still = m_OverlayPartnerStill;
    }

    const Palette s_Palette = Colours(m_ColourBlind);
    ImGui::SetNextWindowPos(ImVec2(s_Left ? s_View->Pos.x + 20.f : s_View->Pos.x + s_View->Size.x - 20.f, m_HudBottom), ImGuiCond_Always,
        ImVec2(s_Left ? 0.f : 1.f, s_Bottom ? 1.f : 0.f));
    ImGui::SetNextWindowBgAlpha(m_HudOpacity);

    if (ImGui::Begin("##HitmanCoopHud", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * m_HudScale);
        ImGui::TextColored(c_Red, ICON_MD_PEOPLE " CO-OP");
        ImGui::SameLine();

        const bool s_Connected = p_Status.state == coop::SessionState::Connected;

        if (s_Connected) {
            const double s_Ping = p_Status.stats.rttMs;
            const double s_Loss = p_Status.stats.lossPercent;
            // Connection quality: ping and loss together.
            const int s_Bars = s_Ping <= 0.0 ? 0 : s_Ping < 80.0 && s_Loss < 2.0 ? 3 : s_Ping < 160.0 && s_Loss < 8.0 ? 2 : 1;
            const ImVec4 s_PingColour = s_Bars == 3 ? s_Palette.good : s_Bars == 2 ? c_Gold : s_Palette.bad;
            ImGui::TextUnformatted(p_Status.peerName.c_str());
            ImGui::SameLine();

            if (s_Ping <= 0.0)
                ImGui::TextDisabled(ICON_MD_SIGNAL_CELLULAR_ALT " ...");
            else if (s_Loss >= 2.0)
                ImGui::TextColored(s_PingColour, ICON_MD_SIGNAL_CELLULAR_ALT " %.0f ms, %.0f%% lost", s_Ping, s_Loss);
            else
                ImGui::TextColored(s_PingColour, ICON_MD_SIGNAL_CELLULAR_ALT " %.0f ms", s_Ping);

            if (p_Status.synced && p_Overlay.partnerShown) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", m_World.Distance(p_Overlay.partnerDistance).c_str());
            }

            if (s_Together >= 0.0) {
                ImGui::SameLine();
                ImGui::TextDisabled(ICON_MD_TIMER " %s", Clock(s_Together).c_str());
            }

            if (const size_t s_Unread = m_World.PartnerChatCount() - std::min(m_ChatSeen, m_World.PartnerChatCount()); s_Unread > 0) {
                ImGui::SameLine();
                ImGui::TextColored(c_Chat, ICON_MD_CHAT " %zu", s_Unread);
            }
        }
        else if (p_Reconnecting)
            ImGui::TextColored(c_Gold, ICON_MD_REFRESH " Reconnecting...");
        else if (p_Status.state == coop::SessionState::Listening)
            ImGui::TextDisabled("waiting for a friend");
        else
            ImGui::TextDisabled("%s", coop::SessionStateName(p_Status.state));

        // What the partner is up to.
        if (s_Connected) {
            const auto& s_Theirs = p_Status.peerState;
            const int s_Menu = p_Status.peerStateKnown ? s_Theirs.menu : 0;

            if (s_Waiting)
                ImGui::TextColored(c_Gold, ICON_MD_HOURGLASS_EMPTY " Waiting for %s to load in (%s)...", p_Status.peerName.c_str(),
                    Clock(s_WaitLeft).c_str());

            if (s_Ready == 2)
                ImGui::TextColored(c_Gold, ICON_MD_HOW_TO_REG " %s asks: ready? (%s)", p_Status.peerName.c_str(),
                    Hotkeys::Name(Hotkeys::Get(Hotkeys::Action::ReadyCheck)).c_str());
            else if (s_Ready == 1)
                ImGui::TextDisabled(ICON_MD_HOW_TO_REG " Waiting for %s to be ready...", p_Status.peerName.c_str());

            if (p_Status.peerSilence > 2.5 && !p_Status.peerBusy)
                ImGui::TextColored(s_Palette.bad, ICON_MD_WARNING " No word from %s for %.0f s...", p_Status.peerName.c_str(), p_Status.peerSilence);
            else if (p_Status.peerBusy)
                ImGui::TextColored(c_Gold, ICON_MD_HOURGLASS_EMPTY " %s is %s...", p_Status.peerName.c_str(), coop::LoadStageName(p_Status.peerBusyStage));
            else if (p_Status.peerPaused || s_Menu > 0)
                ImGui::TextColored(c_Gold, ICON_MD_PAUSE " %s is in %s%s", p_Status.peerName.c_str(), s_Menu > 0 ? MenuName(s_Menu) : "the pause menu",
                    p_Status.peerPaused && !m_PauseTogether ? " (yours keeps running)" : "");
            else if (p_Status.peerStateKnown && (s_Theirs.flags & coop::PlayerFlag_Away))
                ImGui::TextColored(c_Gold, ICON_MD_DESKTOP_WINDOWS " %s tabbed out of the game", p_Status.peerName.c_str());
            else if (p_Status.synced && s_Still > 120.0)
                ImGui::TextDisabled(ICON_MD_ACCESSIBILITY " %s hasn't moved for %s", p_Status.peerName.c_str(), Clock(s_Still).c_str());
            else if (!p_Status.synced && p_InMission)
                ImGui::TextDisabled("Not in the same mission yet");
            else if (!p_Status.synced)
                ImGui::TextDisabled("In the menus");

            if (p_Status.synced && p_Status.peerStateKnown) {
                const bool s_Trespassing = (s_Theirs.flags & coop::PlayerFlag_Trespassing) != 0;
                const int s_Threat = std::min<int>(s_Theirs.threat, coop::Threat_Count - 1);

                if (s_Threat > 0 || s_Trespassing)
                    ImGui::TextColored(s_Threat >= coop::Threat_Hunted ? s_Palette.bad : c_Gold, ICON_MD_VISIBILITY " %s: %s%s", p_Status.peerName.c_str(),
                        c_ThreatNames[s_Threat], s_Trespassing ? ", trespassing" : "");

                if (!p_Overlay.partnerDisguise.empty() && p_Overlay.partnerDisguise != "their suit")
                    ImGui::TextDisabled("%s is disguised: %s", p_Status.peerName.c_str(), p_Overlay.partnerDisguise.c_str());

                // The team's Silent Assassin, as both games' events have it so far.
                const std::string s_Broken = coop::GameEvents::SilentAssassinBroken(s_Ours, s_Theirs);

                if (s_Broken.empty())
                    ImGui::TextColored(s_Palette.good, ICON_MD_STAR " Silent Assassin: on track");
                else
                    ImGui::TextDisabled(ICON_MD_STAR_BORDER " Silent Assassin lost: %s", s_Broken.c_str());
            }

            if (p_Status.congested)
                ImGui::TextColored(s_Palette.bad, ICON_MD_WARNING " Connection struggling");
        }

        ImGui::PopFont();
        m_HudBottom = s_Bottom ? ImGui::GetWindowPos().y : ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    }

    ImGui::End();
}

void HitmanCoop::DrawToasts() {
    if (!m_ShowToasts || m_HideOverlay)
        return;

    const auto s_Toasts = m_World.CopyToasts();
    const double s_Now = Now();
    const double s_Seconds = m_ToastSeconds;
    bool s_Any = false;

    for (const auto& s_Toast : s_Toasts)
        s_Any |= s_Now - s_Toast.time < s_Seconds;

    if (!s_Any)
        return;

    // Under the game's objectives list (top left), or top right when the HUD is in the top left.
    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    const bool s_Right = m_HudCorner == 1;
    ImGui::SetNextWindowPos(ImVec2(s_Right ? s_View->Pos.x + s_View->Size.x - 20.f : s_View->Pos.x + 20.f, s_View->Pos.y + 160.f), ImGuiCond_Always,
        ImVec2(s_Right ? 1.f : 0.f, 0.f));
    // Long lines wrap instead of running off the screen.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(s_View->Size.x * 0.42f, s_View->Size.y * 0.6f));
    ImGui::SetNextWindowBgAlpha(0.6f);

    if (ImGui::Begin("##HitmanCoopToasts", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * m_HudScale);
        ImGui::PushTextWrapPos(s_View->Size.x * 0.4f);
        const Palette s_Palette = Colours(m_ColourBlind);

        for (const auto& s_Toast : s_Toasts) {
            const double s_Age = s_Now - s_Toast.time;

            if (s_Age >= s_Seconds)
                continue;

            ImVec4 s_Colour = s_Toast.kind == SdkWorld::ToastKind::Warning ? s_Palette.bad : s_Toast.kind == SdkWorld::ToastKind::Partner ? c_Gold :
                s_Toast.kind == SdkWorld::ToastKind::Chat ? c_Chat : c_White;
            s_Colour.w = static_cast<float>(std::clamp(s_Seconds - s_Age, 0.0, 1.0));
            const char* s_Icon = s_Toast.kind == SdkWorld::ToastKind::Chat ? ICON_MD_CHAT " " :
                s_Toast.kind == SdkWorld::ToastKind::Warning ? ICON_MD_WARNING " " : ICON_MD_PEOPLE " ";
            std::string s_Line = s_Icon + s_Toast.text;

            if (s_Toast.count > 1)
                s_Line += "  x" + std::to_string(s_Toast.count);

            ImGui::PushStyleColor(ImGuiCol_Text, s_Colour);
            ImGui::TextWrapped("%s", s_Line.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }

    ImGui::End();
}

// ---- The CO-OP window ---------------------------------------------------------------------------

void HitmanCoop::OnDrawMenu() {
    if (ImGui::Button(ICON_MD_PEOPLE " CO-OP"))
        m_ShowWindow = !m_ShowWindow;
}

void HitmanCoop::DrawHeader(const coop::SessionStatus& p_Status, bool p_InMission) {
    ImGui::PushFont(SDK()->GetImGuiBlackFont(), ImGui::GetStyle().FontSizeBase * m_WindowScale * 1.5f);
    ImGui::TextColored(c_Red, "HITMAN  CO-OP");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(c_Gold, ICON_MD_STAR " by %s", c_Author);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize("v0.00.00").x);
    ImGui::TextDisabled("v%s", c_ModVersion);

    // Where things stand, in one line with a coloured dot.
    const Palette s_Palette = Colours(m_ColourBlind);
    ImVec4 s_Dot = ImVec4(0.5f, 0.5f, 0.5f, 1.f);
    std::string s_Line = "Not in a session";

    if (p_Status.state == coop::SessionState::Listening) {
        s_Dot = c_Gold;
        s_Line = "Hosting: waiting for a friend to join";
    }
    else if (p_Status.state == coop::SessionState::Connecting) {
        s_Dot = c_Gold;
        s_Line = "Connecting...";
    }
    else if (p_Status.state == coop::SessionState::Failed) {
        s_Dot = s_Palette.bad;
        s_Line = p_Status.message.empty() ? std::string("Disconnected") : p_Status.message;
    }
    else if (p_Status.state == coop::SessionState::Connected) {
        s_Dot = p_Status.synced ? s_Palette.good : c_Gold;
        char s_Text[200];
        std::snprintf(s_Text, sizeof(s_Text), "%s %s  -  %.0f ms  -  %s", p_Status.role == coop::SessionRole::Host ? "Hosting" : "Joined",
            p_Status.peerName.c_str(), p_Status.stats.rttMs,
            p_Status.synced ? "in the mission together" : p_InMission ? "not in the same mission yet" : "in the menus");
        s_Line = s_Text;
    }

    ImGui::TextColored(s_Dot, ICON_MD_CIRCLE);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", s_Line.c_str());

    if (p_Status.state == coop::SessionState::Connected && !p_Status.peerModVersion.empty() && p_Status.peerModVersion != c_ModVersion)
        ImGui::TextColored(c_Gold, ICON_MD_INFO " %s has HITMAN Co-op %s, you have %s.", p_Status.peerName.c_str(), p_Status.peerModVersion.c_str(), c_ModVersion);

    if (p_Status.state == coop::SessionState::Connected && !p_Status.peerGameVersion.empty() && p_Status.peerGameVersion != m_GameVersion)
        ImGui::TextColored(c_Gold, ICON_MD_INFO " Different game versions (%s here, %s there): some NPCs may not sync.", m_GameVersion.c_str(),
            p_Status.peerGameVersion.c_str());

    if (!m_Unsupported.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, c_Red);
        ImGui::TextWrapped(
            "This game version (%s) isn't compatible with HITMAN Co-op %s: the mod loader couldn't find %s in it. "
            "Co-op stays off so nothing can break. Update the game, then get the HITMAN Co-op build made for it.",
            m_GameVersion.c_str(), c_ModVersion, m_Unsupported.c_str());
        ImGui::PopStyleColor();
    }
}

void HitmanCoop::DrawPlayTab(const coop::SessionStatus& p_Status, bool p_InMission, const std::optional<coop::RelayState>& p_Relay) {
    const bool s_Active = p_Status.role != coop::SessionRole::None;
    const Palette s_Palette = Colours(m_ColourBlind);

    if (!s_Active) {
        SectionTitle(ICON_MD_PERSON " You");
        ImGui::SetNextItemWidth(260.f * m_WindowScale);

        if (ImGui::InputText("Your name", m_NameBuffer, sizeof(m_NameBuffer)))
            SaveSettings();

        if (ImGui::BeginTable("##HitmanCoopStart", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextColumn();
            SectionTitle(ICON_MD_WIFI_TETHERING " Host a game");
            ImGui::TextWrapped(m_RelayBuffer[0] ? "You get a join code to give your friend." : "No co-op server is set (Settings): friends join by IP address.");

            if (!m_LanAddress.empty())
                ImGui::TextDisabled("This PC on your network: %s", Hide(m_LanAddress).c_str());

            if (!m_TailscaleAddress.empty())
                ImGui::TextDisabled("Tailscale: %s", Hide(m_TailscaleAddress).c_str());

            ImGui::Spacing();

            if (WideButton(ICON_MD_WIFI_TETHERING " HOST", 36.f * m_WindowScale)) {
                SaveSettings();
                PushCommand({ Command::Kind::Host, {}, static_cast<uint16_t>(m_Port), m_NameBuffer, m_RelayBuffer });
            }

            ImGui::TableNextColumn();
            SectionTitle(ICON_MD_LOGIN " Join a friend");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##HitmanCoopAddress", "Join code (like K7P2QX) or IP address", m_AddressBuffer, sizeof(m_AddressBuffer),
                m_Streamer ? ImGuiInputTextFlags_Password : 0);

            if (ImGui::Button(ICON_MD_CONTENT_PASTE " Paste")) {
                // The code from a chat message, without its spaces.
                if (const char* s_Clip = ImGui::GetClipboardText()) {
                    std::string s_Clean;

                    for (const char* c = s_Clip; *c && s_Clean.size() + 1 < sizeof(m_AddressBuffer); ++c)
                        if (static_cast<unsigned char>(*c) > 0x20)
                            s_Clean += *c;

                    strncpy_s(m_AddressBuffer, s_Clean.c_str(), _TRUNCATE);
                }
            }

            ImGui::SameLine();

            if (WideButton(ICON_MD_LOGIN " JOIN", 0.f) && m_AddressBuffer[0] != '\0') {
                RememberJoin(m_AddressBuffer);
                SaveSettings();
                PushCommand({ Command::Kind::Join, m_AddressBuffer, 0, m_NameBuffer, m_RelayBuffer });
            }

            if (!m_RecentJoins.empty()) {
                ImGui::TextDisabled("Recent:");

                for (size_t i = 0; i < m_RecentJoins.size(); ++i) {
                    ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(i));

                    if (ImGui::SmallButton(m_Streamer ? ("#" + std::to_string(i + 1)).c_str() : m_RecentJoins[i].c_str()))
                        strncpy_s(m_AddressBuffer, m_RecentJoins[i].c_str(), _TRUNCATE);

                    ImGui::PopID();
                }
            }

            ImGui::EndTable();
        }

        bool s_CanReconnect;

        {
            std::lock_guard s_Lock(m_StatusMutex);
            s_CanReconnect = m_OverlayCanReconnect;
        }

        if (s_CanReconnect && ImGui::Button(ICON_MD_REFRESH " Reconnect now"))
            PushCommand({ Command::Kind::Reconnect });

        ImGui::Spacing();
        ImGui::TextDisabled("Both players need HITMAN Co-op %s and a location they both own. Start a mission, then host or join.", c_ModVersion);
        return;
    }

    // The join code, big, while hosting.
    if (p_Relay && p_Status.role == coop::SessionRole::Host) {
        if (p_Relay->phase == coop::RelayState::Phase::Ready) {
            if (m_AutoCopyCode && m_CopiedCode != p_Relay->code) {
                m_CopiedCode = p_Relay->code;
                ImGui::SetClipboardText(p_Relay->code.c_str());
                m_CodeCopied = true;
            }

            SectionTitle(ICON_MD_KEY " Join code");
            ImGui::PushFont(SDK()->GetImGuiBlackFont(), ImGui::GetStyle().FontSizeBase * m_WindowScale * 2.f);
            ImGui::TextColored(c_Gold, "%s", m_Streamer ? "- - - - - -" : p_Relay->code.c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();

            if (ImGui::Button(ICON_MD_CONTENT_COPY " Copy code"))
                ImGui::SetClipboardText(p_Relay->code.c_str());

            ImGui::SameLine();

            if (ImGui::Button(ICON_MD_SHARE " Copy invite")) {
                const std::string s_Invite = "Join me in HITMAN Co-op (by Jordan Rodrigues, version " + std::string(c_ModVersion) +
                    "): start any mission at the same location, press ~, click CO-OP and type the join code " + p_Relay->code + ".";
                ImGui::SetClipboardText(s_Invite.c_str());
            }

            ImGui::TextDisabled("Your friend types it under \"Join a friend\".%s", m_Streamer ? " (Hidden: streamer mode.)" : "");
        }
        else if (p_Relay->phase == coop::RelayState::Phase::Connecting)
            ImGui::TextDisabled("Getting a join code from the co-op server...");
        else
            ImGui::TextColored(s_Palette.bad, "Co-op server: %s", p_Relay->error.c_str());
    }

    if (p_Status.state == coop::SessionState::Connected) {
        SectionTitle(ICON_MD_PERSON " Your partner");
        ImGui::Text("%s  -  HITMAN Co-op %s  -  game %s", p_Status.peerName.c_str(), p_Status.peerModVersion.empty() ? "?" : p_Status.peerModVersion.c_str(),
            p_Status.peerGameVersion.empty() ? "?" : p_Status.peerGameVersion.c_str());
        ImGui::Text("Ping %.0f ms, loss %.1f%%, jitter %.0f ms%s", p_Status.stats.rttMs, p_Status.stats.lossPercent, p_Status.stats.jitterMs,
            p_Relay ? " (through the co-op server)" : "");

        if (p_Status.role == coop::SessionRole::Client && !p_Status.hostScene.Empty() && !p_Status.synced) {
            const std::string s_Where = coop::LocationDisplayName(p_Status.hostScene.Location());
            ImGui::TextWrapped("%s is in %s (%s). %s", p_Status.peerName.c_str(), s_Where.c_str(), p_Status.hostScene.codename.c_str(),
                m_FollowHost ? "Start any mission there and you'll be taken to theirs." : "Start the same mission to join them.");
        }

        if (p_Status.role == coop::SessionRole::Host && !p_Status.peerScene.Empty() && !p_Status.synced)
            ImGui::TextWrapped("%s is in %s. Play a location you both own.", p_Status.peerName.c_str(),
                coop::LocationDisplayName(p_Status.peerScene.Location()).c_str());

        if (!p_InMission)
            ImGui::TextDisabled("You are in the menus.");

        bool s_Waiting;
        int s_Ready;

        {
            std::lock_guard s_Lock(m_StatusMutex);
            s_Waiting = m_OverlayWaiting;
            s_Ready = m_OverlayReady;
        }

        if (s_Waiting && ImGui::Button(ICON_MD_PLAY_ARROW " Start without waiting for them"))
            m_SkipWait = true;

        // Teamwork, in a grid of buttons (each also has a hotkey).
        SectionTitle(ICON_MD_GROUPS " Teamwork");
        const auto Key = [](Hotkeys::Action p_Action) {
            const std::string s_Name = Hotkeys::Name(Hotkeys::Get(p_Action));
            return s_Name.empty() ? std::string() : " (" + s_Name + ")";
        };

        if (ImGui::BeginTable("##HitmanCoopTeam", 3, ImGuiTableFlags_SizingStretchSame)) {
            const auto Cell = [&](const std::string& p_Label, bool p_Enabled, const char* p_Tip, Command p_Command) {
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!p_Enabled);

                if (WideButton(p_Label.c_str()))
                    PushCommand(p_Command);

                ImGui::EndDisabled();

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", p_Tip);
            };
            const bool s_Synced = p_Status.synced;

            Cell(ICON_MD_NEAR_ME " Regroup" + Key(Hotkeys::Action::Regroup), s_Synced, "Puts your 47 right behind your partner's (every 10 s at most).",
                { Command::Kind::Regroup });
            Cell(ICON_MD_MY_LOCATION " Where are you?" + Key(Hotkeys::Action::WhereAreYou), s_Synced, "Both name tags pulse for a few seconds.",
                { Command::Kind::Ping });
            Cell(ICON_MD_CALL_RECEIVED " Come to me" + Key(Hotkeys::Action::ComeHere), s_Synced, "Asks your partner to come to you.",
                { Command::Kind::ComeHere });
            Cell(std::string(ICON_MD_HOW_TO_REG) + (s_Ready == 2 ? " I'm ready!" : " Ready check") + Key(Hotkeys::Action::ReadyCheck), true,
                "Asks your partner if they're ready; or, when they asked, answers yes.", { Command::Kind::ReadyCheck });
            Cell(ICON_MD_TIMER " 3-2-1 countdown" + Key(Hotkeys::Action::Countdown), true,
                "A countdown on both screens that reaches GO at the same moment: for a double hit.", { Command::Kind::Countdown });
            ImGui::TableNextColumn();

            for (int i = 0; i < Signal_Count; ++i) {
                Command s_Signal { Command::Kind::Signal };
                s_Signal.value = i;
                Cell(std::string(c_SignalIcons[i]) + " " + c_SignalWords[i] + Key(static_cast<Hotkeys::Action>(static_cast<int>(Hotkeys::Action::SignalOk) + i)),
                    s_Synced, "Shows over your head on your partner's screen.", s_Signal);
            }

            ImGui::EndTable();
        }

        if (ImGui::CollapsingHeader("Connection details")) {
            ImGui::Text("Partner address: %s", Hide(p_Status.peerAddress).c_str());
            ImGui::Text("Scene epoch %u, NPCs tracked %zu, puppets %zu, run by the client %zu", p_Status.epoch, p_Status.npcsTracked,
                p_Status.npcsPuppeted, p_Status.npcsOwnedByClient);
            ImGui::Text("Sent %llu packets (%.1f KB), received %llu (%.1f KB), resends %llu", p_Status.stats.packetsSent,
                p_Status.stats.bytesSent / 1024.0, p_Status.stats.packetsReceived, p_Status.stats.bytesReceived / 1024.0, p_Status.stats.resends);
            ImGui::Text("Sending %.0f kbit/s, playback delay %.0f ms, mod cost %.2f ms a frame%s", p_Status.sendKbps, p_Status.playbackDelayMs,
                static_cast<double>(m_ModCostMs.load()), m_SlowMode ? " (slow frames: less NPC work)" : "");

            if (!m_PingHistory.empty()) {
                std::vector<float> s_Values(m_PingHistory.begin(), m_PingHistory.end());
                ImGui::PlotLines("##HitmanCoopPing", s_Values.data(), static_cast<int>(s_Values.size()), 0, "ping, last minute", 0.f,
                    std::max(100.f, *std::max_element(s_Values.begin(), s_Values.end()) * 1.2f), ImVec2(-FLT_MIN, 60.f));
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (p_Status.role == coop::SessionRole::Host && p_Status.state == coop::SessionState::Connected) {
        if (ImGui::Button(ICON_MD_PERSON_REMOVE " Remove partner"))
            ImGui::OpenPopup("##HitmanCoopKick");

        if (ImGui::BeginPopup("##HitmanCoopKick")) {
            ImGui::Text("Send %s out of your session?", p_Status.peerName.c_str());

            if (ImGui::Button("Yes, remove them")) {
                PushCommand({ Command::Kind::Kick });
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }

        ImGui::SameLine();
    }

    if (ImGui::Button(ICON_MD_LOGOUT " Leave session"))
        PushCommand({ Command::Kind::Leave });
}

void HitmanCoop::DrawChatTab(const coop::SessionStatus& p_Status) {
    const auto s_Lines = m_World.CopyChat();
    m_ChatSeen = m_World.PartnerChatCount(); // read now
    static size_t s_Shown = 0;
    const double s_Now = Now();
    const float s_InputHeight = ImGui::GetFrameHeightWithSpacing() * 4.f;

    if (ImGui::BeginChild("##HitmanCoopChatLog", ImVec2(0.f, -s_InputHeight), ImGuiChildFlags_Borders)) {
        if (s_Lines.empty())
            ImGui::TextDisabled("Say hello. Your partner sees your messages on their screen.");

        for (const auto& s_Line : s_Lines) {
            ImGui::TextDisabled("%s", Clock(s_Now - s_Line.time).c_str());
            ImGui::SameLine();
            ImGui::TextColored(s_Line.ours ? c_Chat : c_Gold, "%s:", s_Line.ours ? "You" : p_Status.peerName.c_str());
            ImGui::SameLine();

            if (s_Line.quick)
                ImGui::TextColored(c_Gold, "%s", s_Line.text.c_str());
            else
                ImGui::TextWrapped("%s", s_Line.text.c_str());
        }

        if (s_Lines.size() != s_Shown)
            ImGui::SetScrollHereY(1.f);

        s_Shown = s_Lines.size();
    }

    ImGui::EndChild();

    const bool s_Connected = p_Status.state == coop::SessionState::Connected;
    ImGui::BeginDisabled(!s_Connected);
    ImGui::SetNextItemWidth(-160.f * m_WindowScale);

    if (ImGui::InputTextWithHint("##HitmanCoopChat", s_Connected ? "Type a message, Enter to send" : "Not connected", m_ChatBuffer, sizeof(m_ChatBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue) && m_ChatBuffer[0]) {
        PushCommand({ Command::Kind::Chat, m_ChatBuffer });
        m_ChatBuffer[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_SEND " Send") && m_ChatBuffer[0]) {
        PushCommand({ Command::Kind::Chat, m_ChatBuffer });
        m_ChatBuffer[0] = '\0';
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if (ImGui::Button(ICON_MD_DELETE_SWEEP))
        m_World.ClearChat();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clear the chat log");

    // Quick messages, wrapped to the window (yours, as written under Settings).
    ImGui::BeginDisabled(!s_Connected);
    const float s_Right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    for (int i = 0; i < 8; ++i) {
        const std::string s_Text = m_QuickBuffers[i][0] ? m_QuickBuffers[i] : c_DefaultQuickMessages[i];
        ImGui::PushID(i);

        if (ImGui::SmallButton(s_Text.c_str()))
            PushCommand({ Command::Kind::QuickChat, s_Text });

        ImGui::PopID();

        if (i + 1 < 8) {
            const std::string s_Next = m_QuickBuffers[i + 1][0] ? m_QuickBuffers[i + 1] : c_DefaultQuickMessages[i + 1];

            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(s_Next.c_str()).x + 16.f < s_Right)
                ImGui::SameLine();
        }
    }

    ImGui::EndDisabled();
}

void HitmanCoop::DrawMissionTab(const coop::SessionStatus& p_Status) {
    coop::MsgPlayerState s_Ours;
    Record s_Record;

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_Ours = m_OverlayOurs;
        s_Record = m_Record;
    }

    const bool s_Theirs = p_Status.peerStateKnown;
    const Palette s_Palette = Colours(m_ColourBlind);
    SectionTitle(ICON_MD_ASSIGNMENT " This mission");

    if (ImGui::BeginTable("##HitmanCoopStats", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("You");
        ImGui::TableSetupColumn(s_Theirs ? p_Status.peerName.c_str() : "Partner");
        ImGui::TableHeadersRow();

        const auto Row = [&](const char* p_Name, uint16_t coop::PlayerStats::*p_Field) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p_Name);
            ImGui::TableNextColumn();
            ImGui::Text("%u", s_Ours.stats.*p_Field);
            ImGui::TableNextColumn();

            if (s_Theirs)
                ImGui::Text("%u", p_Status.peerState.stats.*p_Field);
            else
                ImGui::TextDisabled("-");
        };

        Row("Targets", &coop::PlayerStats::targets);
        Row("Other kills", &coop::PlayerStats::nonTargetKills);
        Row("Knocked out", &coop::PlayerStats::knockouts);
        Row("Spotted", &coop::PlayerStats::spotted);
        Row("Bodies found", &coop::PlayerStats::bodiesFound);
        Row("Caught on camera", &coop::PlayerStats::recorded);
        Row("Challenges", &coop::PlayerStats::challenges);
        ImGui::EndTable();
    }

    const std::string s_Broken = coop::GameEvents::SilentAssassinBroken(s_Ours, s_Theirs ? p_Status.peerState : coop::MsgPlayerState {});

    if (s_Broken.empty())
        ImGui::TextColored(s_Palette.good, ICON_MD_STAR " Team Silent Assassin: on track");
    else
        ImGui::TextColored(c_Gold, ICON_MD_STAR_BORDER " Team Silent Assassin lost: %s", s_Broken.c_str());

    ImGui::TextDisabled("Worked out from both games' events the way the game scores it; the game's own score is final.");

    SectionTitle(ICON_MD_EMOJI_EVENTS " Your co-op record");
    ImGui::Text("%d missions together (%d as a Silent Assassin team), %s played together", s_Record.missions, s_Record.silentAssassins,
        Clock(s_Record.seconds).c_str());
    ImGui::Text("Your part: %d kills (%d targets), %d knock-outs", s_Record.kills, s_Record.targets, s_Record.knockouts);

    SectionTitle(ICON_MD_NOTIFICATIONS " Notifications");
    DrawHistory();
}

void HitmanCoop::DrawHistory() {
    const auto s_History = m_World.CopyHistory();
    const double s_Now = Now();
    const Palette s_Palette = Colours(m_ColourBlind);

    if (ImGui::BeginChild("##HitmanCoopHistory", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders)) {
        if (s_History.empty())
            ImGui::TextDisabled("Nothing yet.");

        for (auto s_It = s_History.rbegin(); s_It != s_History.rend(); ++s_It) {
            ImGui::TextDisabled("%s ago", Clock(s_Now - s_It->time).c_str());
            ImGui::SameLine();
            const ImVec4 s_Colour = s_It->kind == SdkWorld::ToastKind::Warning ? s_Palette.bad : s_It->kind == SdkWorld::ToastKind::Partner ? c_Gold :
                s_It->kind == SdkWorld::ToastKind::Chat ? c_Chat : c_White;
            ImGui::PushStyleColor(ImGuiCol_Text, s_Colour);
            ImGui::TextWrapped(s_It->count > 1 ? "%s  x%d" : "%s", s_It->text.c_str(), s_It->count);
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndChild();
}

void HitmanCoop::DrawSettingsTab() {
    bool s_Changed = false;
    const float s_Field = 200.f * m_WindowScale;

    if (ImGui::CollapsingHeader(ICON_MD_GROUPS " Playing together", ImGuiTreeNodeFlags_DefaultOpen)) {
        s_Changed |= ImGui::Checkbox("Follow the host's mission when I start one at the same location", &m_FollowHost);
        s_Changed |= ImGui::Checkbox("Pause together (my partner's pause menu pauses my game)", &m_PauseTogether);
        ImGui::BeginDisabled(!m_PauseTogether);
        s_Changed |= ImGui::Checkbox("...but not when they only look at their map, notebook or phone", &m_MapNoPause);
        ImGui::EndDisabled();
        s_Changed |= ImGui::Checkbox("Start together (hold my mission until my partner has loaded in, up to 90 s)", &m_WaitForPartner);
        s_Changed |= ImGui::Checkbox("Reconnect by myself when the connection drops", &m_AutoReconnect);
        s_Changed |= ImGui::Checkbox("Sounds for my partner's messages, marks and warnings", &m_Sounds);
    }

    if (ImGui::CollapsingHeader(ICON_MD_DASHBOARD " HUD and overlay")) {
        s_Changed |= ImGui::Checkbox("Show the co-op HUD", &m_ShowHud);
        static constexpr const char* c_Corners[] = { "Top right", "Top left", "Bottom right", "Bottom left" };
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::Combo("HUD position", &m_HudCorner, c_Corners, 4);
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderFloat("HUD size", &m_HudScale, 0.75f, 1.75f, "%.2fx");
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderFloat("HUD background", &m_HudOpacity, 0.f, 1.f, "%.2f");
        s_Changed |= ImGui::Checkbox("Show my partner's name over their head", &m_ShowNameTag);
        s_Changed |= ImGui::Checkbox("Show marks", &m_ShowMarks);
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderFloat("Name tag and mark size", &m_OverlayScale, 0.75f, 2.f, "%.2fx");
        s_Changed |= ImGui::Checkbox("Show the radar", &m_ShowRadar);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.f * m_WindowScale);
        static constexpr const char* c_Ranges[] = { "25 m", "50 m", "100 m" };
        s_Changed |= ImGui::Combo("##HitmanCoopRadarRange", &m_RadarRange, c_Ranges, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.f * m_WindowScale);
        s_Changed |= ImGui::SliderFloat("##HitmanCoopRadarSize", &m_RadarSize, 0.6f, 2.f, "size %.1fx");
        s_Changed |= ImGui::Checkbox("Show the compass strip (top of the screen)", &m_ShowCompass);
        s_Changed |= ImGui::Checkbox("Target assist: show the mission's targets on the radar", &m_TargetAssist);
        HelpMarker("An easier game: red rings on the radar where the targets are. Off by default.");
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::ColorEdit3("My colour", m_Colour, ImGuiColorEditFlags_NoInputs);
        HelpMarker("Your partner sees your name tag, marks and radar dot in this colour.");
        s_Changed |= ImGui::Checkbox("Colour-blind friendly colours (blue and orange)", &m_ColourBlind);
        s_Changed |= ImGui::Checkbox("Distances in feet", &m_Feet);
    }

    if (ImGui::CollapsingHeader(ICON_MD_PLACE " Marks")) {
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderInt("Marks I can have at once", &m_MaxMarks, 1, 3);
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderInt("Marks last (seconds)", &m_MarkSeconds, 10, 120);
        ImGui::TextDisabled("Mark with the mark key (Hotkeys tab). Twice quickly: danger. Again later: take it back.");
    }

    if (ImGui::CollapsingHeader(ICON_MD_NOTIFICATIONS " Notifications")) {
        s_Changed |= ImGui::Checkbox("Show notifications", &m_ShowToasts);
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderFloat("On screen for (seconds)", &m_ToastSeconds, 3.f, 20.f, "%.0f");
        s_Changed |= ImGui::Checkbox("My partner's kills and knock-outs", &m_ToastKillFeed);
        s_Changed |= ImGui::Checkbox("My partner's challenges", &m_ToastChallenges);
        s_Changed |= ImGui::Checkbox("My partner's alerts (spotted, hunted, ...)", &m_ToastAlerts);
        s_Changed |= ImGui::Checkbox("Chat messages", &m_ToastChat);
    }

    if (ImGui::CollapsingHeader(ICON_MD_CHAT " Quick messages")) {
        for (int i = 0; i < 8; ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(-FLT_MIN);
            s_Changed |= ImGui::InputTextWithHint("##HitmanCoopQuick", c_DefaultQuickMessages[i], m_QuickBuffers[i], sizeof(m_QuickBuffers[i]));
            ImGui::PopID();
        }

        if (ImGui::Button("Back to the standard messages")) {
            for (int i = 0; i < 8; ++i)
                strncpy_s(m_QuickBuffers[i], c_DefaultQuickMessages[i], _TRUNCATE);

            s_Changed = true;
        }
    }

    if (ImGui::CollapsingHeader(ICON_MD_LAN " Connection")) {
        ImGui::SetNextItemWidth(s_Field * 1.5f);
        s_Changed |= ImGui::InputText("Co-op server", m_RelayBuffer, sizeof(m_RelayBuffer), m_Streamer ? ImGuiInputTextFlags_Password : 0);
        HelpMarker("The relay server that hands out join codes (host[:port]). Empty: direct IP connections only.");
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::InputInt("Direct UDP port", &m_Port);
        m_Port = std::clamp(m_Port, 1024, 65535);
        static constexpr const char* c_Quality[] = { "Low (slow connections)", "Normal", "High (fast connections, LAN)" };
        ImGui::SetNextItemWidth(s_Field * 1.5f);
        s_Changed |= ImGui::Combo("NPC sync detail", &m_SyncQuality, c_Quality, 3);
        HelpMarker("How often NPCs, and how many NPCs' animations, go to your partner.");
        s_Changed |= ImGui::Checkbox("Streamer mode (hide join codes and IP addresses)", &m_Streamer);
        s_Changed |= ImGui::Checkbox("Copy my join code to the clipboard when it arrives", &m_AutoCopyCode);
    }

    if (ImGui::CollapsingHeader(ICON_MD_TUNE " This window and NPCs")) {
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderFloat("Window text size", &m_WindowScale, 0.8f, 1.6f, "%.2fx");
        s_Changed |= ImGui::Checkbox("Correct NPCs that drift from the host's", &m_CorrectNpcs);
        ImGui::SetNextItemWidth(s_Field);
        s_Changed |= ImGui::SliderFloat("Leave NPCs near me alone (metres)", &m_LocalBubble, 0.f, 60.f, "%.0f");
    }

    if (s_Changed) {
        ApplySettings();
        SaveSettings();
    }
}

void HitmanCoop::DrawHotkeysTab() {
    bool s_Changed = ImGui::Checkbox("Hotkeys on", &m_Hotkeys);
    ImGui::SameLine();
    ImGui::TextDisabled("(number pad keys need Num Lock on)");

    // Waiting for a key for one of them.
    if (m_Capturing >= 0) {
        Hotkeys::Bind s_Bind;
        bool s_Cancelled = false;

        if (Hotkeys::Capture(s_Bind, s_Cancelled)) {
            if (!s_Cancelled)
                Hotkeys::Set(static_cast<Hotkeys::Action>(m_Capturing), s_Bind);

            m_Capturing = -1;
            s_Changed = true;
        }
    }

    // The same key for two actions: both are shown in red.
    std::array<bool, Hotkeys::c_Count> s_Clash {};

    for (int i = 0; i < Hotkeys::c_Count; ++i)
        for (int j = i + 1; j < Hotkeys::c_Count; ++j)
            if (const auto s_A = Hotkeys::Get(static_cast<Hotkeys::Action>(i)); s_A.vk != 0 && s_A == Hotkeys::Get(static_cast<Hotkeys::Action>(j)))
                s_Clash[i] = s_Clash[j] = true;

    if (ImGui::BeginTable("##HitmanCoopKeys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
            ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing() * 2.f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Columns as wide as their words, at any text size; the action's name takes the rest.
        const ImGuiStyle& s_Style = ImGui::GetStyle();
        const float s_KeyWidth = ImGui::CalcTextSize("Press a key... (Esc: cancel)").x + s_Style.CellPadding.x * 2.f;
        const float s_ButtonsWidth = ImGui::CalcTextSize("ChangeClearDefault").x + s_Style.FramePadding.x * 6.f + s_Style.ItemSpacing.x * 2.f +
            s_Style.CellPadding.x * 2.f + 4.f;
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, s_KeyWidth);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, s_ButtonsWidth);
        ImGui::TableHeadersRow();
        const char* s_Group = "";

        for (int i = 0; i < Hotkeys::c_Count; ++i) {
            const auto s_Action = static_cast<Hotkeys::Action>(i);
            const auto& s_Info = Hotkeys::Describe(s_Action);

            if (std::strcmp(s_Group, s_Info.group) != 0) {
                s_Group = s_Info.group;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(c_Gold, "%s", s_Group);
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();

            // A quick message's row shows its words.
            if (s_Action >= Hotkeys::Action::Quick1 && s_Action <= Hotkeys::Action::Quick8) {
                const int s_Index = i - static_cast<int>(Hotkeys::Action::Quick1);
                ImGui::Text("\"%s\"", m_QuickBuffers[s_Index][0] ? m_QuickBuffers[s_Index] : c_DefaultQuickMessages[s_Index]);
            }
            else
                ImGui::TextUnformatted(s_Info.name);

            ImGui::TableNextColumn();
            const std::string s_Name = Hotkeys::Name(Hotkeys::Get(s_Action));

            if (m_Capturing == i)
                ImGui::TextColored(c_Gold, "Press a key... (Esc: cancel)");
            else if (s_Name.empty())
                ImGui::TextDisabled("none");
            else if (s_Clash[i])
                ImGui::TextColored(c_Red, "%s (twice!)", s_Name.c_str());
            else
                ImGui::TextUnformatted(s_Name.c_str());

            ImGui::TableNextColumn();
            ImGui::PushID(i);

            if (ImGui::SmallButton(m_Capturing == i ? "Waiting" : "Change")) {
                m_Capturing = i;
                Hotkeys::BeginCapture();
            }

            ImGui::SameLine();

            if (ImGui::SmallButton("Clear")) {
                Hotkeys::Set(s_Action, {});
                s_Changed = true;
            }

            ImGui::SameLine();

            if (ImGui::SmallButton("Default")) {
                Hotkeys::Bind s_Bind;
                Hotkeys::Parse(s_Info.fallback, s_Bind);
                Hotkeys::Set(s_Action, s_Bind);
                s_Changed = true;
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (ImGui::Button(ICON_MD_RESTART_ALT " Reset all to defaults")) {
        Hotkeys::ResetAll();
        m_Capturing = -1;
        s_Changed = true;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("Hold Ctrl, Shift or Alt while pressing to bind a combination.");

    if (s_Changed) {
        ApplySettings();
        SaveSettings();
    }
}

void HitmanCoop::DrawAboutTab(const coop::SessionStatus& p_Status) {
    ImGui::PushFont(SDK()->GetImGuiBlackFont(), ImGui::GetStyle().FontSizeBase * m_WindowScale * 1.3f);
    ImGui::TextColored(c_Red, "HITMAN CO-OP %s", c_ModVersion);
    ImGui::PopFont();
    ImGui::TextColored(c_Gold, ICON_MD_STAR " Created by %s", c_Author);
    ImGui::TextDisabled("Protocol %u, game %s", coop::kProtocolVersion, m_GameVersion.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("A fan-made two-player co-op mod for HITMAN World of Assassination. It isn't made, endorsed or supported by "
                       "IO Interactive. Use it at your own risk.");

    SectionTitle(ICON_MD_HELP " How to play");
    ImGui::TextWrapped("1. Both players start a mission at the same location (one you both own).");
    ImGui::TextWrapped("2. One player presses ~, opens CO-OP and clicks HOST. They get a join code.");
    ImGui::TextWrapped("3. The other types the code under \"Join a friend\" and clicks JOIN. They're taken into the host's mission.");
    ImGui::TextWrapped("4. Mark with the middle mouse button, chat in the Chat tab, use the number pad for quick messages. See Hotkeys.");

    SectionTitle(ICON_MD_SYSTEM_UPDATE " Updates");

    if (ImGui::Checkbox("Update automatically", &m_AutoUpdate))
        SaveSettings();

    HelpMarker("When the game starts, HITMAN Co-op checks its GitHub page for a new version and downloads it by itself. "
        "It starts the next time you start the game. Nothing else is sent anywhere.");
    {
        const Updater::Status s_Update = m_Updater.Get();
        const char* s_Text = s_Update.state == Updater::State::Checking ? "Checking for a new version..." :
            s_Update.state == Updater::State::UpToDate ? "You have the latest version." :
            s_Update.state == Updater::State::Staged ? "A new version is downloaded: it starts the next time you start the game." :
            s_Update.state == Updater::State::Failed ? "Couldn't check for a new version this time." :
            m_AutoUpdate ? "Checks when the game starts." : "Off: get new versions from the GitHub page.";
        ImGui::TextDisabled("%s%s%s", s_Text, s_Update.state == Updater::State::Staged ? " Version " : "",
            s_Update.state == Updater::State::Staged ? s_Update.version.c_str() : "");
    }

    {
        std::string s_Notes;

        {
            std::lock_guard s_Lock(m_StatusMutex);
            s_Notes = m_WhatsNew;
        }

        if (!s_Notes.empty()) {
            ImGui::Text("What's new in %s:", c_ModVersion);
            ImGui::TextWrapped("%s", s_Notes.c_str());
        }
    }

    SectionTitle(ICON_MD_BUILD " Support");

    if (ImGui::Button(ICON_MD_BUG_REPORT " Copy diagnostics"))
        ImGui::SetClipboardText(Diagnostics(p_Status).c_str());

    HelpMarker("Versions, connection details and recent notifications, to paste into a bug report. No IP addresses.");
    ImGui::TextDisabled("Log: mods\\HitmanCoop.log (the previous session's: HitmanCoop-previous.log).");
    ImGui::TextDisabled("Game patches: %s", CrashGuards::Report().c_str());
}

// Everything a bug report needs, for the clipboard (no addresses: it may be posted publicly).
std::string HitmanCoop::Diagnostics(const coop::SessionStatus& p_Status) {
    std::ostringstream s_Out;
    s_Out << "HITMAN Co-op " << c_ModVersion << " (protocol " << coop::kProtocolVersion << "), game " << m_GameVersion << "\n";
    s_Out << "Game patches: " << CrashGuards::Report() << "\n";
    s_Out << "Pause control: " << (GamePause::Available() ? "found" : "not found") << "\n";
    s_Out << "Session: " << coop::SessionStateName(p_Status.state) << (p_Status.role == coop::SessionRole::Host ? " (host)" :
        p_Status.role == coop::SessionRole::Client ? " (client)" : "") << ", synced " << (p_Status.synced ? "yes" : "no") << "\n";
    s_Out << "Partner: mod " << p_Status.peerModVersion << ", game " << p_Status.peerGameVersion << "\n";

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_Out << "Partner's 47: " << (m_StandInText.empty() ? "-" : m_StandInText) << "\n";
    }

    char s_Line[256];
    std::snprintf(s_Line, sizeof(s_Line), "Link: ping %.0f ms, jitter %.0f ms, loss %.1f%%, sending %.0f kbit/s, playback delay %.0f ms, resends %llu%s\n",
        p_Status.stats.rttMs, p_Status.stats.jitterMs, p_Status.stats.lossPercent, p_Status.sendKbps, p_Status.playbackDelayMs,
        static_cast<unsigned long long>(p_Status.stats.resends), p_Status.congested ? ", congested" : "");
    s_Out << s_Line;
    std::snprintf(s_Line, sizeof(s_Line), "NPCs: %zu tracked, %zu puppets, %zu run by the client; mod cost %.2f ms a frame%s\n", p_Status.npcsTracked,
        p_Status.npcsPuppeted, p_Status.npcsOwnedByClient, static_cast<double>(m_ModCostMs.load()), m_SlowMode ? " (slow frames: less NPC work)" : "");
    s_Out << s_Line;
    s_Out << "Recent notifications:\n";

    const auto s_History = m_World.CopyHistory();

    for (size_t i = s_History.size() > 20 ? s_History.size() - 20 : 0; i < s_History.size(); ++i)
        s_Out << "  " << s_History[i].text << "\n";

    return s_Out.str();
}

void HitmanCoop::DrawWindow(const coop::SessionStatus& p_Status, bool p_InMission, const std::optional<coop::RelayState>& p_Relay) {
    const ImGuiViewport* s_View = ImGui::GetMainViewport();
    const float s_Scale = m_WindowScale;

    // A good size the first time, never bigger than the screen, and always on it.
    ImGui::SetNextWindowSize(ImVec2(std::min(720.f * s_Scale, s_View->Size.x - 40.f), std::min(680.f * s_Scale, s_View->Size.y - 80.f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(s_View->Pos.x + s_View->Size.x * 0.5f, s_View->Pos.y + s_View->Size.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(520.f, s_View->Size.x - 20.f), std::min(420.f, s_View->Size.y - 20.f)),
        ImVec2(s_View->Size.x - 20.f, s_View->Size.y - 20.f));
    ImGui::PushFont(SDK()->GetImGuiRegularFont(), ImGui::GetStyle().FontSizeBase * s_Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));

    if (ImGui::Begin(ICON_MD_PEOPLE " HITMAN Co-op###HitmanCoopMain", &m_ShowWindow, ImGuiWindowFlags_NoCollapse)) {
        // Back on screen if it was moved (or saved) off it.
        ImVec2 s_Pos = ImGui::GetWindowPos();
        const ImVec2 s_Size = ImGui::GetWindowSize();
        const ImVec2 s_Clamped(std::clamp(s_Pos.x, s_View->Pos.x, std::max(s_View->Pos.x, s_View->Pos.x + s_View->Size.x - s_Size.x)),
            std::clamp(s_Pos.y, s_View->Pos.y, std::max(s_View->Pos.y, s_View->Pos.y + s_View->Size.y - s_Size.y)));

        if (s_Clamped.x != s_Pos.x || s_Clamped.y != s_Pos.y)
            ImGui::SetWindowPos(s_Clamped);

        DrawHeader(p_Status, p_InMission);
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##HitmanCoopTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            const auto Tab = [&](const char* p_Label, auto p_Draw) {
                if (ImGui::BeginTabItem(p_Label)) {
                    if (ImGui::BeginChild("##HitmanCoopTab", ImVec2(0.f, 0.f), ImGuiChildFlags_None))
                        p_Draw();

                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            };

            Tab(ICON_MD_SPORTS_ESPORTS " Play", [&] { DrawPlayTab(p_Status, p_InMission, p_Relay); });

            const size_t s_Unread = m_World.PartnerChatCount() - std::min(m_ChatSeen, m_World.PartnerChatCount());
            const std::string s_ChatLabel = std::string(ICON_MD_CHAT " Chat") + (s_Unread ? " (" + std::to_string(s_Unread) + ")" : "") + "###HitmanCoopChatTab";
            Tab(s_ChatLabel.c_str(), [&] { DrawChatTab(p_Status); });
            Tab(ICON_MD_ASSIGNMENT " Mission", [&] { DrawMissionTab(p_Status); });
            Tab(ICON_MD_SETTINGS " Settings", [&] { DrawSettingsTab(); });
            Tab(ICON_MD_KEYBOARD " Hotkeys", [&] { DrawHotkeysTab(); });
            Tab(ICON_MD_INFO " About", [&] { DrawAboutTab(p_Status); });
            ImGui::EndTabBar();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

void HitmanCoop::OnDrawUI(bool p_HasFocus) {
    if (!m_UiTraced) {
        m_UiTraced = true;
        m_Test.Trace("first UI frame");
    }

    m_ViewWidth = ImGui::GetMainViewport()->Size.x;
    m_ViewHeight = ImGui::GetMainViewport()->Size.y;
    m_WantText = ImGui::GetIO().WantTextInput;
    m_OverlayOpen = p_HasFocus;

    // The game thread's asks: display toggles from hotkeys, and a save of the co-op record.
    if (m_ToggleHudAsked.exchange(false)) {
        m_ShowHud = !m_ShowHud;
        SaveSettings();
    }

    if (m_ToggleRadarAsked.exchange(false)) {
        m_ShowRadar = !m_ShowRadar;
        SaveSettings();
    }

    if (m_SaveRequested.exchange(false))
        SaveSettings();

    // A key being bound stops waiting once the window is closed.
    if (!p_HasFocus || !m_ShowWindow)
        m_Capturing = -1;

    coop::SessionStatus s_Status;
    bool s_InMission;
    std::optional<coop::RelayState> s_Relay;
    SdkWorld::Overlay s_Overlay;
    bool s_Reconnecting;

    {
        std::lock_guard s_Lock(m_StatusMutex);
        s_Status = m_Status;
        s_InMission = m_InMission;
        s_Relay = m_RelayStatus;
        s_Overlay = m_Overlay;
        s_Reconnecting = m_OverlayReconnecting;
    }

    DrawOverlay(s_Overlay, s_Status);
    DrawCompass(s_Overlay, s_Status);
    DrawHud(s_Status, s_InMission, s_Overlay, s_Reconnecting);
    DrawRadar(s_Overlay, s_Status, m_HudBottom);
    DrawCountdown();
    DrawSummary();
    DrawWhatsNew();
    DrawToasts();

    // The ping, twice a second, for the graph in Connection details.
    if (const double s_Now = Now(); s_Status.state == coop::SessionState::Connected && s_Now - m_PingSampleAt > 0.5) {
        m_PingSampleAt = s_Now;
        m_PingHistory.push_back(static_cast<float>(s_Status.stats.rttMs));

        if (m_PingHistory.size() > 120)
            m_PingHistory.pop_front();
    }

    if (p_HasFocus && m_ShowWindow)
        DrawWindow(s_Status, s_InMission, s_Relay);
}
