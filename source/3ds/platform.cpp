// ftpd is a server implementation based on the following:
// - RFC  959 (https://tools.ietf.org/html/rfc959)
// - RFC 3659 (https://tools.ietf.org/html/rfc3659)
// - suggested implementation details from https://cr.yp.to/ftp/filesystem.html
// - Deflate transmission mode for FTP
//   (https://tools.ietf.org/html/draft-preston-ftpext-deflate-04)
//
// Copyright (C) 2024 Michael Theall
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "platform.h"

#include "fs.h"
#include "ftpServer.h"
#include "log.h"

#include "imgui_citro3d.h"
#include "imgui_ctru.h"

#include <citro3d.h>
#include <tex3ds.h>

#ifndef CLASSIC
#include <imgui.h>

#include "gfx.h"
#endif

#include <arpa/inet.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <span>
#include <vector>

#ifdef CLASSIC
PrintConsole g_statusConsole;
PrintConsole g_logConsole;
PrintConsole g_sessionConsole;
#endif

namespace
{
/// \brief Thread stack size
constexpr auto STACK_SIZE = 0x8000;
/// \brief soc:u buffer alignment
constexpr auto SOCU_ALIGN = 0x1000;
/// \brief soc:u buffer size
constexpr auto SOCU_BUFFERSIZE = 0x100000;

static_assert (SOCU_BUFFERSIZE % SOCU_ALIGN == 0);

/// \brief Whether ndm:u is locked
bool s_ndmuLocked = false;

/// \brief Whether soc:u is active
std::atomic_bool s_socuActive = false;
/// \brief soc:u buffer
u32 *s_socuBuffer = nullptr;
/// \brief ac:u fence
platform::Mutex s_acuFence;

/// \brief Whether to power backlight
bool s_backlight = true;
/// \brief Button state for toggling backlight
unsigned s_buttons = 0;

/// \brief APT hook cookie
aptHookCookie s_aptHookCookie;

#ifdef CLASSIC
/// \brief Host address
in_addr_t s_addr = 0;
#else
/// \brief Screen width
constexpr auto SCREEN_WIDTH = 400.0f;
/// \brief Screen height
constexpr auto SCREEN_HEIGHT = 480.0f;

#if ANTI_ALIAS
/// \brief Display transfer scaling
constexpr auto TRANSFER_SCALING = GX_TRANSFER_SCALE_XY;
/// \brief Framebuffer scale
constexpr auto FB_SCALE = 2.0f;
#else
/// \brief Display transfer scaling
constexpr auto TRANSFER_SCALING = GX_TRANSFER_SCALE_NO;
/// \brief Framebuffer scale
constexpr auto FB_SCALE = 1.0f;
#endif

/// \brief Framebuffer width
constexpr auto FB_WIDTH = SCREEN_WIDTH * FB_SCALE;
/// \brief Framebuffer height
constexpr auto FB_HEIGHT = SCREEN_HEIGHT * FB_SCALE;

/// \brief Display transfer flags
constexpr auto DISPLAY_TRANSFER_FLAGS =
    GX_TRANSFER_FLIP_VERT (0) | GX_TRANSFER_OUT_TILED (0) | GX_TRANSFER_RAW_COPY (0) |
    GX_TRANSFER_IN_FORMAT (GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT (GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING (TRANSFER_SCALING);

/// \brief Top left screen render target
C3D_RenderTarget *s_topLeft = nullptr;
/// \brief Top right screen render target
C3D_RenderTarget *s_topRight = nullptr;
/// \brief Bottom screen render target
C3D_RenderTarget *s_bottom = nullptr;
/// \brief Depth/Stencil buffer
void *s_depthStencil = nullptr;

/// \brief Texture atlas
C3D_Tex s_gfxTexture;
/// \brief Texture atlas metadata
Tex3DS_Texture s_gfxT3x;

/// \brief Performance timer
TickCounter s_timer;
#endif

/// \brief Enable backlight
/// \param enable_ Whether to enable backligh
void enableBacklight (bool const enable_)
{
	if (R_FAILED (gspLcdInit ()))
		return;

	(enable_ ? GSPLCD_PowerOnBacklight : GSPLCD_PowerOffBacklight) (GSPLCD_SCREEN_BOTH);

	gspLcdExit ();
}

/// \brief Handle APT cookie
/// \param type_ Hook type
/// \param param_ User param
void handleAPTHook (APT_HookType const type_, void *const param_)
{
	(void)param_;

	switch (type_)
	{
	case APTHOOK_ONSUSPEND:
	case APTHOOK_ONSLEEP:
		// turn on backlight, or you can't see the home menu!
		if (!s_backlight)
			enableBacklight (true);
		break;

	case APTHOOK_ONRESTORE:
	case APTHOOK_ONWAKEUP:
		// restore backlight setting
		enableBacklight (s_backlight);
		break;

	default:
		break;
	}
}

/// \brief Get network visibility
bool getNetworkVisibility ()
{
	// serialize ac:u access from multiple threads
	auto const lock = std::scoped_lock (s_acuFence);

	// get wifi status
	static std::uint32_t lastWifi = 0;
	static Result lastResult      = 0;

	std::uint32_t wifi = 0;
	auto const result  = ACU_GetWifiStatus (&wifi);
	if (result != lastResult)
		info ("ACU_GetWifiStatus: result 0x%lx -> 0x%lx\n", lastResult, result);
	lastResult = result;

	if (R_SUCCEEDED (result))
	{
		if (wifi != lastWifi)
			info ("ACU_GetWifiStatus: wifi 0x%lx -> 0x%lx\n", lastWifi, wifi);
		lastWifi = wifi;
	}

	if (R_FAILED (result) || !wifi)
	{
#ifdef CLASSIC
		s_addr = 0;
#endif
		return false;
	}

#ifdef CLASSIC
	if (!s_addr)
		s_addr = gethostid ();

	if (s_addr == INADDR_BROADCAST)
		s_addr = 0;
#endif

	return true;
}

/// \brief Start network
void startNetwork ()
{
	// check if already active
	if (s_socuActive)
		return;

	if (!getNetworkVisibility ())
		return;

	// allocate soc:u buffer
	if (!s_socuBuffer)
		s_socuBuffer = static_cast<u32 *> (::memalign (SOCU_ALIGN, SOCU_BUFFERSIZE));

	if (!s_socuBuffer)
		return;

	// initialize soc:u service
	if (R_FAILED (socInit (s_socuBuffer, SOCU_BUFFERSIZE)))
		return;

	aptSetSleepAllowed (false);

	Result res;
	if (R_FAILED (res = NDMU_EnterExclusiveState (NDM_EXCLUSIVE_STATE_INFRASTRUCTURE)))
		error ("Failed to enter exclusive NDM state: 0x%lx\n", res);
	else if (R_FAILED (res = NDMU_LockState ()))
	{
		error ("Failed to lock NDM: 0x%lx\n", res);
		NDMU_LeaveExclusiveState ();
	}
	else
		s_ndmuLocked = true;

	s_socuActive = true;
	info ("Wifi connected\n");
	return;
}

/// \brief Draw citro3d logo
void drawLogo ()
{
#ifndef CLASSIC
	// get citro3d logo subtexture
	auto subTex = Tex3DS_GetSubTexture (s_gfxT3x, gfx_c3dlogo_idx);

	// get framebuffer metrics
	auto const &io          = ImGui::GetIO ();
	auto const screenWidth  = io.DisplaySize.x;
	auto const screenHeight = io.DisplaySize.y;
	auto const logoWidth    = subTex->width;
	auto const logoHeight   = subTex->height;

	// calculate top screen coords
	auto const x1 = (screenWidth - logoWidth) * 0.5f;
	auto const x2 = x1 + logoWidth;
	auto const y1 = (screenHeight * 0.5f - logoHeight) * 0.5f;
	auto const y2 = y1 + logoHeight;

	// calculate uv coords
	auto const uv1 = ImVec2 (subTex->left, subTex->top);
	auto const uv2 = ImVec2 (subTex->right, subTex->bottom);

	auto const drawList = ImGui::GetBackgroundDrawList ();

	// draw to top screen
	drawList->AddCallback (&imgui::citro3d::setZ, std::bit_cast<void *> (-5.0f));
	drawList->AddImage (&s_gfxTexture, ImVec2 (x1, y1), ImVec2 (x2, y2), uv1, uv2);
	drawList->AddCallback (&imgui::citro3d::setZ, std::bit_cast<void *> (0.0f));

	// draw to bottom screen
	drawList->AddImage (&s_gfxTexture,
	    ImVec2 (x1, y1 + screenHeight * 0.5f),
	    ImVec2 (x2, y2 + screenHeight * 0.5f),
	    uv1,
	    uv2);
#endif
}

#ifndef CLASSIC
struct Bubble
{
	float x;
	float y;
	float z;
	float scale;
	float dy;
};

std::vector<Bubble> &getBubbles ()
{
	static std::vector<Bubble> bubbles;

	if (!bubbles.empty ())
		return bubbles;

	auto eng  = std::default_random_engine (std::random_device{}());
	auto dist = std::uniform_real_distribution<float> (0.0f, 1.0f);

	constexpr auto COUNT = 250;

	bubbles.reserve (COUNT);
	for (unsigned i = 0; i < COUNT; ++i)
	{
		auto &bubble = bubbles.emplace_back ();

		bubble.x     = 500.0f * dist (eng) - 50.0f;
		bubble.y     = 240.0f * dist (eng);
		bubble.z     = std::floor (-5.0f * dist (eng));
		bubble.scale = std::max (dist (eng) / 8.0f, 0.0625f);
		bubble.dy    = 20.0f * std::max (1.5f * dist (eng), 0.25f);
	}

	std::ranges::sort (bubbles, {}, [] (auto const &bubble_) { return bubble_.z; });

	return bubbles;
}
#endif

#ifndef CLASSIC
void drawBubbles (ImDrawList *const drawList_, std::span<Bubble> const bubbles_, float const dt_)
{
	auto const &io = ImGui::GetIO ();

	auto const screenHeight = io.DisplaySize.y / 2.0f;

	auto const tex = Tex3DS_GetSubTexture (s_gfxT3x, gfx_bubble_idx);

	auto const uv1 = ImVec2 (tex->left, tex->top);
	auto const uv2 = ImVec2 (tex->right, tex->bottom);

	float lastZ = 0.0f;

	for (auto &bubble : bubbles_)
	{
		if (bubble.z != lastZ)
		{
			lastZ = bubble.z;
			drawList_->AddCallback (&imgui::citro3d::setZ, std::bit_cast<void *> (lastZ));
		}

		bubble.y -= dt_ * bubble.dy;

		if (bubble.y < 0.0f)
			bubble.y = screenHeight;

		auto const width  = bubble.scale * tex->width;
		auto const height = bubble.scale * tex->height;

		auto const p1 = ImVec2 (
		    bubble.x + 100.0f * bubble.scale * std::sin (bubble.z + bubble.y / 40.0f), bubble.y);
		auto const p2 = ImVec2 (p1.x + width, p1.y + height);

		drawList_->AddImage (&s_gfxTexture, p1, p2, uv1, uv2);
	}

	if (lastZ != 0.0f)
		drawList_->AddCallback (&imgui::citro3d::setZ, std::bit_cast<void *> (0.0f));
}
#endif

void drawBubbles ()
{
#ifndef CLASSIC
	osTickCounterUpdate (&s_timer);
	auto const ticks = osTickCounterRead (&s_timer);
	osTickCounterStart (&s_timer);

	// only draw in stereoscopy
	if (!osGet3DSliderState ())
		return;

	auto const dt = ticks / 1000.0f;

	auto &bubbles = getBubbles ();

	auto const split = std::ranges::upper_bound (
	    bubbles, 0.0f, {}, [] (auto const &bubble_) { return bubble_.z; });

	auto const back  = std::span (std::begin (bubbles), split);
	auto const front = std::span (split, std::end (bubbles));

	drawBubbles (ImGui::GetBackgroundDrawList (), back, dt);
	drawBubbles (ImGui::GetForegroundDrawList (), front, dt);
#endif
}

/// \brief Draw status
void drawStatus ()
{
#ifndef CLASSIC
	constexpr unsigned batteryLevels[] = {
	    gfx_battery0_idx,
	    gfx_battery0_idx,
	    gfx_battery1_idx,
	    gfx_battery2_idx,
	    gfx_battery3_idx,
	    gfx_battery4_idx,
	};

	constexpr unsigned wifiLevels[] = {
	    gfx_wifiNull_idx,
	    gfx_wifi1_idx,
	    gfx_wifi2_idx,
	    gfx_wifi3_idx,
	};

	// get battery charging state or level
	static u8 charging = 0;
	static u8 level    = 5;
	PTMU_GetBatteryChargeState (&charging);
	if (!charging)
	{
		PTMU_GetBatteryLevel (&level);
		if (level >= std::extent_v<decltype (batteryLevels)>)
			svcBreak (USERBREAK_PANIC);
	}

	auto const &io    = ImGui::GetIO ();
	auto const &style = ImGui::GetStyle ();

	auto const screenWidth = io.DisplaySize.x;

	// calculate battery icon metrics
	auto const battery =
	    Tex3DS_GetSubTexture (s_gfxT3x, charging ? gfx_batteryCharge_idx : batteryLevels[level]);
	auto const batteryWidth  = battery->width;
	auto const batteryHeight = battery->height;

	// calculate battery icon position
	auto const p1 = ImVec2 (screenWidth - batteryWidth, 0.0f);
	auto const p2 = ImVec2 (p1.x + batteryWidth, p1.y + batteryHeight);

	// calculate battery icon uv coords
	auto const uv1 = ImVec2 (battery->left, battery->top);
	auto const uv2 = ImVec2 (battery->right, battery->bottom);

	// draw battery icon
	ImGui::GetForegroundDrawList ()->AddImage (
	    &s_gfxTexture, p1, p2, uv1, uv2, ImGui::GetColorU32 (ImGuiCol_Text));

	// get wifi strength
	auto const wifiStrength = osGetWifiStrength ();

	// calculate wifi icon metrics
	auto const wifi       = Tex3DS_GetSubTexture (s_gfxT3x, wifiLevels[wifiStrength]);
	auto const wifiWidth  = wifi->width;
	auto const wifiHeight = wifi->height;

	// calculate wifi icon position
	auto const p3 = ImVec2 (p1.x - wifiWidth - style.FramePadding.x, 0.0f);
	auto const p4 = ImVec2 (p3.x + wifiWidth, p3.y + wifiHeight);

	// calculate wifi icon uv coords
	auto const uv3 = ImVec2 (wifi->left, wifi->top);
	auto const uv4 = ImVec2 (wifi->right, wifi->bottom);

	// draw wifi icon
	ImGui::GetForegroundDrawList ()->AddImage (
	    &s_gfxTexture, p3, p4, uv3, uv4, ImGui::GetColorU32 (ImGuiCol_Text));

	// draw current timestamp
	char timeBuffer[16];
	auto const now = std::time (nullptr);
	std::strftime (timeBuffer, sizeof (timeBuffer), "%H:%M:%S", std::localtime (&now));

	// draw free space
	char buffer[64];
	auto const freeSpace = FtpServer::getFreeSpace ();

	std::snprintf (
	    buffer, sizeof (buffer), "%s %s", timeBuffer, freeSpace.empty () ? "" : freeSpace.c_str ());

	auto const size = ImGui::CalcTextSize (buffer);
	auto const p5   = ImVec2 (p3.x - size.x - style.FramePadding.x, style.FramePadding.y);
	ImGui::GetForegroundDrawList ()->AddText (p5, ImGui::GetColorU32 (ImGuiCol_Text), buffer);
#endif
}
}

bool platform::init ()
{
	// enable New 3DS speedup
	osSetSpeedupEnable (true);

	acInit ();
	ndmuInit ();
	ptmuInit ();
#ifndef CLASSIC
	romfsInit ();
#endif
	gfxInit (GSP_BGR8_OES, GSP_BGR8_OES, false);

#ifdef CLASSIC
	gfxSet3D (false);

	consoleInit (GFX_TOP, &g_statusConsole);
	consoleInit (GFX_TOP, &g_logConsole);
	consoleInit (GFX_BOTTOM, &g_sessionConsole);

	consoleSetWindow (&g_statusConsole, 0, 0, 50, 1);
	consoleSetWindow (&g_logConsole, 0, 1, 50, 29);
	consoleSetWindow (&g_sessionConsole, 0, 0, 40, 30);
#else
	gfxSet3D (true);
#endif

#ifndef NDEBUG
	consoleDebugInit (debugDevice_SVC);
	std::setvbuf (stderr, nullptr, _IOLBF, 0);
#endif

	aptHook (&s_aptHookCookie, handleAPTHook, nullptr);

#ifndef CLASSIC
	// initialize citro3d
	C3D_Init (4 * C3D_DEFAULT_CMDBUF_SIZE);

	// create top left screen render target
	s_topLeft = C3D_RenderTargetCreate (FB_HEIGHT * 0.5f, FB_WIDTH, GPU_RB_RGBA8, -1);
	C3D_RenderTargetSetOutput (s_topLeft, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	// create top right screen render target
	s_topRight = C3D_RenderTargetCreate (FB_HEIGHT * 0.5f, FB_WIDTH, GPU_RB_RGBA8, -1);
	C3D_RenderTargetSetOutput (s_topRight, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

	// create bottom screen render target
	s_bottom = C3D_RenderTargetCreate (FB_HEIGHT * 0.5f, FB_WIDTH * 0.8f, GPU_RB_RGBA8, -1);
	C3D_RenderTargetSetOutput (s_bottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	// create and attach depth/stencil buffer
	{
		auto const size =
		    C3D_CalcDepthBufSize (FB_HEIGHT * 0.5f, FB_WIDTH, GPU_RB_DEPTH24_STENCIL8);
		s_depthStencil = vramAlloc (size);
		C3D_FrameBufDepth (&s_topLeft->frameBuf, s_depthStencil, GPU_RB_DEPTH24_STENCIL8);
		C3D_FrameBufDepth (&s_topRight->frameBuf, s_depthStencil, GPU_RB_DEPTH24_STENCIL8);
		C3D_FrameBufDepth (&s_bottom->frameBuf, s_depthStencil, GPU_RB_DEPTH24_STENCIL8);
	}

	if (!imgui::ctru::init ())
		return false;

	imgui::citro3d::init ();

	{
		// load texture atlas
		fs::File file;
		if (!file.open ("romfs:/gfx.t3x"))
			svcBreak (USERBREAK_PANIC);

		s_gfxT3x = Tex3DS_TextureImportStdio (file, &s_gfxTexture, nullptr, false);
		if (!s_gfxT3x)
			svcBreak (USERBREAK_PANIC);

		C3D_TexSetFilter (&s_gfxTexture, GPU_LINEAR, GPU_LINEAR);
	}

	auto &io    = ImGui::GetIO ();
	auto &style = ImGui::GetStyle ();

	// disable imgui.ini file
	io.IniFilename = nullptr;

	// citro3d logo doesn't quite show with the default transparency
	style.Colors[ImGuiCol_WindowBg].w = 0.8f;
	style.ScaleAllSizes (0.5f);
#endif

	return true;
}

bool platform::networkVisible ()
{
	// check if soc:u is active
	if (!s_socuActive)
		return false;

	return getNetworkVisibility ();
}

bool platform::networkAddress (SockAddr &addr_)
{
	sockaddr_in addr;
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = gethostid ();

	addr_ = addr;
	return true;
}

std::string const &platform::hostname ()
{
	static std::string const hostname = "3ds-ftpd";
	return hostname;
}

bool platform::loop ()
{
	if (!aptMainLoop ())
		return false;

	startNetwork ();

	hidScanInput ();

	auto const kDown = hidKeysDown ();
	auto const kHeld = hidKeysHeld ();
	auto const kUp   = hidKeysUp ();

	// check if the user wants to exit
	if (kDown & KEY_START)
		return false;

	// check if the user wants to toggle the backlight
	// avoid toggling during the Rosalina menu default combo
	if (kDown == KEY_SELECT && kHeld == KEY_SELECT)
	{
		// SELECT was pressed and no other keys are held, so reset state
		s_buttons = KEY_SELECT;
	}
	else if (kUp & KEY_SELECT)
	{
		// SELECT was released
		if (s_buttons == KEY_SELECT)
		{
			// no other button was held at the same time as SELECT, so toggle
			s_backlight = !s_backlight;
			enableBacklight (s_backlight);
		}
	}
	else
	{
		// add any held buttons
		s_buttons |= kHeld;
	}

#ifndef CLASSIC
	auto &io = ImGui::GetIO ();

	// setup display metrics
	io.DisplaySize             = ImVec2 (SCREEN_WIDTH, SCREEN_HEIGHT);
	io.DisplayFramebufferScale = ImVec2 (FB_SCALE, FB_SCALE);

	imgui::ctru::newFrame ();
	ImGui::NewFrame ();
#endif

	return true;
}

void platform::render ()
{
	drawLogo ();
	drawBubbles ();
	drawStatus ();

#ifdef CLASSIC
	gfxFlushBuffers ();
	gspWaitForVBlank ();
	gfxSwapBuffers ();
#else
	ImGui::Render ();

	C3D_FrameBegin (C3D_FRAME_SYNCDRAW);

	imgui::citro3d::render (s_topLeft, s_topRight, s_bottom);

	C3D_FrameEnd (0);
#endif
}

void platform::exit ()
{
#ifndef CLASSIC
	imgui::citro3d::exit ();

	// free graphics
	Tex3DS_TextureFree (s_gfxT3x);
	C3D_TexDelete (&s_gfxTexture);

	// free render targets
	C3D_RenderTargetDelete (s_bottom);
	C3D_RenderTargetDelete (s_topRight);
	C3D_RenderTargetDelete (s_topLeft);

	// free depth/stencil buffer
	vramFree (s_depthStencil);

	// deinitialize citro3d
	C3D_Fini ();
#endif

	if (s_ndmuLocked)
	{
		NDMU_UnlockState ();
		NDMU_LeaveExclusiveState ();
		aptSetSleepAllowed (true);
		s_ndmuLocked = false;
	}

	if (s_socuActive)
	{
		socExit ();
		s_socuActive = false;
	}

	std::free (s_socuBuffer);

	aptUnhook (&s_aptHookCookie);

	// turn backlight back on
	if (!s_backlight)
		enableBacklight (true);

	gfxExit ();
#ifndef CLASSIC
	romfsExit ();
#endif
	ptmuExit ();
	ndmuExit ();
	acExit ();
}

///////////////////////////////////////////////////////////////////////////
platform::steady_clock::time_point platform::steady_clock::now () noexcept
{
	return time_point (duration (svcGetSystemTick ()));
}

///////////////////////////////////////////////////////////////////////////
/// \brief Platform thread pimpl
class platform::Thread::privateData_t
{
public:
	privateData_t ()
	{
		if (thread)
			threadFree (thread);
	}

	/// \brief Parameterized constructor
	/// \param func_ Thread entry point
	privateData_t (std::function<void ()> &&func_) : thread (nullptr), func (std::move (func_))
	{
		// use next-lower priority
		s32 priority = 0x30;
		svcGetThreadPriority (&priority, CUR_THREAD_HANDLE);
		priority = std::clamp<s32> (priority, 0x18, 0x3F - 1) + 1;

		// use appcore
		thread = threadCreate (&privateData_t::threadFunc, this, STACK_SIZE, priority, 0, false);
		assert (thread);
	}

	/// \brief Underlying thread entry point
	/// \param arg_ Thread pimpl object
	static void threadFunc (void *const arg_)
	{
		// call passed-in entry point
		auto const t = static_cast<privateData_t *> (arg_);
		t->func ();
	}

	/// \brief Underlying thread
	::Thread thread = nullptr;
	/// \brief Thread entry point
	std::function<void ()> func;
};

///////////////////////////////////////////////////////////////////////////
platform::Thread::~Thread () = default;

platform::Thread::Thread () : m_d (new privateData_t ())
{
}

platform::Thread::Thread (std::function<void ()> &&func_)
    : m_d (new privateData_t (std::move (func_)))
{
}

platform::Thread::Thread (Thread &&that_) : m_d (new privateData_t ())
{
	std::swap (m_d, that_.m_d);
}

platform::Thread &platform::Thread::operator= (Thread &&that_)
{
	std::swap (m_d, that_.m_d);
	return *this;
}

void platform::Thread::join ()
{
	threadJoin (m_d->thread, UINT64_MAX);
}

void platform::Thread::sleep (std::chrono::milliseconds const timeout_)
{
	svcSleepThread (std::chrono::nanoseconds (timeout_).count ());
}

///////////////////////////////////////////////////////////////////////////
/// \brief Platform mutex pimpl
class platform::Mutex::privateData_t
{
public:
	/// \brief Underlying mutex
	LightLock mutex;
};

///////////////////////////////////////////////////////////////////////////
platform::Mutex::~Mutex () = default;

platform::Mutex::Mutex () : m_d (new privateData_t ())
{
	LightLock_Init (&m_d->mutex);
}

void platform::Mutex::lock ()
{
	LightLock_Lock (&m_d->mutex);
}

void platform::Mutex::unlock ()
{
	LightLock_Unlock (&m_d->mutex);
}
