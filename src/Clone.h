#pragma once

// The preview clone (AnimationWatch 1.7.0): a temporary copy of the player standing in front of the player, which
// plays the animations clicked on the Animation Dolls page. Based on the clone in SkyAnimViewer's source.
namespace Clone
{
	void        Install();             // Esc in the viewing room (kDataLoaded)
	void        RequestCreate();       // queued to the game's main thread (SKSE task)
	void        RequestRemove();
	void        RequestMoveToFront();
	RE::Actor*  Get();                 // nullptr when there is none
	bool        Ready();
	bool        ViewReady();           // 1.8.7: dressed, faded in, first person in the room (or 5 s passed): the world may stand still
	bool        InViewingRoom();      // the player is in the viewing room               // exists, its 3D is loaded and the gear is on
	void        OnFrame();             // every frame from the player's update (main thread)
	void        OnGameLoad();          // removes clones left in a save, returns from the viewing room
	void        InstallSerialization();  // co-save record for the way back from the viewing room
	std::string Status();
}
