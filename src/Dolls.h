#pragma once

// Animation dolls (AnimationWatch 1.3.0): one page per activity showing which animation mod wins each
// animation slot, with conflicts in yellow, problems in red, and a way to pick the winner.
namespace Dolls
{
	void StartScan();                                                                       // reads every OAR / DAR pack on disk (background thread)
	void OnPlayed(const std::string& a_graph, const std::string& a_path, const std::string& a_source);  // an animation started on the player
	void MarkFlipFlop(const std::string& a_graph, const std::string& a_path);              // that slot flip-flopped
	void __stdcall Render();                                                                // F1 page
}
