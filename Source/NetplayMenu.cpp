/**
 * @file NetplayMenu.cpp
 * @brief The in-game netplay menu's nodes (see NetplayMenu.h).
 */
#include "NetplayMenu.h"
#include "NetplaySession.h"

#include "Nodes/Widgets/Button.h"
#include "Nodes/Widgets/InputField.h"
#include "Nodes/Widgets/Text.h"
#include "Property.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

FORCE_LINK_DEF(NetplayButton);
DEFINE_NODE(NetplayButton, RecompButton);
FORCE_LINK_DEF(NetplayMenu);
DEFINE_NODE(NetplayMenu, Widget);

namespace
{
// The top of the UI a node is in (the generated scene's root Canvas).
Node* UiRoot(Node* node)
{
    while (node != nullptr && node->GetParent() != nullptr)
    {
        node = node->GetParent();
    }
    return node;
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

// ---- NetplayMenu ---------------------------------------------------------------------
void NetplayMenu::SetNote(const std::string& note)
{
    mNote = note;
    mNoteTime = 6.0f;
}

void NetplayMenu::RunAction(Node* from, const std::string& action)
{
    Node* root = UiRoot(from);
    NetplayMenu* menu = Find<NetplayMenu>(root, "NetplayMenu");
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

    Node* root = UiRoot(this);
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
}
