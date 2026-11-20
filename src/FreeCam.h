#pragma once

// Free camera and orbit view in the viewing room (AnimationWatch 1.8.9): fly around the clone, or let the camera
// circle it by itself, and watch the animation from any angle. See FreeCam.cpp for how it works.
namespace FreeCam
{
	void        Install();                    // input sink (kDataLoaded)
	void        RequestToggle();              // any thread: free camera on / off on the next frame
	void        RequestOrbit();               // any thread: orbit view on / off on the next frame
	void        RequestReset();               // any thread: camera off, first person facing the clone
	void        Off(const char* a_why);       // main thread: now (leaving the room, removing the clone)
	void        OnGameLoad();                 // forget the state without touching the camera
	void        Update(float a_realDelta);    // every frame from the animation player's tick (main thread)
	bool        IsOn();                       // free camera or orbit, on or being turned on
	bool        IsOrbiting();
	bool        ControlsShown();              // the on-screen camera controls are wanted
	void        RenderControls();             // the on-screen camera controls (inside an ImGui window being drawn)
	float       Speed();                      // units per second
	bool        InvertY();
	void        SetInvertY(bool a_invert);
	std::string KeyHelp();                    // one line for the on-screen key help
}
