/**
 * @file NetplayMenu.cpp
 * @brief The in-game netplay menu's nodes (see NetplayMenu.h).
 */
#include "NetplayMenu.h"
#include "NetplaySession.h"

#include "ModBaseProvider.h" // com.recomp.mod.base: Recomp_PointerInUse

#include "Nodes/Widgets/Button.h"
#include "Nodes/Widgets/InputField.h"
#include "Nodes/Widgets/ScrollContainer.h"
#include "Nodes/Widgets/Text.h"
#include "Engine.h"
#include "Property.h"
#include "SignalBus.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

FORCE_LINK_DEF(NetplayButton);
DEFINE_NODE(NetplayButton, RecompButton);
FORCE_LINK_DEF(NetplayMenu);
DEFINE_NODE(NetplayMenu, Widget);

namespace
{
// The menu a node belongs to: the NetplayMenu among the children of its nearest ancestor that
// has one (the scene's root for the Network scene, the Netplay page's list in the Mods UI).
NetplayMenu* MenuOf(Node* node)
{
    for (Node* n = node; n != nullptr; n = n->GetParent())
    {
        if (Node* menu = n->FindChild("NetplayMenu", false))
        {
            if (NetplayMenu* m = menu->As<NetplayMenu>())
            {
                return m;
            }
        }
    }
    return nullptr;
}

// Where the menu's widgets are looked up by name: the menu's parent (so names like "Note" in the
// rest of the Mods UI are not touched).
Node* Scope(NetplayMenu* menu, Node* from)
{
    if (menu != nullptr && menu->GetParent() != nullptr)
    {
        return menu->GetParent();
    }
    while (from != nullptr && from->GetParent() != nullptr)
    {
        from = from->GetParent();
    }
    return from;
}

template <class T>
T* Find(Node* root, const char* name)
{
    Node* node = root != nullptr ? root->FindChild(name, true) : nullptr;
    return node != nullptr ? node->As<T>() : nullptr;
}

std::string Trimmed(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.pop_back();
    }
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
    {
        ++start;
    }
    return text.substr(start);
}

void SetText(Node* root, const char* name, const std::string& text)
{
    if (Text* t = Find<Text>(root, name))
    {
        if (t->GetText() != text)
        {
            t->SetText(text);
        }
    }
}

void SetLabel(Node* root, const char* name, const std::string& text)
{
    if (Button* b = Find<Button>(root, name))
    {
        if (b->GetTextString() != text)
        {
            b->SetTextString(text);
        }
    }
}

bool InSession(NetplayState state)
{
    return state == NETPLAY_LOBBY || state == NETPLAY_JOINING || state == NETPLAY_SYNCING || state == NETPLAY_READY ||
           state == NETPLAY_RUNNING;
}
}

// ---- NetplayButton ---------------------------------------------------------------------
void NetplayButton::Activate()
{
    // the button's own reaction (signals, Lua OnActivated), not RecompButton's setting / request
    Button::Activate();
    NetplayMenu::RunAction(this, mAction);
}

void NetplayButton::GatherProperties(std::vector<Property>& outProps)
{
    RecompButton::GatherProperties(outProps);
    SCOPED_CATEGORY("Netplay");
    outProps.push_back(Property(DatumType::String, "Action", this, &mAction));
}

void NetplayButton::SetAction(const std::string& action)
{
    mAction = action;
}

const std::string& NetplayButton::GetAction() const
{
    return mAction;
}

// ---- NetplayMenu: opening / closing, SignalBus ------------------------------------------------
namespace
{
std::vector<NetplayMenu*> sMenus; // started menus, oldest first
NetplayState sLastSessionState = NETPLAY_IDLE;

const char* StateName(NetplayState state)
{
    switch (state)
    {
    case NETPLAY_IDLE: return "idle";
    case NETPLAY_LOBBY: return "lobby";
    case NETPLAY_JOINING: return "joining";
    case NETPLAY_SYNCING: return "syncing";
    case NETPLAY_READY: return "ready";
    case NETPLAY_RUNNING: return "running";
    case NETPLAY_DESYNC: return "desync";
    case NETPLAY_DISCONNECTED: return "disconnected";
    case NETPLAY_ERROR: return "error";
    }
    return "idle";
}

// "Netplay.Open": the menu named by the first argument (its UI root's name), else the first one
void OnOpenSignal(Node* listener, const std::vector<Datum>& args)
{
    NetplayMenu* menu = listener != nullptr ? listener->As<NetplayMenu>() : nullptr;
    if (menu == nullptr || sMenus.empty())
    {
        return;
    }
    const std::string wanted =
        (!args.empty() && args[0].GetType() == DatumType::String) ? args[0].GetString() : std::string();
    if (wanted.empty() ? menu == sMenus.front() : menu->UiName() == wanted)
    {
        menu->OpenMenu();
    }
}
}

