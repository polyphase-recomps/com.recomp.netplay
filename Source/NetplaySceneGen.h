/**
 * @file NetplaySceneGen.h
 * @brief Tools > Recomp > UI > Generate Network Scene (editor only): the in-game netplay menu as
 *        a scene, Assets/Scenes/<name>.oct, laid out with com.recomp.mod.base's UI builder:
 *
 *   Netplay (Canvas)                   stays visible (hidden widgets don't tick)
 *     Panel (Quad)                     shown / hidden by the controller, fitted to the screen
 *       Scroll (ScrollContainer) > Layout (ArrayWidget, column)
 *         Title, Status, Players, Note
 *         HostRow    [Host a session] [Port]
 *         JoinRow    [Address .......] [Join]
 *         DelayRow   Input delay [-] 2 [+]
 *         Search     [Search the LAN]
 *         Found      [Found_0] .. [Found_3]   the sessions found
 *         Actions    [Start] [Leave] [Close]
 *     MenuController (RecompMenuController)  gamepad navigation, opened by its toggle button /
 *                                            the HOME menu, B closes
 *     NetplayMenu                            keeps it up to date (NetplayMenu.h)
 *
 * Running it again updates the scene without touching what is already there (nodes matched
 * by name): only missing parts are added. The game's Menu Style (Mod Map) is applied when
 * the game has one. Put the scene in the game's scene (or the launcher) to have the menu.
 */
#pragma once

#include <cstdint>

#if EDITOR
struct EditorUIHooks;

namespace NetplaySceneGen
{
void Register(EditorUIHooks* hooks, uint64_t hookId);
}
#endif
