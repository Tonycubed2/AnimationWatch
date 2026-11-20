#pragma once

// Animation player (AnimationWatch 1.5.0): plays any .hkx animation file on the player, with the game's own Havok code.
// See Player.cpp for how it works.
namespace AnimPlayer
{
	void        Install();                                                // hook, at kDataLoaded
	void        Play(const std::string& a_dataPath, const std::string& a_label);  // a_dataPath: "meshes\actors\character\...\x.hkx"
	void        Stop();
	void        OnGameLoad();  // stop, and put the time multiplier back
	bool        IsBusy();

	// playback controls
	bool  IsPaused();
	void  SetPaused(bool a_paused);
	float Speed();
	void  SetSpeed(float a_speed);  // 0.05 to 3
	bool  Loop();
	void  SetLoop(bool a_loop);     // on: until Stop; off: once
	float Time();
	float Duration();
	void  Seek(float a_time);       // jump to a time (keeps playing unless paused)
	void  Restart();
	void  StepFrames(int a_frames); // pause and move by whole frames
	std::string Status();
}