void NetplayMenu::Start()
{
    Widget::Start();
    if (std::find(sMenus.begin(), sMenus.end(), this) == sMenus.end())
    {
        sMenus.push_back(this);
    }
    // a scene saved by an older generator can carry stale links (the gamepad stopped at Search)
    LinkNavigation(Scope(this, this));
    GetSignalBus()->Subscribe("Netplay.Open", this, OnOpenSignal);
}

void NetplayMenu::Stop()
{
    GetSignalBus()->Unsubscribe("Netplay.Open", this);
    sMenus.erase(std::remove(sMenus.begin(), sMenus.end(), this), sMenus.end());
    Widget::Stop();
}

void NetplayMenu::Destroy()
{
    GetSignalBus()->Unsubscribe("Netplay.Open", this);
    sMenus.erase(std::remove(sMenus.begin(), sMenus.end(), this), sMenus.end());
    Widget::Destroy();
}

std::string NetplayMenu::UiName()
{
    Node* n = this;
    while (n->GetParent() != nullptr)
    {
        n = n->GetParent();
    }
    return n->GetName();
}

// In the Mods UI the menu lives on the page "Page_Netplay" (its tab shows it).
Node* NetplayMenu::ModsPage()
{
    for (Node* n = GetParent(); n != nullptr; n = n->GetParent())
    {
        if (n->GetName() == "Page_Netplay")
        {
            return n;
        }
    }
    return nullptr;
}

bool NetplayMenu::IsMenuOpen()
{
    RecompMenuController* controller = RecompMenuController::FindFor(this);
    if (controller == nullptr || !controller->IsOpen())
    {
        return false;
    }
    Node* page = ModsPage();
    Widget* pageWidget = page != nullptr ? page->As<Widget>() : nullptr;
    return pageWidget == nullptr || pageWidget->IsVisible();
}

bool NetplayMenu::OpenMenu()
{
    RecompMenuController* controller = RecompMenuController::FindFor(this);
    if (controller == nullptr)
    {
        return false;
    }
    controller->Open();
    if (ModsPage() != nullptr)
    {
        // the Mods UI: show its Netplay page, as its tab does
        Node* root = this;
        while (root->GetParent() != nullptr)
        {
            root = root->GetParent();
        }
        Node* tab = root->FindChild("Tab_Netplay", true);
        if (Button* button = tab != nullptr ? tab->As<Button>() : nullptr)
        {
            button->Activate();
        }
    }
    return true;
}

void NetplayMenu::CloseMenu()
{
    if (RecompMenuController* controller = RecompMenuController::FindFor(this))
    {
        controller->Close();
    }
}

namespace
{
void RefreshTree(Node* node)
{
    if (node == nullptr)
    {
        return;
    }
    if (Text* text = node->As<Text>())
    {
        text->MarkVerticesDirty();
    }
    if (Widget* widget = node->As<Widget>())
    {
        widget->MarkDirty();
    }
    for (uint32_t i = 0; i < node->GetNumChildren(); ++i)
    {
        RefreshTree(node->GetChild((int32_t)i));
    }
}
}

void NetplayMenu::RefreshWidgets()
{
    RefreshTree(Scope(this, this));
}

