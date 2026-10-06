/**
 * @file NetplaySceneGen.cpp
 * @brief Tools > Recomp > UI > Generate Network Scene (see NetplaySceneGen.h).
 */
#include "NetplaySceneGen.h"

#if EDITOR

#include "NetplayMenu.h"

// com.recomp.mod.base: the non-destructive UI builder, the menu controller, the game's menu style
#include "ModBaseModMap.h"
#include "ModBaseSettings.h"
#include "ModBaseUiBuilder.h"
#include "ModBaseWidgets.h"

#include "AssetDir.h"
#include "AssetManager.h"
#include "Assets/Scene.h"
#include "Editor/EditorUtils.h"
#include "Input/InputTypes.h"
#include "Nodes/Widgets/InputField.h"
#include "Plugins/EditorUIHooks.h"

#include "imgui.h"

#include <string>

using namespace RecompUi;

namespace
{
const char* const kWindowId = "com.recomp.netplay.scenegen";
const float kRowH = 32.0f;
const float kGap = 6.0f;
const float kPad = 16.0f;
EditorUIHooks* sHooks = nullptr;

struct Options
{
    std::string sceneName = "SC_Netplay";
    int toggleButton = GAMEPAD_THUMBL; // -1: none (HOME menu / scripts only); Select opens the mod settings
    bool startVisible = false;         // a lobby screen the game opens with
    bool inHomeMenu = true;
};

NetplayButton* Btn(Builder& b, Node* parent, const char* name, const char* label, const char* action,
                   const Placer& place)
{
    return b.Ensure<NetplayButton>(parent, name, [&](NetplayButton* btn) {
        place(btn);
        btn->SetTextString(label);
        btn->SetLabelFormat("");
        btn->SetAction(action);
    });
}

InputField* Field(Builder& b, Node* parent, const char* name, const char* text, const char* placeholder,
                  const Placer& place)
{
    return b.Ensure<InputField>(parent, name, [&](InputField* f) {
        place(f);
        f->SetText(text);
        f->SetPlaceholder(placeholder);
    });
}

AssetDir* ScenesDir(std::string& error)
{
    AssetManager* am = AssetManager::Get();
    AssetDir* project = am != nullptr ? am->FindProjectDirectory() : nullptr;
    if (project == nullptr)
    {
        error = "No project is open.";
        return nullptr;
    }
    AssetDir* scenes = project->GetSubdirectory("Scenes");
    if (scenes == nullptr)
    {
        scenes = project->CreateSubdirectory("Scenes");
    }
    if (scenes == nullptr)
    {
        error = "Cannot create the Scenes folder in " + project->mPath;
    }
    return scenes;
}

bool Generate(const Options& options, std::string& message)
{
    const std::string sceneName = options.sceneName.empty() ? std::string("SC_Netplay") : options.sceneName;

    // the scene of that name, opened for updating, or a new root
    AssetStub* stub = FetchAssetStub(sceneName);
    if (stub != nullptr && stub->mType != Scene::GetStaticType())
    {
        message = "An asset named " + sceneName + " exists and is not a Scene.";
        return false;
    }
    Scene* scene = nullptr;
    NodePtr root;
    if (stub != nullptr)
    {
        scene = LoadAsset<Scene>(sceneName);
        if (scene != nullptr)
        {
            root = scene->Instantiate();
        }
    }
    const bool updating = root.Get() != nullptr;
    if (!updating)
    {
        SharedPtr<Canvas> canvas = Node::Construct<Canvas>();
        canvas->SetName("Netplay");
        canvas->SetFullScreen();
        root = PtrStaticCast<Node>(canvas);
    }
    if (root->As<Widget>() == nullptr)
    {
        message = sceneName + "'s root is not a widget: left as it is.";
        return false;
    }

    Builder b;
    Quad* panel = b.Ensure<Quad>(root.Get(), "Panel", [](Quad* q) {
        Full(q);
        q->SetColor(kPanelColor);
    });
    if (panel == nullptr)
    {
        message = "Panel exists but is not a Quad: left as it is.";
        return false;
    }
    ScrollContainer* scroll = Scroll(b, panel, "Scroll", false, Filled());
    // the column's height: its rows (a scrolled list is as tall as its content)
    const float heights[] = {34, 44, 84, 20, kRowH, kRowH, kRowH, kRowH, 4 * kRowH + 3 * kGap, kRowH};
    float total = 2 * kPad;
    for (float h : heights)
    {
        total += h + kGap;
    }
    Widget* layout = Array(b, scroll != nullptr ? (Node*)scroll : (Node*)panel, "Layout", false, kGap, kPad,
                           FullWidth(total));
    if (layout == nullptr)
    {
        message = "Layout exists but is not an ArrayWidget (or the engine has none): left as it is.";
        return false;
    }

    if (Text* title = Label(b, layout, "Title", "Netplay", FullWidth(34.0f), 24.0f, kHeaderColor))
    {
        title->SetHorizontalJustification(Justification::Center);
    }
    Label(b, layout, "Status", "Not in a session", FullWidth(44.0f), 14.0f, kDimColor);
    Label(b, layout, "Players", "", FullWidth(84.0f), 14.0f, kTextColor);
    Label(b, layout, "Note", "", FullWidth(20.0f), 13.0f, kHeaderColor);

    Widget* hostRow = Array(b, layout, "HostRow", true, kGap, 0.0f, FullWidth(kRowH));
    NetplayButton* host = Btn(b, hostRow, "Host", "Host a session", "host", FullWidth(kRowH));
    Field(b, hostRow, "Port", "", "27464", At(0, 0, 110, kRowH));

    Widget* joinRow = Array(b, layout, "JoinRow", true, kGap, 0.0f, FullWidth(kRowH));
    Field(b, joinRow, "Address", "", "Host address (ip or ip:port)", FullWidth(kRowH));
    NetplayButton* join = Btn(b, joinRow, "Join", "Join", "join", At(0, 0, 110, kRowH));

    Widget* delayRow = Array(b, layout, "DelayRow", true, kGap, 0.0f, FullWidth(kRowH));
    Label(b, delayRow, "DelayLabel", "Input delay (frames)", FullWidth(kRowH), kFontSize, kTextColor);
    NetplayButton* delayDown = Btn(b, delayRow, "DelayDown", "-", "delay-", At(0, 0, 44, kRowH));
    if (Text* value = Label(b, delayRow, "DelayValue", "2", At(0, 0, 44, kRowH), kFontSize, kHeaderColor))
    {
        value->SetHorizontalJustification(Justification::Center);
    }
    NetplayButton* delayUp = Btn(b, delayRow, "DelayUp", "+", "delay+", At(0, 0, 44, kRowH));

    NetplayButton* search = Btn(b, layout, "Search", "Search the LAN", "search", FullWidth(kRowH));
    Widget* found = Array(b, layout, "Found", false, kGap, 0.0f, FullWidth(4 * kRowH + 3 * kGap));
    NetplayButton* slots[NetplayMenu::kFoundSlots] = {};
    for (int i = 0; i < NetplayMenu::kFoundSlots; i++)
    {
        const std::string name = "Found_" + std::to_string(i);
        const std::string action = "joinfound:" + std::to_string(i);
        slots[i] = Btn(b, found, name.c_str(), "-", action.c_str(), FullWidth(kRowH));
    }

    Widget* actions = Array(b, layout, "Actions", true, kGap, 0.0f, FullWidth(kRowH));
    NetplayButton* start = Btn(b, actions, "Start", "Start", "start", FullWidth(kRowH));
    NetplayButton* leave = Btn(b, actions, "Leave", "Leave", "leave", At(0, 0, 110, kRowH));
    NetplayButton* close = Btn(b, actions, "Close", "Close", "close", At(0, 0, 110, kRowH));

    LinkNavigation({{host}, {join}, {delayDown, delayUp}, {search}, {slots[0]}, {slots[1]}, {slots[2]}, {slots[3]},
                    {start, leave, close}});

    RecompMenuController* controller = b.Ensure<RecompMenuController>(root.Get(), "MenuController",
        [&](RecompMenuController* c) {
            Place(c, 0.0f, 0.0f, 0.0f, 0.0f);
            c->Setup("Netplay", options.startVisible, true, host);
        });
    if (controller != nullptr)
    {
        // applied on every run, like the mod settings scene's "Open with"
        controller->SetPanel(panel);
        controller->SetPanelFit(glm::vec2(560.0f, 640.0f), 16.0f, 0);
        controller->SetToggleButton(options.toggleButton);
        controller->SetInHomeMenu(options.inHomeMenu);
        root->SetVisible(true);
    }
    b.Ensure<NetplayMenu>(root.Get(), "NetplayMenu", [](NetplayMenu* n) { Place(n, 0.0f, 0.0f, 0.0f, 0.0f); });

    // the game's look (its Mod Map's menu style), when it has one
    if (ModMap* map = ModSettings::Get().GetMap())
    {
        ModStyle_Apply(root.Get(), map->mStyle);
    }

    if (!updating)
    {
        std::string error;
        AssetDir* dir = ScenesDir(error);
        if (dir == nullptr)
        {
            message = error;
            return false;
        }
        stub = EditorAddUniqueAsset(sceneName.c_str(), dir, Scene::GetStaticType(), true);
        scene = (stub != nullptr && stub->mAsset != nullptr) ? stub->mAsset->As<Scene>() : nullptr;
        if (scene == nullptr)
        {
            message = "Cannot create the Scene asset " + sceneName;
            return false;
        }
    }
    scene->Capture(root.Get());
    AssetManager::Get()->SaveAsset(*stub);
    message = std::string(updating ? "Updated " : "Created ") + stub->mName + ": " + std::to_string(b.added) +
              " node(s) added" + (updating ? ", " + std::to_string(b.kept) + " kept as they were" : std::string()) +
              ". Add it to the game's scene (or launcher) to have the menu in the game.";
    LogDebug("[netplay] %s", message.c_str());
    return true;
}

void DrawWindow(void*)
{
    static Options options;
    static char name[64] = "SC_Netplay";
    static std::string message;
    static const int kButtons[] = {-1, GAMEPAD_SELECT, GAMEPAD_THUMBL, GAMEPAD_THUMBR, GAMEPAD_Z};
    static int buttonIndex = 2; // L3 (Select opens the mod settings menu)

    ImGui::TextWrapped("The in-game netplay menu: host, join by address or from the LAN, the players, Start. "
                       "Generating again only adds what is missing; your changes to the scene stay.");
    ImGui::InputText("Scene name", name, sizeof(name));
    const char* preview = kButtons[buttonIndex] < 0 ? "None (HOME menu / scripts)" : RecompGamepadButtonName(kButtons[buttonIndex]);
    if (ImGui::BeginCombo("Open with", preview))
    {
        for (int i = 0; i < (int)(sizeof(kButtons) / sizeof(kButtons[0])); i++)
        {
            const char* label = kButtons[i] < 0 ? "None (HOME menu / scripts)" : RecompGamepadButtonName(kButtons[i]);
            if (ImGui::Selectable(label, i == buttonIndex))
            {
                buttonIndex = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Open when the scene starts (a lobby screen)", &options.startVisible);
    ImGui::Checkbox("List it in the HOME menu", &options.inHomeMenu);

    const bool exists = FetchAssetStub(name) != nullptr;
    if (ImGui::Button(exists ? "Update scene" : "Generate scene"))
    {
        options.sceneName = name;
        options.toggleButton = kButtons[buttonIndex];
        Generate(options, message);
    }
    if (!message.empty())
    {
        ImGui::TextWrapped("%s", message.c_str());
    }
    ImGui::Separator();
    ImGui::TextDisabled("The Menu Style comes from the game's Mod Map (Tools > Recomp > Mods > Menu Style).");
}
}

void NetplaySceneGen::Register(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    if (hooks->RegisterWindow != nullptr)
    {
        hooks->RegisterWindow(hookId, "Generate Network Scene", kWindowId, DrawWindow, nullptr);
    }
    if (hooks->AddMenuItem != nullptr)
    {
        hooks->AddMenuItem(hookId, "Tools", "Recomp/UI/Generate Network Scene...",
                           [](void*) {
                               if (sHooks != nullptr && sHooks->OpenWindow != nullptr)
                               {
                                   sHooks->OpenWindow(kWindowId);
                               }
                           },
                           nullptr, nullptr);
    }
}

#endif
