/**
 * @file fsvatsimgui.cpp
 * @brief Dear ImGui host inside the viewer for the VATs editor UI (VATs spec 09 §0a, stage U2).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 Viewport Avatar Toolset contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsvatsimgui.h"
#include "fsvatshost.h"

#include "llclipboard.h"
#include "lldir.h"
#include "llfloater.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llmodaldialog.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"  // the hovered window, for routing presses

#include <cctype>
#include <cfloat>
#include <map>

namespace
{
    ImGuiContext* sCtx = nullptr;
    bool sGLReady = false;              // ImGui_ImplOpenGL3_Init done for the current GL context
    struct Client
    {
        std::function<void()> draw, before;
    };
    std::map<std::string, Client> sClients;
    ImVec2 sOrigin;                     // the world view's top-left in the window, scaled UI units
    U32 sWorldHoverFrame = 0;           // the last frame the viewer's hover reached the world (worldHover)
    bool sBuilt = false;                // this frame's ImGui frame is built (buildFrame)
    bool sWorldDrawn = false;           // and its world layer drawn (renderWorld)
    bool sPanelsDrawn = false;          // and its panels drawn (under LLUI while a viewer popup is open)
    ImDrawList* sWorldList = nullptr;   // the world layer: ImGui's background draw list
    std::string sIniPath;
    std::string sClipboard;             // backing store for ImGui's GetClipboardText
    LLTimer sFrameTimer;
    U32 sCapturedButtons = 0;           // buttons whose press ImGui consumed, so their release goes to ImGui too
    bool sHadTextInput = false;
    int sGLChecksLeft = 0;              // frames left to verify GL state is restored around ImGui's draw

    ImVec2 toImGui(LLCoordGL pos)
    {
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        return ImVec2((F32)pos.mX / scale.mV[VX] - sOrigin.x,
                      (F32)(gViewerWindow->getWindowHeightRaw() - pos.mY) / scale.mV[VY] - sOrigin.y);
    }

    void setMods(ImGuiIO& io, MASK mask)
    {
        io.AddKeyEvent(ImGuiMod_Ctrl, (mask & MASK_CONTROL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (mask & MASK_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (mask & MASK_ALT) != 0);
    }

    ImGuiKey toImGuiKey(KEY key)
    {
        if (key >= 'A' && key <= 'Z') return (ImGuiKey)(ImGuiKey_A + (key - 'A'));
        if (key >= '0' && key <= '9') return (ImGuiKey)(ImGuiKey_0 + (key - '0'));
        if (key >= KEY_F1 && key <= KEY_F12) return (ImGuiKey)(ImGuiKey_F1 + (key - KEY_F1));
        switch (key)
        {
            case ' ': return ImGuiKey_Space;
            case '\'': return ImGuiKey_Apostrophe;
            case ',': return ImGuiKey_Comma;
            case '-': case KEY_HYPHEN: return ImGuiKey_Minus;
            case '.': return ImGuiKey_Period;
            case '/': return ImGuiKey_Slash;
            case ';': return ImGuiKey_Semicolon;
            case '=': case KEY_EQUALS: return ImGuiKey_Equal;
            case '[': return ImGuiKey_LeftBracket;
            case '\\': return ImGuiKey_Backslash;
            case ']': return ImGuiKey_RightBracket;
            case '`': return ImGuiKey_GraveAccent;
            case KEY_RETURN: return ImGuiKey_Enter;
            case KEY_PAD_RETURN: return ImGuiKey_KeypadEnter;
            case KEY_LEFT: case KEY_PAD_LEFT: return ImGuiKey_LeftArrow;
            case KEY_RIGHT: case KEY_PAD_RIGHT: return ImGuiKey_RightArrow;
            case KEY_UP: case KEY_PAD_UP: return ImGuiKey_UpArrow;
            case KEY_DOWN: case KEY_PAD_DOWN: return ImGuiKey_DownArrow;
            case KEY_ESCAPE: return ImGuiKey_Escape;
            case KEY_BACKSPACE: return ImGuiKey_Backspace;
            case KEY_DELETE: case KEY_PAD_DEL: return ImGuiKey_Delete;
            case KEY_INSERT: case KEY_PAD_INS: return ImGuiKey_Insert;
            case KEY_HOME: case KEY_PAD_HOME: return ImGuiKey_Home;
            case KEY_END: case KEY_PAD_END: return ImGuiKey_End;
            case KEY_PAGE_UP: case KEY_PAD_PGUP: return ImGuiKey_PageUp;
            case KEY_PAGE_DOWN: case KEY_PAD_PGDN: return ImGuiKey_PageDown;
            case KEY_TAB: return ImGuiKey_Tab;
            case KEY_SHIFT: return ImGuiKey_LeftShift;
            case KEY_CONTROL: return ImGuiKey_LeftCtrl;
            case KEY_ALT: return ImGuiKey_LeftAlt;
            case KEY_CAPSLOCK: return ImGuiKey_CapsLock;
            case KEY_ADD: case KEY_PAD_ADD: return ImGuiKey_KeypadAdd;
            case KEY_SUBTRACT: case KEY_PAD_SUBTRACT: return ImGuiKey_KeypadSubtract;
            case KEY_MULTIPLY: case KEY_PAD_MULTIPLY: return ImGuiKey_KeypadMultiply;
            case KEY_DIVIDE: case KEY_PAD_DIVIDE: return ImGuiKey_KeypadDivide;
            default: return ImGuiKey_None;
        }
    }

    ECursorType toCursor(ImGuiMouseCursor c)
    {
        switch (c)
        {
            case ImGuiMouseCursor_TextInput: return UI_CURSOR_IBEAM;
            case ImGuiMouseCursor_ResizeAll: return UI_CURSOR_SIZEALL;
            case ImGuiMouseCursor_ResizeNS: return UI_CURSOR_SIZENS;
            case ImGuiMouseCursor_ResizeEW: return UI_CURSOR_SIZEWE;
            case ImGuiMouseCursor_ResizeNESW: return UI_CURSOR_SIZENESW;
            case ImGuiMouseCursor_ResizeNWSE: return UI_CURSOR_SIZENWSE;
            case ImGuiMouseCursor_Hand: return UI_CURSOR_HAND;
            case ImGuiMouseCursor_NotAllowed: return UI_CURSOR_NO;
            default: return UI_CURSOR_ARROW;
        }
    }

    // The GL state LLRender and LLGLState cache or assume, read back so a draw can be checked against it.
    struct GLSnapshot
    {
        GLint v[26] = {};
        void read()
        {
            GLint* p = v;
            glGetIntegerv(GL_CURRENT_PROGRAM, p++);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, p++);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, p++);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, p++);
            glGetIntegerv(GL_ACTIVE_TEXTURE, p++);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, p++);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, p++);
            glGetIntegerv(GL_BLEND_SRC_RGB, p++);
            glGetIntegerv(GL_BLEND_DST_RGB, p++);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, p++);
            glGetIntegerv(GL_BLEND_DST_ALPHA, p++);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, p++);
            glGetIntegerv(GL_VIEWPORT, p); p += 4;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, p++);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, p++);
            *p++ = glIsEnabled(GL_BLEND);
            *p++ = glIsEnabled(GL_CULL_FACE);
            *p++ = glIsEnabled(GL_DEPTH_TEST);
            *p++ = glIsEnabled(GL_STENCIL_TEST);
            *p++ = glIsEnabled(GL_SCISSOR_TEST);
            GLboolean depth_mask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
            *p++ = depth_mask;
        }
    };

    // Verifies ImGui's backend put back all the GL state above, and that LLRender's caches (bound
    // shader, active texture unit, texture on that unit) still match GL. Runs on the first frames only.
    void checkGLState(const GLSnapshot& before)
    {
        static const char* names[] = { "program", "vao", "array buffer", "element buffer", "active texture",
            "texture 2D", "draw framebuffer", "blend src rgb", "blend dst rgb", "blend src alpha", "blend dst alpha",
            "blend equation", "viewport x", "viewport y", "viewport w", "viewport h", "unpack alignment",
            "unpack row length", "blend", "cull face", "depth test", "stencil test", "scissor test", "depth mask" };
        GLSnapshot after;
        after.read();
        std::string bad;
        for (size_t i = 0; i < LL_ARRAY_SIZE(names); ++i)
        {
            if (before.v[i] != after.v[i])
            {
                bad += llformat(" %s %d->%d", names[i], before.v[i], after.v[i]);
            }
        }
        if ((GLuint)after.v[0] != LLGLSLShader::sCurBoundShader)
        {
            bad += llformat(" LLGLSLShader cache %u vs GL %d", LLGLSLShader::sCurBoundShader, after.v[0]);
        }
        S32 unit = gGL.getCurrentTexUnitIndex();
        if (after.v[4] != (GLint)(GL_TEXTURE0 + unit))
        {
            bad += llformat(" LLRender texture unit %d vs GL %d", unit, after.v[4] - GL_TEXTURE0);
        }
        else if (gGL.getTexUnit(unit)->getCurrType() == LLTexUnit::TT_TEXTURE
                 && (GLuint)after.v[5] != gGL.getTexUnit(unit)->getCurrTexture())
        {
            bad += llformat(" LLTexUnit %d cache %u vs GL %d", unit, gGL.getTexUnit(unit)->getCurrTexture(), after.v[5]);
        }
        if (bad.empty())
        {
            LL_INFOS("VATsImGui") << "GL state after ImGui draw matches the state before and LLRender's caches" << LL_ENDL;
        }
        else
        {
            LL_WARNS("VATsImGui") << "GL state changed by ImGui draw:" << bad << LL_ENDL;
        }
    }

    const char* getClipboard(ImGuiContext*)
    {
        LLWString text;
        LLClipboard::instance().pasteFromClipboard(text);
        sClipboard = wstring_to_utf8str(text);
        return sClipboard.c_str();
    }

    void setClipboard(ImGuiContext*, const char* text)
    {
        LLWString wtext = utf8str_to_wstring(text);
        LLClipboard::instance().copyToClipboard(wtext, 0, static_cast<S32>(wtext.size()));
    }

    void create()
    {
        IMGUI_CHECKVERSION();
        sCtx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.BackendPlatformName = "soapstorm_viewer";
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        sIniPath = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "vats_imgui.ini");
        io.IniFilename = sIniPath.c_str();
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        pio.Platform_GetClipboardTextFn = getClipboard;
        pio.Platform_SetClipboardTextFn = setClipboard;
        ImGui::StyleColorsDark();
        sFrameTimer.reset();
        sCapturedButtons = 0;
        sHadTextInput = false;
        sGLChecksLeft = 3;
        LL_INFOS("VATsImGui") << "ImGui " << IMGUI_VERSION << " context created, ini " << sIniPath << LL_ENDL;
    }

    void destroy()
    {
        ImGui::SetCurrentContext(sCtx);
        FSVATsImGui::destroyGL();
        ImGui::DestroyContext(sCtx);    // also writes the ini
        sCtx = nullptr;
        LL_INFOS("VATsImGui") << "ImGui context destroyed" << LL_ENDL;
    }

    // The debug test: the ImGui demo plus a window with its own dockspace and a few input checks.
    void drawTest()
    {
        static bool show_demo = true;
        static bool dock_over_viewport = false;
        static char text[256] = "";
        static float slider = 0.5f;
        if (dock_over_viewport)
        {
            ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
        }
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(420.f, 360.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(40.f, 60.f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("VATs UI test", &open))
        {
            ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("Dear ImGui %s in SoapStorm", IMGUI_VERSION);
            ImGui::Text("DisplaySize %.0f x %.0f, FramebufferScale %.2f x %.2f",
                        io.DisplaySize.x, io.DisplaySize.y, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            ImGui::Text("%.1f fps, mouse %.0f,%.0f", io.Framerate, io.MousePos.x, io.MousePos.y);
            ImGui::Text("WantCaptureMouse %d  WantCaptureKeyboard %d  WantTextInput %d",
                        io.WantCaptureMouse, io.WantCaptureKeyboard, io.WantTextInput);
            ImGui::InputText("Text", text, sizeof(text));
            ImGui::SliderFloat("Slider", &slider, 0.f, 1.f);
            ImGui::Checkbox("Demo window", &show_demo);
            ImGui::Checkbox("Dockspace over the viewer (passthrough centre)", &dock_over_viewport);
            ImGui::Separator();
            ImGui::TextUnformatted("Drag windows onto the dock area below:");
            ImGui::DockSpace(ImGui::GetID("VATsTestDock"), ImVec2(0.f, 0.f));
        }
        ImGui::End();
        if (show_demo)
        {
            ImGui::ShowDemoWindow(&show_demo);
        }
        if (!open)
        {
            gSavedSettings.setBOOL("VATsImGuiTest", false);
        }
    }
}

namespace
{
    // The viewer's camera keys with Alt (key_bindings.xml, every mode): the arrows, Page Up/Down, and A, D, W, S,
    // E, C. Other Alt keys (the editor's Alt+R...) stay the editor's.
    bool isAltCameraKey(KEY key, MASK mask)
    {
        if (!(mask & MASK_ALT))
        {
            return false;
        }
        switch (key)
        {
            case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN: case KEY_PAGE_UP: case KEY_PAGE_DOWN:
            case KEY_PAD_LEFT: case KEY_PAD_RIGHT: case KEY_PAD_UP: case KEY_PAD_DOWN: case KEY_PAD_PGUP: case KEY_PAD_PGDN:
            case 'A': case 'D': case 'W': case 'S': case 'E': case 'C':
                return true;
            default:
                return false;
        }
    }

    // Keys (see the header): an ImGui text field first; then LLUI's keyboard focus (the chat bar, a floater);
    // Alt camera keys are the viewer's; the rest go to ImGui while a VATs window holds the keyboard, and all
    // of them while the editor is open.
    bool imguiGetsKeys(KEY key, MASK mask)
    {
        if (!sCtx)
        {
            return false;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput)
        {
            return true;
        }
        if (gFocusMgr.getKeyboardFocus() || isAltCameraKey(key, mask))
        {
            return false;
        }
        return io.WantCaptureKeyboard || FSVATsEditor::ownsWorld();
    }

    // A viewer menu or modal dialog is open: it goes over the editor's panels and gets the clicks.
    bool viewerPopupOpen()
    {
        return (gMenuHolder && gMenuHolder->hasVisibleMenu()) || LLModalDialog::activeCount() > 0;
    }

    // A press ImGui's own windows take: over a window, or anywhere while a popup is open (clicking away closes
    // it). Presses over the world come through worldClick instead, after LLUI had its chance.
    bool panelsTakePress()
    {
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        return !viewerPopupOpen() && (g.HoveredWindow != nullptr || g.OpenPopupStack.Size > 0);
    }

    // Draws one layer of the frame built this frame: the world layer (the background list: bones, gizmos,
    // markers) or the rest (the panels).
    void drawLayer(bool world)
    {
        const ImDrawData* all = ImGui::GetDrawData();
        if (!all || !all->Valid)
        {
            return;
        }
        ImDrawData part = *all;
        part.CmdLists.resize(0);
        part.TotalVtxCount = part.TotalIdxCount = 0;
        for (ImDrawList* list : all->CmdLists)
        {
            if ((list == sWorldList) == world)
            {
                part.CmdLists.push_back(list);
                part.TotalVtxCount += list->VtxBuffer.Size;
                part.TotalIdxCount += list->IdxBuffer.Size;
            }
        }
        part.CmdListsCount = part.CmdLists.Size;
        if (!part.CmdListsCount)
        {
            return;
        }
        // LLRender batches immediate-mode vertices: flush them now or they land on top of ImGui.
        gGL.flush();
        GLSnapshot before;
        if (sGLChecksLeft > 0)
        {
            before.read();
        }
        ImGui_ImplOpenGL3_RenderDrawData(&part);
        if (sGLChecksLeft > 0)
        {
            --sGLChecksLeft;
            checkGLState(before);
        }
    }
}

void FSVATsImGui::setClient(const std::string& name, std::function<void()> draw, std::function<void()> before)
{
    sClients[name] = { std::move(draw), std::move(before) };
}

void FSVATsImGui::removeClient(const std::string& name)
{
    sClients.erase(name);
}

bool FSVATsImGui::isOpen()
{
    return sCtx != nullptr;
}

void FSVATsImGui::destroyGL()
{
    if (sCtx && sGLReady)
    {
        ImGui::SetCurrentContext(sCtx);
        ImGui_ImplOpenGL3_Shutdown();
        sGLReady = false;
    }
}

// Builds this frame's ImGui frame (clients, input, layout); false when nothing is open.
static bool buildFrame()
{
    if (!gViewerWindow)
    {
        return false;
    }
    static LLCachedControl<bool> test(gSavedSettings, "VATsImGuiTest", false);
    static bool test_on = false;
    if (test != test_on)
    {
        test_on = test;
        if (test_on)
        {
            FSVATsImGui::setClient("test", drawTest);
        }
        else
        {
            FSVATsImGui::removeClient("test");
        }
    }
    static LLCachedControl<bool> editor(gSavedSettings, "VATsEditor", false);
    FSVATsEditor::update(editor);
    if (sClients.empty())
    {
        if (sCtx)
        {
            destroy();
        }
        return false;
    }
    if (!sCtx)
    {
        create();
    }
    ImGui::SetCurrentContext(sCtx);
    if (!sGLReady)
    {
        // GLSL 1.50 suits every context the viewer runs on (GL 3.2+ core or compatibility, macOS core).
        sGLReady = ImGui_ImplOpenGL3_Init("#version 150");
        if (!sGLReady)
        {
            LL_WARNS("VATsImGui") << "ImGui OpenGL3 backend failed to initialise" << LL_ENDL;
            return false;
        }
    }

    for (auto& client : sClients)
    {
        if (client.second.before)
        {
            client.second.before();
        }
    }
    // ImGui covers the world view only, so the viewer's menu, navigation bar and toolbars stay usable and
    // markers projected with LLViewerCamera line up (the camera projects onto this rectangle).
    ImGuiIO& io = ImGui::GetIO();
    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    const F32 window_h = (F32)gViewerWindow->getWindowHeightScaled();
    sOrigin = ImVec2((F32)world.mLeft, window_h - (F32)world.mTop);
    io.DisplaySize = ImVec2((F32)llmax(world.getWidth(), 1), (F32)llmax(world.getHeight(), 1));
    const LLVector2& scale = gViewerWindow->getDisplayScale();
    io.DisplayFramebufferScale = ImVec2(scale.mV[VX], scale.mV[VY]);
    io.DeltaTime = llmax((F32)sFrameTimer.getElapsedTimeAndResetF32(), 1e-4f);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    for (auto& client : sClients)
    {
        client.second.draw();
    }
    sWorldList = ImGui::GetBackgroundDrawList();
    ImGui::Render();
    // Drawn into the whole window, offset to the world view: the backend's viewport and scissor are
    // window-sized, its projection starts at DisplayPos.
    ImDrawData* draw_data = ImGui::GetDrawData();
    draw_data->DisplayPos = ImVec2(-sOrigin.x, -sOrigin.y);
    draw_data->DisplaySize = ImVec2((F32)gViewerWindow->getWindowWidthScaled(), window_h);
    sBuilt = true;
    sWorldDrawn = sPanelsDrawn = false;
    return true;
}

void FSVATsImGui::renderWorld()
{
    if (!sBuilt && !buildFrame())
    {
        return;
    }
    drawLayer(true);
    sWorldDrawn = true;
    if (viewerPopupOpen())
    {
        drawLayer(false);  // the viewer's menu or modal dialog goes over the panels
        sPanelsDrawn = true;
    }
}

void FSVATsImGui::render()
{
    if (!sBuilt && !buildFrame())
    {
        return;
    }
    if (!sWorldDrawn)
    {
        drawLayer(true);  // no world pass this frame (the login screen)
    }
    if (!sPanelsDrawn)
    {
        drawLayer(false);
    }
    sBuilt = sWorldDrawn = sPanelsDrawn = false;

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse)
    {
        gViewerWindow->setCursor(toCursor(ImGui::GetMouseCursor()));
    }
    // One text caret at a time: an ImGui text field taking the keyboard takes it from LLUI.
    if (io.WantTextInput && !sHadTextInput)
    {
        gFocusMgr.setKeyboardFocus(nullptr);
    }
    sHadTextInput = io.WantTextInput;
}

namespace
{
    int toImGuiButton(EMouseClickType click)
    {
        switch (click)
        {
            case CLICK_LEFT: case CLICK_DOUBLELEFT: return ImGuiMouseButton_Left;
            case CLICK_RIGHT: return ImGuiMouseButton_Right;
            case CLICK_MIDDLE: return ImGuiMouseButton_Middle;
            case CLICK_BUTTON4: return 3;
            case CLICK_BUTTON5: return 4;
            default: return -1;
        }
    }

    // ImGui takes this press: it gets the button, its release will come back to it, and LLUI loses the keys.
    void takePress(LLCoordGL pos, MASK mask, int button)
    {
        ImGuiIO& io = ImGui::GetIO();
        setMods(io, mask);
        const ImVec2 p = toImGui(pos);
        io.AddMousePosEvent(p.x, p.y);
        io.AddMouseButtonEvent(button, true);
        sCapturedButtons |= 1u << button;
        gFocusMgr.setKeyboardFocus(nullptr);
    }
}

bool FSVATsImGui::mouseButton(LLCoordGL pos, MASK mask, EMouseClickType click, bool down)
{
    const int button = sCtx ? toImGuiButton(click) : -1;
    if (button < 0)
    {
        return false;
    }
    if (down)
    {
        if (panelsTakePress())
        {
            takePress(pos, mask, button);
            return true;
        }
        // Not ImGui's: LLUI or the world (worldClick) has it. A VATs window stops holding the keyboard.
        ImGui::FocusWindow(nullptr);
        return false;
    }
    // Releases always reach ImGui, so it is never left with a button held; the viewer loses only those
    // whose press ImGui took.
    ImGuiIO& io = ImGui::GetIO();
    setMods(io, mask);
    const ImVec2 p = toImGui(pos);
    io.AddMousePosEvent(p.x, p.y);
    io.AddMouseButtonEvent(button, false);
    const U32 bit = 1u << button;
    const bool captured = (sCapturedButtons & bit) != 0;
    sCapturedButtons &= ~bit;
    if (captured)
    {
        gViewerWindow->getWindow()->releaseMouse();  // a world press was captured by handleAnyMouseClick
    }
    return captured;
}

bool FSVATsImGui::worldClick(LLCoordGL pos, MASK mask, EMouseClickType click, bool down)
{
    // Alt presses are the viewer's camera (Alt-cam, Ctrl+Alt orbit, Ctrl+Alt+Shift pan).
    const int button = sCtx && down && !(mask & MASK_ALT) && FSVATsEditor::ownsWorld() ? toImGuiButton(click) : -1;
    if (button < 0)
    {
        return false;
    }
    takePress(pos, mask, button);
    return true;
}

bool FSVATsImGui::worldHover(MASK mask)
{
    sWorldHoverFrame = LLFrameTimer::getFrameCount();
    if (!sCtx || (mask & MASK_ALT) || !FSVATsEditor::ownsWorld())
    {
        return false;
    }
    gViewerWindow->setCursor(UI_CURSOR_ARROW);  // no hand over touchable objects; ImGui sets its own at render
    return true;
}

void FSVATsImGui::mouseMove(LLCoordGL pos, MASK mask)
{
    if (sCtx)
    {
        ImVec2 p = toImGui(pos);
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
    }
}

void FSVATsImGui::mouseLeave()
{
    if (sCtx)
    {
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
}

bool FSVATsImGui::scrollWheel(S32 clicks, bool horizontal)
{
    if (!sCtx || !ImGui::GetIO().WantCaptureMouse)
    {
        return false;
    }
    // The viewer counts wheel-down (toward the user) as positive; ImGui counts wheel-up.
    ImGui::GetIO().AddMouseWheelEvent(horizontal ? (F32)-clicks : 0.f, horizontal ? 0.f : (F32)-clicks);
    return true;
}

bool FSVATsImGui::keyDown(KEY key, MASK mask)
{
    if (!imguiGetsKeys(key, mask))
    {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    setMods(io, mask);
    ImGuiKey k = toImGuiKey(key);
    if (k != ImGuiKey_None)
    {
        io.AddKeyEvent(k, true);
    }
    return true;
}

void FSVATsImGui::keyUp(KEY key, MASK mask)
{
    // Releases always reach both sides, so neither is left with a key held down.
    if (sCtx)
    {
        ImGuiIO& io = ImGui::GetIO();
        setMods(io, mask);
        ImGuiKey k = toImGuiKey(key);
        if (k != ImGuiKey_None)
        {
            io.AddKeyEvent(k, false);
        }
    }
}

bool FSVATsImGui::unicodeChar(llwchar uni_char, MASK mask)
{
    if (!imguiGetsKeys(uni_char < 128 ? (KEY)toupper((int)uni_char) : 0, mask))
    {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    // Control characters (Return, Backspace, Ctrl+letter) arrive as key events instead.
    if (io.WantTextInput && uni_char >= 32 && uni_char != 127)
    {
        io.AddInputCharacter((unsigned int)uni_char);
    }
    return true;
}

void FSVATsImGui::focusLost()
{
    if (sCtx)
    {
        ImGui::GetIO().AddFocusEvent(false);
        sCapturedButtons = 0;
    }
}

bool FSVATsImGui::capturesMouse()
{
    // Not ImGui's world capture (a bone under the pointer): LLUI windows over the world keep their hover.
    return sCtx && (panelsTakePress() || sCapturedButtons != 0);
}

bool FSVATsImGui::pointerOnWorld()
{
    return sWorldHoverFrame == LLFrameTimer::getFrameCount();
}