void NetplayMenu::LinkNavigation(Node* scope)
{
    std::vector<std::vector<const char*>> names = {{"Host"}, {"Join"}, {"DelayDown", "DelayUp"}, {"Search"}};
    static const char* const kFound[kFoundSlots] = {"Found_0", "Found_1", "Found_2", "Found_3"};
    for (const char* found : kFound)
    {
        names.push_back({found});
    }
    names.push_back({"Start", "Leave", "Close"});

    std::vector<std::vector<Button*>> rows;
    for (const std::vector<const char*>& rowNames : names)
    {
        std::vector<Button*> row;
        for (const char* name : rowNames)
        {
            if (Button* b = Find<Button>(scope, name))
            {
                row.push_back(b);
            }
        }
        if (!row.empty())
        {
            rows.push_back(row);
        }
    }
    // every link inside the menu, overwritten; the first row's up and the last row's down lead
    // out of it (the Mods UI's tab, its Save) and are kept
    for (size_t r = 0; r < rows.size(); ++r)
    {
        for (size_t c = 0; c < rows[r].size(); ++c)
        {
            Button* btn = rows[r][c];
            btn->SetNavLeft(c > 0 ? rows[r][c - 1] : nullptr);
            btn->SetNavRight(c + 1 < rows[r].size() ? rows[r][c + 1] : nullptr);
            if (r > 0)
            {
                const std::vector<Button*>& up = rows[r - 1];
                btn->SetNavUp(up[c < up.size() ? c : up.size() - 1]);
            }
            if (r + 1 < rows.size())
            {
                const std::vector<Button*>& down = rows[r + 1];
                btn->SetNavDown(down[c < down.size() ? c : down.size() - 1]);
            }
        }
    }
}

bool NetplayMenu::OpenAny(const std::string& uiName)
{
    for (NetplayMenu* menu : sMenus)
    {
        if (uiName.empty() || menu->UiName() == uiName)
        {
            return menu->OpenMenu();
        }
    }
    return false;
}

bool NetplayMenu::CloseAny()
{
    bool closed = false;
    for (NetplayMenu* menu : sMenus)
    {
        if (menu->IsMenuOpen())
        {
            menu->CloseMenu();
            closed = true;
        }
    }
    return closed;
}

bool NetplayMenu::AnyOpen()
{
    for (NetplayMenu* menu : sMenus)
    {
        if (menu->IsMenuOpen())
        {
            return true;
        }
    }
    return false;
}

// Every frame (the addon's tick: a closed menu doesn't tick itself). Sends "Netplay.Opened" /
// "Netplay.Closed" when a menu shows / goes away, and closes the menus when a session starts.
void NetplayMenu::TickAll()
{
    const NetplayState state = netplay_state(NetplaySession::Get());
    const bool started = state == NETPLAY_RUNNING && sLastSessionState != NETPLAY_RUNNING;
    sLastSessionState = state;

    const std::vector<NetplayMenu*> menus = sMenus; // a handler may open / close menus
    for (NetplayMenu* menu : menus)
    {
        if (std::find(sMenus.begin(), sMenus.end(), menu) == sMenus.end())
        {
            continue;
        }
        if (started && menu->IsMenuOpen())
        {
            menu->CloseMenu(); // the game is on: back to it
        }
        const bool open = menu->IsMenuOpen();
        if (open != menu->mWasOpen)
        {
            menu->mWasOpen = open;
            if (open)
            {
                menu->RefreshWidgets();
            }
            GetSignalBus()->Emit(open ? "Netplay.Opened" : "Netplay.Closed",
                                 {Datum(menu->UiName()), Datum(StateName(state))});
        }
    }
}

// ---- NetplayMenu: contents -----------------------------------------------------------------------
void NetplayMenu::SetNote(const std::string& note)
{
    mNote = note;
    mNoteTime = 6.0f;
}

