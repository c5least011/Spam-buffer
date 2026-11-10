# SpamBuffer
A mod that make you become a god spammer

<img src="logo.png" width="150" alt="the mod's logo" />

## Getting started
We recommend heading over to [the getting started section on our docs](https://docs.geode-sdk.org/getting-started/) for useful info on what to do next.

## Build instructions
For more info, see [our docs](https://docs.geode-sdk.org/getting-started/create-mod#build)
```sh
# Assuming you have the Geode CLI set up already
geode build
```

# Resources
* [Geode SDK Documentation](https://docs.geode-sdk.org/)
* [Geode SDK Source Code](https://github.com/geode-sdk/geode/)
* [Geode CLI](https://github.com/geode-sdk/cli)
* [Bindings](https://github.com/geode-sdk/bindings/)
* [Dev Tools](https://github.com/geode-sdk/DevTools)


## Frame Perfect behavior
- Activation threshold: `minimum input CPS = target CPS - CPS below target allowed`.
- The minimum-CPS setting is checked only when activating.
- Once active, the mod keeps output at the configured target CPS, regardless of the measured CPS.
- It does not inspect wave parts, structures, collisions, or player path. A collision/death does not stop it. It stops only after physical spam input goes quiet, the feature is disabled, or the level is explicitly reset/paused/exited.


## Dual-side input and in-game menu
- P1 and P2 use separate input histories and assist state, so left-side and right-side controls do not overwrite each other in dual mode. In normal single-player mode, Geometry Dash still routes both touch areas to its single active player.
- The gameplay overlay provides a quick `FP ON/OFF` switch and an `SB` button that opens the dedicated Spam Buffer popup.
- The popup controls Wave, Frame Perfect, Orb, HUD, target CPS, allowed CPS shortfall, TPS, Wave delay / Wave look, trajectory preview, and Orb look.
- Configuration is stored through Geode saved values. No configurable entries are exposed in the Geode mod Settings page.
- Windows toggles Frame Perfect with Left Shift while a level is active.


## Wave spam (v1.1.0)
Wave spam is now the ghost-player buffer from the Predictive Input Buffer mod (`Engine.hpp`), not a click-rhythm heuristic.
- A press/release that would kill the wave is held back a few physics ticks only when a hidden ghost player shows that doing it a little later survives. Otherwise it passes through untouched. It never invents clicks.
- `Wave delay` (0-8): maximum ticks an event may be held back. 0 turns the buffer off.
- `Wave look` (13-18): ticks the ghost must survive.
- `Wave imminent` is fixed at 8.
- Held-back events keep their order and spacing. Delays are counted in physics ticks (the engine's own clock, as in the input buffer mod) and the due press / release is fired at the start of the tick, before the player moves, one event per tick and lane. Frame Perfect has priority and clears the queue when it takes over.
- With TRAJECTORY on, a line shows the buffer status (`ready`, number of searches / delayed events, last verdict).
- `TRAJ` draws the ghost's "if you hold" / "if you release" paths (display only).

## Menu
- A small round `SB` icon in the pause menu and the main menu opens the mod menu. Nothing is drawn on screen while playing, except the CPS window and the watermark.
- The menu is a small rounded window. Drag it by its title bar.
- Three pills pick the mode (Orb is shown as "ORB SOON" and is not active yet; only one mode can be on at a time) and show that mode's settings below; values are typed into text boxes (Wave: delay 0-8, look 13-18; Frame Perfect: target CPS, CPS below, game TPS; Orb: look 10-120).
- Windows: Left Shift (in a level) only shows / hides the CPS window.
- While any mode is on, a small gold "Spam Buffer by C5least" (35% opacity) sits bottom centre.

## Inputs
Every event keeps its own flag and its own lane: an event flagged player 2 is a player-2 event and is never re-tagged as player 1 (dual or no dual). Mod-made presses use the lane's own flag too. With no dual only player 1 exists, so the player-2 lane simply reads the same player (mode / orb zone / ghost) as the player-1 lane.
With TRAJECTORY on, the CPS window also shows the layout and raw input counters (`L` = player-1 flagged presses/releases, `R` = player-2 flagged, forwarded, injected) for debugging.

## Orb assist (blue orbs): coming soon
The menu keeps its place (grey "ORB SOON" pill and tab), but the feature is switched off for now: it never runs, nothing is scanned and no ghost is spent on it. The code (`OrbGuard.hpp`, `Engine::sbOrbRescue`) stays in the project, dormant.
