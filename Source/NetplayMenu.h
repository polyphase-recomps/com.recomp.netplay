/**
 * @file NetplayMenu.h
 * @brief The in-game netplay menu's nodes (the scene: Tools > Recomp > UI > Generate Network Scene).
 *
 *  NetplayButton  A RecompButton (com.recomp.mod.base: menu style, gamepad selection) whose press
 *                 runs a netplay action (Action property):
 *                   host        host on the UDP port typed in the "Port" field (empty: default)
 *                   join        join the address typed in the "Address" field ("ip" or "ip:port")
 *                   search      look for sessions on the LAN (again: stop)
 *                   joinfound:N join the N-th session found on the LAN (0-based)
 *                   start       host: start the game (everyone ready)
 *                   leave       leave the session
 *                   delay-, delay+  the input delay (before the start; the host's counts)
 *                   close       close the menu
 *  NetplayMenu    Keeps the menu up to date every frame, by the names of its widgets: "Status",
 *                 "Players", "Note" (texts), "DelayValue" (text), "Search" (button label),
 *                 "Found_0".."Found_3" (buttons: the sessions on the LAN), "Start".
 *
 * Both work in packaged games (no editor needed); the generated scene only lays them out.
 */
#pragma once

#include "ModBaseWidgets.h" // com.recomp.mod.base: RecompButton

#include "Nodes/Widgets/Widget.h"

#include <string>

class NetplayButton : public RecompButton
{
public:
    DECLARE_NODE(NetplayButton, RecompButton);

    virtual void Activate() override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    void SetAction(const std::string& action);
    const std::string& GetAction() const;

protected:
    std::string mAction;
};

// SignalBus (the engine's global bus; Lua SignalBus.Emit / SignalBus.Subscribe):
//   "Netplay.Open"    in:  opens the netplay menu (in the Mods UI: the Mods UI on its Netplay tab).
//                          Optional argument: the name of the UI's root node ("Netplay",
//                          "ModSettings", ...) when a scene has several; else the first one.
//   "Netplay.Opened"  out: the netplay menu was shown
//   "Netplay.Closed"  out: it went away (closed, B, another Mods UI tab, or the session started:
//                          the menu closes itself then). Whoever opened it handles what's next.
//   Both out signals carry (UI name, session state: "idle", "lobby", "ready", "running", ...).
class NetplayMenu : public Widget
{
public:
    DECLARE_NODE(NetplayMenu, Widget);

    virtual void Start() override;
    virtual void Stop() override;
    virtual void Destroy() override;
    virtual void Tick(float deltaTime) override;

    // Runs a NetplayButton action for the menu `from` is in.
    static void RunAction(Node* from, const std::string& action);

    // The menus in the running scenes: open (the one whose UI root has that name, else the
    // first) / close / query, and the addon's per-frame check that sends Opened / Closed.
    static bool OpenAny(const std::string& uiName = "");
    static bool CloseAny();
    static bool AnyOpen();
    static void TickAll();

    bool OpenMenu();
    void CloseMenu();
    bool IsMenuOpen();
    std::string UiName();

    static const int kFoundSlots = 4;

protected:
    void SetNote(const std::string& note);
    Node* ModsPage();

    std::string mNote;
    float mNoteTime = 0.0f;
    bool mSearching = false;
    bool mWasOpen = false;
};