void NetplayMenu::RunAction(Node* from, const std::string& action)
{
    NetplayMenu* menu = MenuOf(from);
    Node* root = Scope(menu, from);
    Netplay* np = NetplaySession::Get();
    auto note = [menu](const std::string& text) {
        if (menu != nullptr)
        {
            menu->SetNote(text);
        }
    };

    if (action == "host")
    {
        InputField* portField = Find<InputField>(root, "Port");
        const int port = portField != nullptr ? std::atoi(Trimmed(portField->GetText()).c_str()) : 0;
        if (!NetplaySession::Host(port))
        {
            note(netplay_status_text(np));
        }
    }
    else if (action == "join")
    {
        InputField* addressField = Find<InputField>(root, "Address");
        const std::string address = addressField != nullptr ? Trimmed(addressField->GetText()) : std::string();
        if (address.empty())
        {
            note("Type the host's address (192.168.1.20, or 192.168.1.20:27500), or search the LAN.");
        }
        else if (!NetplaySession::Join(address.c_str()))
        {
            note(netplay_status_text(np));
        }
    }
    else if (action == "search")
    {
        if (menu != nullptr)
        {
            menu->mSearching = !menu->mSearching;
            netplay_search(np, menu->mSearching ? 1 : 0);
        }
    }
    else if (action.compare(0, 10, "joinfound:") == 0)
    {
        NetplayFoundHost hosts[16];
        const int n = netplay_found(np, hosts, 16);
        const int index = std::atoi(action.c_str() + 10);
        if (index < 0 || index >= n)
        {
            note("No session there: search the LAN first.");
        }
        else if (!hosts[index].compatible)
        {
            note("That session plays another game or ROM.");
        }
        else if (hosts[index].in_game)
        {
            note("That session has already started.");
        }
        else
        {
            NetplaySession::Join(hosts[index].address);
        }
    }
    else if (action == "start")
    {
        if (!netplay_is_host(np) || netplay_state(np) != NETPLAY_LOBBY)
        {
            note("Only the host starts the game, from its lobby.");
        }
        else if (!NetplaySession::Start())
        {
            note("Not yet: every player must have joined and received the save data.");
        }
    }
    else if (action == "leave")
    {
        NetplaySession::Leave();
    }
    else if (action == "delay-" || action == "delay+")
    {
        NetplaySession::SetInputDelay(NetplaySession::InputDelay() + (action == "delay+" ? 1 : -1));
    }
    else if (action == "close")
    {
        if (RecompMenuController* controller = RecompMenuController::FindFor(from))
        {
            controller->Close();
        }
    }
}

void NetplayMenu::Tick(float deltaTime)
{
    Widget::Tick(deltaTime);

    Node* root = Scope(this, this);
    Netplay* np = NetplaySession::Get();
    const NetplayState state = netplay_state(np);

    std::string status = netplay_status_text(np);
    if (!NetplaySession::HasGame())
    {
        status += "\nThe game has not started yet.";
    }
    else if (netplay_peer_flavor_differs(np))
    {
        status += "\nAnother machine runs another build of the game: if the games differ, the session stops.";
    }
    SetText(root, "Status", status);

    std::string players;
    if (InSession(state) || state == NETPLAY_DESYNC)
    {
        NetplayPlayerInfo list[NETPLAY_MAX_PLAYERS];
        const int n = netplay_players(np, list, NETPLAY_MAX_PLAYERS);
        for (int i = 0; i < n; i++)
        {
            char line[96];
            char ping[24] = "";
            if (list[i].rtt_ms >= 0 && !list[i].is_local)
            {
                std::snprintf(ping, sizeof(ping), ", %d ms", list[i].rtt_ms);
            }
            std::snprintf(line, sizeof(line), "Player %d: %s%s - %s%s\n", list[i].port + 1, list[i].name,
                          list[i].is_local ? " (you)" : "",
                          !list[i].connected ? "left" : list[i].ready ? "ready" : "joining", ping);
            players += line;
        }
    }
    SetText(root, "Players", players);

    char delay[16];
    std::snprintf(delay, sizeof(delay), "%d", NetplaySession::InputDelay());
    SetText(root, "DelayValue", delay);

    SetLabel(root, "Search", mSearching ? "Searching the LAN... (press to stop)" : "Search the LAN");
    NetplayFoundHost hosts[16];
    const int found = mSearching ? netplay_found(np, hosts, 16) : 0;
    for (int i = 0; i < kFoundSlots; i++)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "Found_%d", i);
        std::string label = "-";
        if (i < found)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "Join %s: %s (%d/%d)%s", hosts[i].host_name, hosts[i].game_name,
                          hosts[i].players, hosts[i].max_players,
                          !hosts[i].compatible ? " - another game or ROM" : hosts[i].in_game ? " - in game" : "");
            label = text;
        }
        SetLabel(root, name, label);
    }

    if (netplay_is_host(np) && state == NETPLAY_LOBBY)
    {
        NetplayPlayerInfo list[NETPLAY_MAX_PLAYERS];
        const int n = netplay_players(np, list, NETPLAY_MAX_PLAYERS);
        int ready = 0;
        for (int i = 0; i < n; i++)
        {
            ready += list[i].ready;
        }
        char label[48];
        std::snprintf(label, sizeof(label), "Start (%d/%d ready)", ready, n);
        SetLabel(root, "Start", label);
    }
    else
    {
        SetLabel(root, "Start", "Start");
    }

    if (mNoteTime > 0.0f)
    {
        mNoteTime -= deltaTime;
        if (mNoteTime <= 0.0f)
        {
            mNote.clear();
        }
    }
    SetText(root, "Note", mNote);

    KeepSelectionInView();
}

