// SPDX-License-Identifier: Apache-2.0
// The host services a Gea program calls outside the UI: animation frames,
// the frame rate, and the display settings other boards expose.
//
// Animation frames ride the shell's frame timer: a request arms it, and each
// tick runs the callbacks queued before it, the way a browser does. The rate
// is the program's (`Display.setFrameRate`), bounded by what a Pebble panel
// can show. Display tuning a Pebble has no hardware for -- vsync, flush
// chunking, e-paper waveforms, rotation -- is accepted and does nothing.
#include "pebble_host.h"

#include "host/backends.h"
#include "host/timers.h"
#include "services/frame_scheduler.h"

#include <string>
#include <utility>
#include <vector>

namespace {

// The panel refreshes at most about 30 times a second; asking the timer for
// more only burns battery on frames nobody sees.
constexpr int kMinFrameIntervalMs = 33;

int intervalMs = kMinFrameIntervalMs;
double nextFrameId = 1;

struct Pending {
	double id;
	gea::host::AnimationFrameCallback callback;
};

std::vector<Pending> &queued()
{
	static std::vector<Pending> list;
	return list;
}

}  // namespace

// The program's cycle-collection host contract (emitted with every program):
// a host brackets its callbacks, and closing the bracket is a collection
// safepoint. Allocation is the only other one, and a frame that only moves
// numbers allocates nothing -- while every reactive read still queues its
// cell as a candidate, so without this the candidate list grew every frame
// until the heap ran out.
extern "C" void gea_cycle_collection_defer_begin();
extern "C" void gea_cycle_collection_defer_end();

extern "C" bool gea_pebble_run_animation_frames(uint32_t now_ms)
{
	if (queued().empty()) return false;
	// Callbacks queued while these run wait for the next frame.
	std::vector<Pending> due;
	due.swap(queued());
	gea_cycle_collection_defer_begin();
	for (Pending &entry : due) entry.callback(static_cast<gea::host::AnimationFrameTimestamp>(now_ms));
	gea_cycle_collection_defer_end();
	if (!queued().empty()) gea_pebble_request_frame(intervalMs);
	return true;
}

namespace gea::host {

double requestAnimationFrame(AnimationFrameCallback callback)
{
	const double id = nextFrameId++;
	if (queued().empty()) gea_pebble_request_frame(intervalMs);
	queued().push_back({id, std::move(callback)});
	return id;
}

}  // namespace gea::host

namespace gea::framework::services {

void FrameScheduler::setFrameIntervalMs(int interval_ms)
{
	intervalMs = interval_ms < kMinFrameIntervalMs ? kMinFrameIntervalMs : interval_ms > kMaxFrameIntervalMs ? kMaxFrameIntervalMs : interval_ms;
}

void FrameScheduler::setFrameRate(double fps)
{
	if (fps > 0) setFrameIntervalMs(static_cast<int>(1000 / fps));
}

int FrameScheduler::frameIntervalMs() { return intervalMs; }
double FrameScheduler::frameRate() { return 1000.0 / intervalMs; }

}  // namespace gea::framework::services

namespace gea::framework::display {

double DisplayBackend::brightness() { return 1; }
void DisplayBackend::setBrightness(double) {}
void DisplayBackend::setAA(double) {}
void DisplayBackend::setFlushConfig(double, double) {}
void DisplayBackend::setEpaperRefreshConfig(double, double, double, const std::vector<std::uint8_t> &, bool, const std::vector<std::uint8_t> &, bool) {}
void DisplayBackend::epaperFullRefresh() {}
void DisplayBackend::setEpaperGrayscale(bool) {}
void DisplayBackend::setEpaperFullOnCover(bool) {}
void DisplayBackend::setVSync(bool) {}
void DisplayBackend::setTextRasterCache(bool) {}
void DisplayBackend::setTextSolidBackdrop(int) {}
void DisplayBackend::invalidate() { gea_pebble_request_redraw(); }
std::string DisplayBackend::orientation() { return "portrait"; }
void DisplayBackend::setOrientation(const std::string &) {}
std::vector<std::string> DisplayBackend::supportedOrientations() { return {"portrait"}; }
void DisplayBackend::setSupportedOrientations(const std::vector<std::string> &) {}
void DisplayBackend::setSupportedOrientations(const std::string &) {}
bool DisplayBackend::autoRotate() { return false; }
void DisplayBackend::setAutoRotate(bool) {}
std::string DisplayBackend::pixelFormat() { return "argb2222"; }
void DisplayBackend::setPixelFormat(const std::string &) {}
std::string DisplayBackend::panelPixelFormat() { return "argb2222"; }
std::vector<std::string> DisplayBackend::supportedPixelFormats() { return {"argb2222"}; }
void DisplayBackend::updateAutoRotationFromAccelerometer() {}
void DisplayBackend::updateAutoRotation(double, double, double) {}

}  // namespace gea::framework::display
