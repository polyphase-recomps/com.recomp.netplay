/**
 * @file NetplaySceneGen.cpp
 * @brief Tools > Recomp > UI > Generate Network Scene, and the Netplay page of the Mods UI
 *        (see NetplaySceneGen.h).
 */
#include "NetplaySceneGen.h"

#if EDITOR

#include "NetplayMenu.h"

// com.recomp.mod.base: the non-destructive UI builder, the menu controller, the game's menu
// style and its Mod Settings scene
#include "ModBaseModMap.h"
#include "ModBaseSceneGen.h"
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

#include <algorithm>
#include <string>
#include <vector>

using namespace RecompUi;

namespace
{
const char* const kWindowId = "com.recomp.netplay.scenegen";
const float kRowH = 32.0f;
const float kGap = 6.0f;
const float kPad = 16.0f;
const float kTabW = 120.0f; // as the Mods UI's tabs
// the sessions found scroll in a box this tall (two rows): the whole list doesn't fit a
// 640x480 Wii / GameCube screen
const float kFoundViewH = 2 * kRowH + kGap;
const float kFoundListH = NetplayMenu::kFoundSlots * kRowH + (NetplayMenu::kFoundSlots - 1) * kGap;
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

// The menu's rows, in a column (an ArrayWidget): status, players, host, join, delay, LAN search,
// the sessions found, Start / Leave (/ Close). Returns the buttons by row, for navigation, and
// the column's height (an ArrayWidget doesn't size itself).
std::vector<std::vector<Button*>> BuildRows(Builder& b, Widget* column, bool withClose, float& height)
{
    Label(b, column, "Status", "Not in a session", FullWidth(44.0f), 14.0f, kDimColor);
    Label(b, column, "Players", "", FullWidth(84.0f), 14.0f, kTextColor);
    Label(b, column, "Note", "", FullWidth(20.0f), 13.0f, kHeaderColor);

    Widget* hostRow = Array(b, column, "HostRow", true, kGap, 0.0f, FullWidth(kRowH));
    NetplayButton* host = Btn(b, hostRow, "Host", "Host a session", "host", FullWidth(kRowH));
    Field(b, hostRow, "Port", "", "27464", At(0, 0, 110, kRowH));

    Widget* joinRow = Array(b, column, "JoinRow", true, kGap, 0.0f, FullWidth(kRowH));
    Field(b, joinRow, "Address", "", "Host address (ip or ip:port)", FullWidth(kRowH));
    NetplayButton* join = Btn(b, joinRow, "Join", "Join", "join", At(0, 0, 110, kRowH));

    Widget* delayRow = Array(b, column, "DelayRow", true, kGap, 0.0f, FullWidth(kRowH));
    Label(b, delayRow, "DelayLabel", "Input delay (frames)", FullWidth(kRowH), kFontSize, kTextColor);
    NetplayButton* delayDown = Btn(b, delayRow, "DelayDown", "-", "delay-", At(0, 0, 44, kRowH));
    if (Text* value = Label(b, delayRow, "DelayValue", "2", At(0, 0, 44, kRowH), kFontSize, kHeaderColor))
    {
        value->SetHorizontalJustification(Justification::Center);
    }
    NetplayButton* delayUp = Btn(b, delayRow, "DelayUp", "+", "delay+", At(0, 0, 44, kRowH));

    NetplayButton* search = Btn(b, column, "Search", "Search the LAN", "search", FullWidth(kRowH));
    // the sessions found: a short vertical scroll box (the menu controller scrolls the selected
    // one into view; the right stick and the mouse wheel scroll it too)
    ScrollContainer* foundScroll = Scroll(b, column, "FoundScroll", false, FullWidth(kFoundViewH));
    Node* foundParent = column;
    if (foundScroll != nullptr)
    {
        foundParent = foundScroll;
        // right after the Search button (a scroll box added to an older scene lands last)
        const int32_t searchAt = column->FindChildIndex("Search");
        const int32_t at = column->FindChildIndex(foundScroll);
        if (searchAt >= 0 && at != searchAt + 1)
        {
            foundScroll->Attach(column, false, at > searchAt ? searchAt + 1 : searchAt);
        }
        // a scene made before the scroll box: its list moves in
        if (Node* old = column->FindChild("Found", false))
        {
            old->Attach(foundScroll, false, 0);
        }
    }
    Widget* found = Array(b, foundParent, "Found", false, kGap, 0.0f, At(0, 0, 0, kFoundListH));
    if (found != nullptr && foundScroll != nullptr)
    {
        found->SetHeight(kFoundListH); // the scrolled content is as tall as its rows
    }
    std::vector<std::vector<Button*>> rows = {{host}, {join}, {delayDown, delayUp}, {search}};
    for (int i = 0; i < NetplayMenu::kFoundSlots; i++)
    {
        const std::string name = "Found_" + std::to_string(i);
        const std::string action = "joinfound:" + std::to_string(i);
        rows.push_back({Btn(b, found, name.c_str(), "-", action.c_str(), FullWidth(kRowH))});
    }

    Widget* actions = Array(b, column, "Actions", true, kGap, 0.0f, FullWidth(kRowH));
    NetplayButton* start = Btn(b, actions, "Start", "Start", "start", FullWidth(kRowH));
    NetplayButton* leave = Btn(b, actions, "Leave", "Leave", "leave", At(0, 0, 110, kRowH));
    if (withClose)
    {
        rows.push_back({start, leave, Btn(b, actions, "Close", "Close", "close", At(0, 0, 110, kRowH))});
    }
    else
    {
        rows.push_back({start, leave});
    }

    const float heights[] = {44, 84, 20, kRowH, kRowH, kRowH, kRowH, kFoundViewH, kRowH};
    height = 0.0f;
    for (float h : heights)
    {
        height += h + kGap;
    }
    return rows;
}

// The scene asset of that name opened for updating (root set), or nullptr root when there is none.
bool OpenScene(const std::string& sceneName, AssetStub*& stub, Scene*& scene, NodePtr& root, std::string& message)
{
    stub = FetchAssetStub(sceneName);
    scene = nullptr;
    if (stub != nullptr && stub->mType != Scene::GetStaticType())
    {
        message = "An asset named " + sceneName + " exists and is not a Scene.";
        return false;
    }
    if (stub != nullptr)
    {
        scene = LoadAsset<Scene>(sceneName);
        if (scene != nullptr)
        {
            root = scene->Instantiate();
        }
    }
    return true;
}

void SaveScene(AssetStub* stub, Scene* scene, Node* root)
{
    scene->Capture(root);
    AssetManager::Get()->SaveAsset(*stub);
}

void ApplyGameStyle(Node* root)
{
    if (ModMap* map = ModSettings::Get().GetMap())
    {
        ModStyle_Apply(root, map->mStyle);
    }
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

// ---- the Network scene ----------------------------------------------------------------------
bool Generate(const Options& options, std::string& message)
{
    const std::string sceneName = options.sceneName.empty() ? std::string("SC_Netplay") : options.sceneName;
    AssetStub* stub = nullptr;
    Scene* scene = nullptr;
    NodePtr root;
    if (!OpenScene(sceneName, stub, scene, root, message))
    {
        return false;
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
    Widget* layout = Array(b, scroll != nullptr ? (Node*)scroll : (Node*)panel, "Layout", false, kGap, kPad,
                           FullWidth(kRowH));
    if (layout == nullptr)
    {
        message = "Layout exists but is not an ArrayWidget (or the engine has none): left as it is.";
        return false;
    }
    if (Text* title = Label(b, layout, "Title", "Netplay", FullWidth(34.0f), 24.0f, kHeaderColor))
    {
        title->SetHorizontalJustification(Justification::Center);
    }
    float rowsHeight = 0.0f;
    std::vector<std::vector<Button*>> rows = BuildRows(b, layout, true, rowsHeight);
    layout->SetHeight(2 * kPad + 34.0f + kGap + rowsHeight); // a scrolled list is as tall as its content
    LinkNavigation(rows);

    Button* first = rows.empty() ? nullptr : rows.front().front();
    RecompMenuController* controller = b.Ensure<RecompMenuController>(root.Get(), "MenuController",
        [&](RecompMenuController* c) {
            Place(c, 0.0f, 0.0f, 0.0f, 0.0f);
            c->Setup("Netplay", options.startVisible, true, first);
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
    ApplyGameStyle(root.Get());

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
    SaveScene(stub, scene, root.Get());
    message = std::string(updating ? "Updated " : "Created ") + stub->mName + ": " + std::to_string(b.added) +
              " node(s) added" + (updating ? ", " + std::to_string(b.kept) + " kept as they were" : std::string()) +
              ". Add it to the game's scene (or launcher) to have the menu in the game.";
    LogDebug("[netplay] %s", message.c_str());
    return true;
}

// ---- the Netplay page of the Mods UI ----------------------------------------------------------
// com.recomp.mod.base's Mod Settings scene: TabStrip > Tabs > Tab_<group> (RecompButton
// "@page:<group>"), Pages > Page_<group> (ScrollContainer) > List, Footer (Save / Reset / Close).
// Adds Tab_Netplay and Page_Netplay the same way, so its page switching, gamepad navigation and
// menu style work on them.
bool AddToModsUI(const std::string& sceneName, std::string& message)
{
    AssetStub* stub = nullptr;
    Scene* scene = nullptr;
    NodePtr root;
    if (!OpenScene(sceneName, stub, scene, root, message))
    {
        return false;
    }
    if (root.Get() == nullptr)
    {
        message = "No scene " + sceneName + ": generate the Mods UI first (Tools > Recomp > Mods > Generate Mod Settings "
                  "Scene...), or type its name.";
        return false;
    }
    Node* tabsNode = root->FindChild("Tabs", true);
    Node* pagesNode = root->FindChild("Pages", true);
    Widget* tabs = tabsNode != nullptr ? tabsNode->As<Widget>() : nullptr;
    if (tabs == nullptr || pagesNode == nullptr)
    {
        message = sceneName + " has no Tabs / Pages: it is not a Mod Settings scene made by com.recomp.mod.base.";
        return false;
    }

    Builder b;
    // the other tabs, before ours is added (for navigation)
    std::vector<Button*> otherTabs;
    for (uint32_t i = 0; i < tabs->GetNumChildren(); ++i)
    {
        Button* t = tabs->GetChild((int32_t)i)->As<Button>();
        if (t != nullptr && t->GetName() != "Tab_Netplay")
        {
            otherTabs.push_back(t);
        }
    }
    RecompButton* tab = SettingButton(b, tabs, "Tab_Netplay", "Netplay", "@page:Netplay", 1, At(0, 0, kTabW, kRowH));
    if (tab != nullptr)
    {
        tab->SetLabelFormat("");
        tab->SetTextString("Netplay");
    }
    tabs->SetWidth(float(tabs->GetNumChildren()) * (kTabW + 4.0f)); // the strip scrolls when they don't fit

    ScrollContainer* page = Scroll(b, pagesNode, "Page_Netplay", false, [](Widget* w) {
        Full(w);
        w->SetVisible(false); // shown by its tab
    });
    Widget* list = Array(b, page, "List", false, 4.0f, 0.0f, At(0, 0, 0, kRowH));
    if (list == nullptr)
    {
        message = "Page_Netplay/List exists but is not an ArrayWidget: left as it is.";
        return false;
    }
    float height = 0.0f;
    std::vector<std::vector<Button*>> rows = BuildRows(b, list, false, height);
    // the menu that keeps the page up to date (looks its widgets up within this list)
    b.Ensure<NetplayMenu>(list, "NetplayMenu", [](NetplayMenu* n) { Place(n, 0.0f, 0.0f, 0.0f, 0.0f); });
    list->SetHeight(height + 4.0f);
    LinkNavigation(rows);

    // gamepad: tab row <-> our tab, tab -> the page's first button, first row -> the tab,
    // last row -> the footer's Save (the footer's way back up is set when the page shows)
    if (tab != nullptr)
    {
        if (!otherTabs.empty())
        {
            Button* last = otherTabs.back();
            if (last->GetNavRight() == nullptr) last->SetNavRight(tab);
            if (tab->GetNavLeft() == nullptr) tab->SetNavLeft(last);
        }
        if (!rows.empty())
        {
            if (tab->GetNavDown() == nullptr) tab->SetNavDown(rows.front().front());
            for (Button* btn : rows.front())
            {
                if (btn->GetNavUp() == nullptr) btn->SetNavUp(tab);
            }
        }
    }
    Node* saveNode = root->FindChild("Save", true);
    Button* save = saveNode != nullptr ? saveNode->As<Button>() : nullptr;
    if (save != nullptr && !rows.empty())
    {
        for (Button* btn : rows.back())
        {
            if (btn->GetNavDown() == nullptr) btn->SetNavDown(save);
        }
    }
    ApplyGameStyle(root.Get());

    SaveScene(stub, scene, root.Get());
    message = "Netplay page " + std::string(b.added > 0 ? "added to " : "checked in ") + stub->mName + ": " +
              std::to_string(b.added) + " node(s) added, " + std::to_string(b.kept) +
              " kept. After regenerating the Mods UI, run this again (it keeps the tab strip wide enough).";
    LogDebug("[netplay] %s", message.c_str());
    return true;
}

// ---- the window ------------------------------------------------------------------------------
void DrawWindow(void*)
{
    static Options options;
    static char name[64] = "SC_Netplay";
    static char modsScene[96] = "";
    static bool modsSceneLoaded = false;
    static std::string message;
    static const int kButtons[] = {-1, GAMEPAD_SELECT, GAMEPAD_THUMBL, GAMEPAD_THUMBR, GAMEPAD_Z};
    static int buttonIndex = 2; // L3 (Select opens the mod settings menu)

    ImGui::SeparatorText("In the Mods UI");
    if (!modsSceneLoaded)
    {
        std::snprintf(modsScene, sizeof(modsScene), "%s", ModScene_DefaultName(ModSettings::Get().GetMap()).c_str());
        modsSceneLoaded = true;
    }
    ImGui::TextWrapped("A Netplay tab in the game's Mod Settings scene (the Mods UI): host, join, the LAN, the "
                       "players, Start, with the rest of the mod settings.");
    ImGui::InputText("Mods UI scene", modsScene, sizeof(modsScene));
    if (ImGui::Button("Add Netplay to the Mods UI"))
    {
        AddToModsUI(modsScene, message);
    }

    ImGui::SeparatorText("As its own scene");
    ImGui::TextWrapped("The netplay menu on its own. Generating again only adds what is missing; your changes stay.");
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
        ImGui::Separator();
        ImGui::TextWrapped("%s", message.c_str());
    }
    ImGui::Separator();
    ImGui::TextDisabled("The Menu Style comes from the game's Mod Map (Tools > Recomp > Mods > Menu Style).");
}

void OpenWindow(void*)
{
    if (sHooks != nullptr && sHooks->OpenWindow != nullptr)
    {
        sHooks->OpenWindow(kWindowId);
    }
}
}

void NetplaySceneGen::Register(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    if (hooks->RegisterWindow != nullptr)
    {
        hooks->RegisterWindow(hookId, "Netplay UI", kWindowId, DrawWindow, nullptr);
    }
    if (hooks->AddMenuItem != nullptr)
    {
        hooks->AddMenuItem(hookId, "Tools", "Recomp/UI/Generate Network Scene...", OpenWindow, nullptr, nullptr);
        hooks->AddMenuItem(hookId, "Tools", "Recomp/UI/Add Netplay to Mods UI...", OpenWindow, nullptr, nullptr);
    }
}

#endif