void NetplayMenu::KeepSelectionInView()
{
    Node* scope = Scope(this, this);
    Node* page = ModsPage();
    if (page != nullptr)
    {
        // the Mods UI's footer (Save / Reset / Close) goes back up to the menu's last row
        // (the page shows set it to the first button found, as the list is in its own scroll)
        Button* last = Find<Button>(scope, "Start");
        Node* root = page;
        while (root->GetParent() != nullptr)
        {
            root = root->GetParent();
        }
        Node* footer = root->FindChild("Footer", true);
        for (uint32_t i = 0; last != nullptr && footer != nullptr && i < footer->GetNumChildren(); ++i)
        {
            if (Button* b = footer->GetChild((int32_t)i)->As<Button>())
            {
                if (b->GetNavUp() != last)
                {
                    b->SetNavUp(last);
                }
            }
        }
    }

    if (ScrollContainer* pageScroll = page != nullptr ? page->As<ScrollContainer>() : nullptr)
    {
        // a page saved before the page sized its list scroll (see NetplaySceneGen)
        Widget* content = pageScroll->GetContentWidget();
        if (content != nullptr && content->As<ScrollContainer>() != nullptr &&
            pageScroll->GetScrollSizeMode() != ScrollSizeMode::FitBoth)
        {
            pageScroll->SetScrollSizeMode(ScrollSizeMode::FitBoth);
            pageScroll->SetHorizontalScrollbarMode(ScrollbarMode::Hidden);
            pageScroll->SetVerticalScrollbarMode(ScrollbarMode::Hidden);
            pageScroll->SetScrollOffset(glm::vec2(0.0f));
        }
    }

    // the selection follows the mouse while it's in use: scrolling then would move another
    // button under the pointer, which selects it, which scrolls... (the list jittered)
    if (Recomp_PointerInUse())
    {
        return;
    }
    Button* selected = Button::GetSelectedButton();
    bool inMenu = false;
    for (Node* n = selected; n != nullptr && !inMenu; n = n->GetParent())
    {
        inMenu = n == scope;
    }
    if (!inMenu)
    {
        return;
    }
    const float kEdge = 6.0f;
    // the first row shows the menu's top too (status, players)
    const bool top = selected->GetName() == "Host";
    for (Node* n = selected->GetParent(); n != nullptr; n = n->GetParent())
    {
        ScrollContainer* scroll = n->As<ScrollContainer>();
        Widget* content = scroll != nullptr ? scroll->GetContentWidget() : nullptr;
        if (content != nullptr)
        {
            // its content size is measured only while it's dirty (not while hidden)
            if (scroll->GetContentSize().y != content->GetHeight())
            {
                scroll->MarkDirty();
            }
            const Rect view = scroll->GetRect();
            const Rect c = content->GetRect();
            const Rect r = selected->GetRect();
            const glm::vec2 s = scroll->GetAbsoluteScale();
            if (s.y > 0.0f && view.mHeight > 0.0f && r.mHeight > 0.0f)
            {
                // in the content's own units, so the current offset doesn't matter
                const float viewH = view.mHeight / s.y;
                const float y0 = top ? 0.0f : (r.mY - c.mY) / s.y;
                const float y1 = (r.mY - c.mY + r.mHeight) / s.y;
                float offset = scroll->GetScrollOffset().y;
                if (y0 - kEdge < offset)
                {
                    offset = y0 - kEdge;
                }
                else if (y1 + kEdge > offset + viewH)
                {
                    offset = y1 + kEdge - viewH;
                }
                offset = std::max(0.0f, std::min(offset, content->GetHeight() - viewH));
                if (offset != scroll->GetScrollOffset().y)
                {
                    scroll->SetScrollOffsetY(offset);
                }
            }
        }
        if (n == page)
        {
            break;
        }
    }
}
