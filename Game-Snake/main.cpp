#define NOMINMAX
#define OLC_PGE_APPLICATION
#define OLC_PGEX_QUICKGUI

#include <Windows.h>

#include "olcPixelGameEngine.h"
#include "olcPGEX_QuickGUI.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// Design overview:
// - The game is state-driven so each screen owns its own input/update/draw behavior.
// - Profiles and leaderboard entries share one backing record to keep saves simple.
// - Settings are split between values that can apply live and values that require restart.
// - The board stays square across resolutions so gameplay layout remains predictable.

namespace dir_vec
{
	const olc::vi2d UP = { 0, -1 };
	const olc::vi2d DOWN = { 0, 1 };
	const olc::vi2d LEFT = { -1, 0 };
	const olc::vi2d RIGHT = { 1, 0 };
}

constexpr int DEFAULT_SNAKE_LEN = 3;
constexpr int SNAKE_SIZE = 16;
constexpr int NUM_OF_APPLES = 3;
constexpr float MOVE_INTERVAL = 0.12f;
constexpr int HUD_HEIGHT = 32;
constexpr int SCREEN_SIZE = 512;
constexpr int PIXEL_SCALE = 2;
constexpr char DEFAULT_PLAYER_NAME[] = "Player1";
constexpr size_t MAX_PLAYER_NAME_LENGTH = 18;
constexpr size_t LEADERBOARD_LIMIT = 8;

enum class WindowMode
{
	// Standard decorated desktop window.
	Windowed,
	// Borderless window sized to the chosen client area.
	BorderlessWindow,
	// Fullscreen path selected during engine construction.
	Fullscreen
};

enum class GameState
{
	// Front page with the primary game actions.
	MainMenu,
	// Read-only ranked score view across all saved profiles.
	Leaderboard,
	// Runtime settings screen for presentation and frame pacing options.
	Settings,
	// Profile create/select/remove flow.
	ProfileSelect,
	// Active run.
	Playing,
	// Gameplay frozen while pause actions remain clickable.
	Paused,
	// Run has ended and the score is already saved.
	GameOver
};

struct ResolutionOption
{
	// Window size is kept square so the board composition remains consistent.
	olc::vi2d windowSize;
};

struct AppConfig
{
	// Applied config is copied into a draft while the settings menu is open.
	bool vsync = true;
	int frameRateCap = 0;
	olc::vi2d windowSize = { 1024, 1024 };
	WindowMode windowMode = WindowMode::Windowed;
};

constexpr std::array<int, 5> FRAME_RATE_OPTIONS = { 30, 60, 120, 144, 0 };

// Store data next to the executable so saves work regardless of the launch directory.
fs::path resolveExecutableFilePath(const std::string& fileName)
{
	char modulePath[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
	if (length == 0)
	{
		return fs::current_path() / fileName;
	}

	return fs::path(std::string(modulePath, length)).parent_path() / fileName;
}

std::string toString(WindowMode mode)
{
	switch (mode)
	{
	case WindowMode::Windowed:
		return "Window";
	case WindowMode::BorderlessWindow:
		return "Window (Borderless)";
	case WindowMode::Fullscreen:
		return "Fullscreen";
	}

	return "Window";
}

WindowMode windowModeFromString(const std::string& value)
{
	if (value == "Fullscreen")
	{
		return WindowMode::Fullscreen;
	}

	if (value == "WindowBorderless")
	{
		return WindowMode::BorderlessWindow;
	}

	return WindowMode::Windowed;
}

bool isSupportedFrameRateCap(int cap)
{
	return std::ranges::find(FRAME_RATE_OPTIONS, cap) != FRAME_RATE_OPTIONS.end();
}

olc::vi2d getPrimaryWorkAreaSize()
{
	RECT workArea{};
	if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0))
	{
		return { workArea.right - workArea.left, workArea.bottom - workArea.top };
	}

	return { 1280, 1024 };
}

RECT getPrimaryWorkAreaRect()
{
	RECT workArea{ 0, 0, 1280, 1024 };
	SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
	return workArea;
}

std::vector<ResolutionOption> buildResolutionOptions(const olc::vi2d& workAreaSize)
{
	std::vector<ResolutionOption> options;
	// Keep the game square and leave some margin so the window still fits on screen.
	const int maxSize = std::max(512, std::min(workAreaSize.x, workAreaSize.y) - 80);

	for (int size = 512; size <= maxSize; size += 128)
	{
		options.push_back({ { size, size } });
	}

	if (options.empty() || options.back().windowSize.x != maxSize)
	{
		options.push_back({ { maxSize, maxSize } });
	}

	std::vector<ResolutionOption> uniqueOptions;
	for (const auto& option : options)
	{
		if (uniqueOptions.empty() || uniqueOptions.back().windowSize != option.windowSize)
		{
			uniqueOptions.push_back(option);
		}
	}

	return uniqueOptions;
}

size_t findBestResolutionIndex(const std::vector<ResolutionOption>& options, const olc::vi2d& targetSize)
{
	if (options.empty())
	{
		return 0;
	}

	size_t bestIndex = 0;
	int bestDistance = std::numeric_limits<int>::max();

	for (size_t i = 0; i < options.size(); ++i)
	{
		const int distance = std::abs(options[i].windowSize.x - targetSize.x) + std::abs(options[i].windowSize.y - targetSize.y);
		if (distance < bestDistance)
		{
			bestDistance = distance;
			bestIndex = i;
		}
	}

	return bestIndex;
}

AppConfig sanitizeAppConfig(AppConfig config)
{
	if (!isSupportedFrameRateCap(config.frameRateCap))
	{
		config.frameRateCap = 0;
	}

	if (config.windowSize.x < 512 || config.windowSize.y < 512)
	{
		config.windowSize = { 1024, 1024 };
	}

	return config;
}

AppConfig loadAppConfig(const fs::path& path)
{
	AppConfig config;

	// Missing settings are fine: the in-struct defaults become the startup config.
	std::ifstream input(path);
	if (!input.is_open())
	{
		return config;
	}

	std::string tag;
	while (input >> tag)
	{
		if (tag == "VSync")
		{
			int enabled = 0;
			if (input >> enabled)
			{
				config.vsync = enabled != 0;
			}
		}
		else if (tag == "FrameRateCap")
		{
			int cap = 0;
			if (input >> cap)
			{
				config.frameRateCap = cap;
			}
		}
		else if (tag == "ResolutionIndex")
		{
			size_t index = 0;
			if (input >> index)
			{
				config.windowSize = { 512 + static_cast<int>(index) * 128, 512 + static_cast<int>(index) * 128 };
			}
		}
		else if (tag == "ResolutionWidth")
		{
			input >> config.windowSize.x;
		}
		else if (tag == "ResolutionHeight")
		{
			input >> config.windowSize.y;
		}
		else if (tag == "WindowMode")
		{
			std::string mode;
			if (input >> mode)
			{
				config.windowMode = windowModeFromString(mode);
			}
		}
		else
		{
			std::string ignored;
			std::getline(input, ignored);
		}
	}

	return sanitizeAppConfig(config);
}

void saveAppConfig(const fs::path& path, const AppConfig& config)
{
	const AppConfig sanitized = sanitizeAppConfig(config);

	// A tiny text format keeps the file easy to inspect or repair by hand.
	std::ofstream output(path, std::ios::trunc);
	if (!output.is_open())
	{
		return;
	}

	output << "VSync " << (sanitized.vsync ? 1 : 0) << '\n';
	output << "FrameRateCap " << sanitized.frameRateCap << '\n';
	output << "ResolutionWidth " << sanitized.windowSize.x << '\n';
	output << "ResolutionHeight " << sanitized.windowSize.y << '\n';
	output << "WindowMode ";
	switch (sanitized.windowMode)
	{
	case WindowMode::Windowed:
		output << "Window";
		break;
	case WindowMode::BorderlessWindow:
		output << "WindowBorderless";
		break;
	case WindowMode::Fullscreen:
		output << "Fullscreen";
		break;
	}
	output << '\n';
}

std::string frameRateLabel(int cap)
{
	return cap <= 0 ? "Infinite" : std::to_string(cap);
}

int nextFrameRateCap(int currentCap)
{
	auto it = std::find(FRAME_RATE_OPTIONS.begin(), FRAME_RATE_OPTIONS.end(), currentCap);
	if (it == FRAME_RATE_OPTIONS.end())
	{
		return FRAME_RATE_OPTIONS.front();
	}

	++it;
	return it == FRAME_RATE_OPTIONS.end() ? FRAME_RATE_OPTIONS.front() : *it;
}

WindowMode nextWindowMode(WindowMode currentMode)
{
	switch (currentMode)
	{
	case WindowMode::Windowed:
		return WindowMode::BorderlessWindow;
	case WindowMode::BorderlessWindow:
		return WindowMode::Fullscreen;
	case WindowMode::Fullscreen:
		return WindowMode::Windowed;
	}

	return WindowMode::Windowed;
}

struct Apple
{
	olc::vi2d mPos;

	void draw(olc::PixelGameEngine& engine) const
	{
		// Rounded fruit stands out better than a same-sized square against the grid.
		const olc::vi2d center = mPos + olc::vi2d{ SNAKE_SIZE / 2, SNAKE_SIZE / 2 };
		const int radius = SNAKE_SIZE / 2 - 2;
		engine.FillCircle(center, radius, olc::RED);
		engine.DrawCircle(center, radius, olc::Pixel(255, 225, 225));
	}
};

struct Snake
{
	struct SnakeBody
	{
		olc::vi2d pos;
	};

	// Head is stored at index 0 so movement, collision, and growth stay straightforward.
	std::vector<SnakeBody> mBody;
	olc::vi2d mDirVec2d = dir_vec::LEFT;

	Snake() = default;

	Snake(int32_t width, int32_t height)
	{
		reset(width, height, HUD_HEIGHT);
	}

	void reset(int32_t width, int32_t height, int32_t topBoundary)
	{
		mBody.clear();
		mDirVec2d = dir_vec::LEFT;

		const int startX = ((width / 2) / SNAKE_SIZE) * SNAKE_SIZE;
		const int playableRows = (height - topBoundary) / SNAKE_SIZE;
		// Start inside the playable area instead of centering through the HUD strip.
		const int startY = topBoundary + (playableRows / 2) * SNAKE_SIZE;

		for (int i = 0; i < DEFAULT_SNAKE_LEN; ++i)
		{
			mBody.push_back({ { startX + i * SNAKE_SIZE, startY } });
		}
	}

	void stepUpdate()
	{
		// Shift tail segments first, then advance the head one grid cell.
		for (size_t i = mBody.size() - 1; i > 0; --i)
		{
			mBody[i].pos = mBody[i - 1].pos;
		}

		mBody.front().pos += mDirVec2d * SNAKE_SIZE;
	}

	void draw(olc::PixelGameEngine& engine) const
	{
		for (size_t i = 0; i < mBody.size(); ++i)
		{
			const olc::Pixel fill = i == 0 ? olc::Pixel(244, 203, 73) : olc::Pixel(64, 204, 123);
			const olc::Pixel edge = i == 0 ? olc::Pixel(255, 238, 167) : olc::Pixel(175, 255, 211);

			engine.FillRect(mBody[i].pos, { SNAKE_SIZE, SNAKE_SIZE }, fill);
			engine.DrawRect(mBody[i].pos, { SNAKE_SIZE, SNAKE_SIZE }, edge);
		}
	}

	void toUp() noexcept { mDirVec2d = dir_vec::UP; }
	void toRight() noexcept { mDirVec2d = dir_vec::RIGHT; }
	void toDown() noexcept { mDirVec2d = dir_vec::DOWN; }
	void toLeft() noexcept { mDirVec2d = dir_vec::LEFT; }

	SnakeBody& getSnakeHead() noexcept
	{
		return mBody.front();
	}

	const SnakeBody& getSnakeHead() const noexcept
	{
		return mBody.front();
	}

	bool isDeath(int32_t width, int32_t height, int32_t topBoundary) const noexcept
	{
		const auto& headPos = getSnakeHead().pos;

		if (headPos.x < 0 || headPos.y < topBoundary ||
			headPos.x + SNAKE_SIZE > width ||
			headPos.y + SNAKE_SIZE > height)
		{
			return true;
		}

		for (size_t i = 1; i < mBody.size(); ++i)
		{
			if (mBody[i].pos == headPos)
			{
				return true;
			}
		}

		return false;
	}

	void extendBody()
	{
		mBody.push_back(mBody.back());
	}
};

struct LeaderboardEntry
{
	// One entry doubles as both a saved profile and that profile's best run.
	std::string playerName;
	uint64_t highScore = 0;
	std::string updatedAt;
};

class Game final : public olc::PixelGameEngine
{
public:
	// The constructor captures startup config only; engine-dependent setup happens later.
	Game(const AppConfig& startupConfig)
		: olc::PixelGameEngine()
		, mScore(0)
		, mMoveTimer(0.0f)
		, mRandEngine(std::random_device{}())
		, mWorkAreaSize(getPrimaryWorkAreaSize())
		, mResolutionOptions(buildResolutionOptions(mWorkAreaSize))
		, mProfilePath(resolveExecutableFilePath("snake_profiles.dat"))
		, mSettingsPath(resolveExecutableFilePath("snake_settings.dat"))
		, mAppliedConfig(sanitizeAppConfig(startupConfig))
		, mDraftConfig(mAppliedConfig)
	{
		sAppName = "Snake";
	}

	bool OnUserCreate() override
	{
		// Startup order matters here: load data, build widgets that point at that data,
		// then sync labels before the first frame is drawn.
		rebuildRandomRange();
		initializeResolutionChoices();
		saveAppConfig(mSettingsPath, mAppliedConfig);
		loadProfileData();
		buildUi();
		refreshMainMenuButtonLabels();
		refreshSettingsButtonLabels();
		applyDisplaySettingsIfPossible();
		resetGame();
		mState = GameState::MainMenu;
		return true;
	}

	bool OnUserUpdate(float fElapsedTime) override
	{
		drawBackdrop();

		// Keep menu/gameplay behavior explicit. Each state owns both input and drawing.
		switch (mState)
		{
		case GameState::MainMenu:
			updateMainMenu();
			drawHud("Main Menu");
			drawMainMenuCard();
			break;

		case GameState::Leaderboard:
			updateLeaderboardMenu();
			drawHud("Leaderboard");
			drawLeaderboardCard();
			break;

		case GameState::Settings:
			updateSettingsMenu();
			drawHud("Settings");
			drawSettingsCard();
			break;

		case GameState::ProfileSelect:
			updateProfileSelectMenu();
			drawHud("Profiles");
			drawProfileSelectCard();
			break;

		case GameState::Playing:
			handleGameplayInput();
			if (mState == GameState::Playing)
			{
				updateGameplay(fElapsedTime);
			}
			drawPlayfield();
			if (mState == GameState::GameOver)
			{
				drawHud("Game Over");
				drawGameOverCard();
			}
			else
			{
				drawHud("Esc or P to pause");
			}
			break;

		case GameState::Paused:
			drawPlayfield();
			updatePauseMenu();
			drawHud("Paused");
			drawPauseCard();
			break;

		case GameState::GameOver:
			drawPlayfield();
			updateGameOverInput();
			drawHud("Game Over");
			drawGameOverCard();
			break;
		}

		enforceFrameRateCap();
		return !mShouldExit;
	}

private:
	// Resolution choices are rebuilt from the current monitor work area instead of using a
	// fixed list, so the settings screen only offers sizes that are realistic on this PC.
	void initializeResolutionChoices()
	{
		if (mResolutionOptions.empty())
		{
			mResolutionOptions.push_back({ { 1024, 1024 } });
		}

		mResolutionLabels.clear();
		mResolutionLabels.reserve(mResolutionOptions.size());

		for (auto& option : mResolutionOptions)
		{
			const std::string label = std::to_string(option.windowSize.x) + " x " + std::to_string(option.windowSize.y);
			mResolutionLabels.push_back(label);
		}

		// Clamp saved config to the generated list so the UI always has a valid selection.
		mAppliedConfig.windowSize = mResolutionOptions[findBestResolutionIndex(mResolutionOptions, mAppliedConfig.windowSize)].windowSize;
		mDraftConfig = mAppliedConfig;
		mSelectedResolutionIndex = findBestResolutionIndex(mResolutionOptions, mDraftConfig.windowSize);
	}

	void buildUi()
	{
		// All menus reuse the same visual language, but keep separate managers so each
		// screen can update only the widgets it owns.
		configureTheme(mMainMenuGui);
		mLeaderboardGui.CopyThemeFrom(mMainMenuGui);
		mPauseMenuGui.CopyThemeFrom(mMainMenuGui);
		mSettingsGui.CopyThemeFrom(mMainMenuGui);
		mProfileGui.CopyThemeFrom(mMainMenuGui);

		// Main menu follows a common game pattern: title and central action stack, with the
		// active profile exposed separately in the lower-left corner.
		mButtonMainStart = new olc::QuickGUI::Button(
			mMainMenuGui,
			"Start Game",
			{ 156.0f, 182.0f },
			{ 200.0f, 30.0f }
		);
		mButtonMainLeaderboard = new olc::QuickGUI::Button(
			mMainMenuGui,
			"Leaderboard",
			{ 156.0f, 222.0f },
			{ 200.0f, 30.0f }
		);
		mButtonMainSettings = new olc::QuickGUI::Button(
			mMainMenuGui,
			"Settings",
			{ 156.0f, 262.0f },
			{ 200.0f, 30.0f }
		);
		mButtonMainQuit = new olc::QuickGUI::Button(
			mMainMenuGui,
			"Exit",
			{ 156.0f, 302.0f },
			{ 200.0f, 30.0f }
		);
		mButtonMainProfile = new olc::QuickGUI::Button(
			mMainMenuGui,
			"",
			{ 24.0f, 472.0f },
			{ 196.0f, 24.0f }
		);

		mButtonLeaderboardBack = new olc::QuickGUI::Button(
			mLeaderboardGui,
			"Back",
			{ 182.0f, 444.0f },
			{ 148.0f, 28.0f }
		);

		const float pauseButtonX = ScreenWidth() * 0.5f - 84.0f;
		// Pause actions stay centered so the menu remains easy to hit with the mouse.
		mButtonPauseResume = new olc::QuickGUI::Button(
			mPauseMenuGui,
			"Resume",
			{ pauseButtonX, 228.0f },
			{ 168.0f, 28.0f }
		);
		mButtonPauseRestart = new olc::QuickGUI::Button(
			mPauseMenuGui,
			"Restart",
			{ pauseButtonX, 264.0f },
			{ 168.0f, 28.0f }
		);
		mButtonPauseMenu = new olc::QuickGUI::Button(
			mPauseMenuGui,
			"Main Menu",
			{ pauseButtonX, 300.0f },
			{ 168.0f, 28.0f }
		);

		mButtonSettingsVSync = new olc::QuickGUI::Button(
			mSettingsGui,
			"",
			{ 274.0f, 170.0f },
			{ 168.0f, 28.0f }
		);
		mButtonSettingsFrameRate = new olc::QuickGUI::Button(
			mSettingsGui,
			"",
			{ 274.0f, 210.0f },
			{ 168.0f, 28.0f }
		);
		mButtonSettingsWindowMode = new olc::QuickGUI::Button(
			mSettingsGui,
			"",
			{ 274.0f, 332.0f },
			{ 168.0f, 28.0f }
		);
		mResolutionListBox = new olc::QuickGUI::ListBox(
			mSettingsGui,
			mResolutionLabels,
			{ 274.0f, 250.0f },
			{ 168.0f, 72.0f }
		);
		// Resolution is the one option with enough values to justify list selection.
		mButtonSettingsApply = new olc::QuickGUI::Button(
			mSettingsGui,
			"Apply",
			{ 118.0f, 394.0f },
			{ 148.0f, 28.0f }
		);
		mButtonSettingsBack = new olc::QuickGUI::Button(
			mSettingsGui,
			"Back",
			{ 284.0f, 394.0f },
			{ 148.0f, 28.0f }
		);

		mProfileListBox = new olc::QuickGUI::ListBox(
			mProfileGui,
			mProfileNames,
			{ 150.0f, 170.0f },
			{ 212.0f, 150.0f }
		);
		mButtonProfileUse = new olc::QuickGUI::Button(
			mProfileGui,
			"Use Selected",
			{ 94.0f, 348.0f },
			{ 146.0f, 28.0f }
		);
		mButtonProfileCreate = new olc::QuickGUI::Button(
			mProfileGui,
			"Create New",
			{ 272.0f, 348.0f },
			{ 146.0f, 28.0f }
		);
		mButtonProfileRemove = new olc::QuickGUI::Button(
			mProfileGui,
			"Remove Selected",
			{ 94.0f, 388.0f },
			{ 146.0f, 28.0f }
		);
		mButtonProfileBack = new olc::QuickGUI::Button(
			mProfileGui,
			"Back",
			{ 272.0f, 388.0f },
			{ 148.0f, 28.0f }
		);
	}

	void configureTheme(olc::QuickGUI::Manager& gui)
	{
		// Keep one shared palette so screen-specific managers still look like the same game.
		gui.colNormal = olc::Pixel(24, 81, 122);
		gui.colHover = olc::Pixel(46, 137, 193);
		gui.colClick = olc::Pixel(234, 179, 8);
		gui.colDisable = olc::Pixel(45, 55, 68);
		gui.colBorder = olc::Pixel(220, 234, 245);
		gui.colText = olc::WHITE;
		gui.fHoverSpeedOn = 14.0f;
		gui.fHoverSpeedOff = 8.0f;
	}

	void loadProfileData()
	{
		mLeaderboard.clear();
		mCurrentPlayerName = DEFAULT_PLAYER_NAME;

		// If no profile file exists yet, create the default profile immediately so later
		// gameplay and UI paths can assume at least one valid profile is present.
		std::ifstream input(mProfilePath);
		if (!input.is_open())
		{
			ensureCurrentProfileEntry();
			saveProfileData();
			return;
		}

		std::string tag;
		// File format:
		// PROFILE "active profile name"
		// ENTRY "profile name" bestScore "last updated"
		while (input >> tag)
		{
			if (tag == "PROFILE")
			{
				std::string storedPlayer;
				if (input >> std::quoted(storedPlayer))
				{
					mCurrentPlayerName = sanitizePlayerName(storedPlayer, true);
				}
			}
			else if (tag == "ENTRY")
			{
				std::string playerName;
				std::string updatedAt;
				uint64_t highScore = 0;

				if (input >> std::quoted(playerName) >> highScore >> std::quoted(updatedAt))
				{
					upsertLeaderboardEntry(playerName, highScore, updatedAt);
				}
			}
			else
			{
				std::string ignored;
				std::getline(input, ignored);
			}
		}

		if (mCurrentPlayerName.empty())
		{
			mCurrentPlayerName = DEFAULT_PLAYER_NAME;
		}

		ensureCurrentProfileEntry();
		sortLeaderboard();
		refreshProfileNames();
	}

	void saveProfileData()
	{
		sortLeaderboard();

		// Persist the active profile plus every best-score entry in one file.
		std::ofstream output(mProfilePath, std::ios::trunc);
		if (!output.is_open())
		{
			return;
		}

		output << "PROFILE " << std::quoted(mCurrentPlayerName) << '\n';
		for (const auto& entry : mLeaderboard)
		{
			output << "ENTRY "
				<< std::quoted(entry.playerName) << ' '
				<< entry.highScore << ' '
				<< std::quoted(entry.updatedAt)
				<< '\n';
		}
	}

	void refreshProfileNames()
	{
		mProfileNames.clear();
		mProfileNames.reserve(mLeaderboard.size());

		for (const auto& entry : mLeaderboard)
		{
			mProfileNames.push_back(entry.playerName);
		}

		std::sort(mProfileNames.begin(), mProfileNames.end());

		if (mProfileNames.empty())
		{
			mProfileNames.push_back(DEFAULT_PLAYER_NAME);
		}

		// Keep the list selection aligned with the active profile after create/remove/load.
		auto it = std::find(mProfileNames.begin(), mProfileNames.end(), mCurrentPlayerName);
		mSelectedProfileIndex = it == mProfileNames.end()
			? 0
			: static_cast<size_t>(std::distance(mProfileNames.begin(), it));
	}

	void refreshMainMenuButtonLabels()
	{
		if (mButtonMainProfile != nullptr)
		{
			mButtonMainProfile->sText = "Profile: " + mCurrentPlayerName;
		}

		if (mProfileListBox != nullptr && !mProfileNames.empty())
		{
			// QuickGUI keeps selection history internally, so refresh all related fields
			// whenever the underlying profile list changes.
			mProfileListBox->nSelectedItem = std::min(mSelectedProfileIndex, mProfileNames.size() - 1);
			mProfileListBox->nPreviouslySelectedItem = mProfileListBox->nSelectedItem;
			mProfileListBox->bSelectionChanged = false;
		}
	}

	std::string createNextProfileName() const
	{
		int index = 1;
		while (true)
		{
			const std::string candidate = "Player" + std::to_string(index);
			const bool exists = std::ranges::any_of(
				mLeaderboard,
				[&](const LeaderboardEntry& entry) { return entry.playerName == candidate; }
			);

			if (!exists)
			{
				return candidate;
			}

			++index;
		}
	}

	void setCurrentProfile(const std::string& profileName)
	{
		mCurrentPlayerName = sanitizePlayerName(profileName, true);
		ensureCurrentProfileEntry();
		refreshProfileNames();
		refreshMainMenuButtonLabels();
		saveProfileData();
	}

	void createProfile()
	{
		const std::string newProfile = createNextProfileName();
		// A zero-score entry makes the new profile immediately visible to every screen.
		upsertLeaderboardEntry(newProfile, 0, "-");
		setCurrentProfile(newProfile);
		mProfileStatusText = "Created profile " + newProfile + ".";
	}

	void removeSelectedProfile()
	{
		if (mProfileNames.empty())
		{
			mProfileStatusText = "No profile is available to remove.";
			return;
		}

		if (mProfileNames.size() <= 1)
		{
			mProfileStatusText = "Keep at least one profile in the game.";
			return;
		}

		const size_t index = std::min(mSelectedProfileIndex, mProfileNames.size() - 1);
		const std::string removedProfile = mProfileNames[index];

		std::erase_if(
			mLeaderboard,
			[&](const LeaderboardEntry& entry) { return entry.playerName == removedProfile; }
		);

		// If the active profile was deleted, switch to a nearby surviving profile so the
		// rest of the session stays valid without forcing the player through setup again.
		if (mCurrentPlayerName == removedProfile)
		{
			const size_t fallbackIndex = index > 0 ? index - 1 : 0;
			mCurrentPlayerName = mProfileNames[fallbackIndex == index ? index + 1 : fallbackIndex];
		}

		ensureCurrentProfileEntry();
		refreshProfileNames();
		refreshMainMenuButtonLabels();
		saveProfileData();
		mProfileStatusText = "Removed profile " + removedProfile + ".";
	}

	void openProfileSelect()
	{
		// Re-sync when opening so recent gameplay changes are visible immediately.
		refreshProfileNames();
		refreshMainMenuButtonLabels();
		mProfileStatusText = "Select a profile or create a new one.";
		mState = GameState::ProfileSelect;
	}

	void rebuildRandomRange()
	{
		const int cols = ScreenWidth() / SNAKE_SIZE;
		const int rows = (ScreenHeight() - HUD_HEIGHT) / SNAKE_SIZE;

		mRandX = std::uniform_int_distribution<int32_t>(0, cols - 1);
		mRandY = std::uniform_int_distribution<int32_t>(0, rows - 1);
	}

	void startNewGame()
	{
		resetGame();
		mState = GameState::Playing;
	}

	void returnToMainMenu()
	{
		resetGame();
		mState = GameState::MainMenu;
	}

	void resetGame()
	{
		mApples.clear();
		mScore = 0;
		mMoveTimer = 0.0f;
		mRunScoreSaved = false;

		// Reset only per-run state here; menus, settings, and profiles stay intact.
		mSnake.reset(ScreenWidth(), ScreenHeight(), HUD_HEIGHT);
		addApples();
	}

	void updateGameplay(float fElapsedTime)
	{
		mMoveTimer += fElapsedTime;

		// Catch up in fixed-size movement steps so gameplay speed stays stable even if
		// a frame takes longer than usual.
		while (mMoveTimer >= MOVE_INTERVAL)
		{
			mMoveTimer -= MOVE_INTERVAL;
			mSnake.stepUpdate();

			if (isEatApple())
			{
				addApples();
			}

			if (mSnake.isDeath(ScreenWidth(), ScreenHeight(), HUD_HEIGHT))
			{
				recordScoreIfNeeded();
				mState = GameState::GameOver;
				return;
			}
		}
	}

	void updateMainMenu()
	{
		mMainMenuGui.Update(this);

		// Main menu keeps a small keyboard fallback while remaining primarily click-driven.
		if (GetKey(olc::Key::ENTER).bPressed || GetKey(olc::Key::SPACE).bPressed)
		{
			startNewGame();
			return;
		}

		if (GetKey(olc::Key::ESCAPE).bPressed)
		{
			mShouldExit = true;
			return;
		}

		if (mButtonMainStart->bReleased)
		{
			startNewGame();
		}
		else if (mButtonMainLeaderboard->bReleased)
		{
			mState = GameState::Leaderboard;
		}
		else if (mButtonMainSettings->bReleased)
		{
			mDraftConfig = mAppliedConfig;
			refreshSettingsButtonLabels();
			mSettingsStatusText = "Change options, then press Apply.";
			mState = GameState::Settings;
		}
		else if (mButtonMainProfile->bReleased)
		{
			openProfileSelect();
		}
		else if (mButtonMainQuit->bReleased)
		{
			mShouldExit = true;
		}
	}

	void updateLeaderboardMenu()
	{
		mLeaderboardGui.Update(this);

		if (GetKey(olc::Key::ESCAPE).bPressed || mButtonLeaderboardBack->bReleased)
		{
			mState = GameState::MainMenu;
		}
	}

	void updateSettingsMenu()
	{
		if (GetKey(olc::Key::ESCAPE).bPressed)
		{
			mState = GameState::MainMenu;
			return;
		}

		mSettingsGui.Update(this);

		// Edits stay in the draft config until Apply is pressed so multiple changes can be
		// reviewed together before they affect the running session.
		if (mButtonSettingsVSync->bReleased)
		{
			mDraftConfig.vsync = !mDraftConfig.vsync;
			refreshSettingsButtonLabels();
		}
		else if (mButtonSettingsFrameRate->bReleased)
		{
			mDraftConfig.frameRateCap = nextFrameRateCap(mDraftConfig.frameRateCap);
			refreshSettingsButtonLabels();
		}
		else if (mResolutionListBox != nullptr && mResolutionListBox->bSelectionChanged)
		{
			mSelectedResolutionIndex = std::min(mResolutionListBox->nSelectedItem, mResolutionOptions.size() - 1);
			mDraftConfig.windowSize = mResolutionOptions[mSelectedResolutionIndex].windowSize;
		}
		else if (mButtonSettingsWindowMode->bReleased)
		{
			mDraftConfig.windowMode = nextWindowMode(mDraftConfig.windowMode);
			refreshSettingsButtonLabels();
		}
		else if (mButtonSettingsApply->bReleased)
		{
			applySettingsFromDraft();
		}
		else if (mButtonSettingsBack->bReleased)
		{
			mDraftConfig = mAppliedConfig;
			refreshSettingsButtonLabels();
			mState = GameState::MainMenu;
		}
	}

	void updateProfileSelectMenu()
	{
		mProfileGui.Update(this);

		// The list box handles the mouse interaction, but the game owns the selected index
		// so it can safely rebuild the list after create/remove operations.
		if (mProfileListBox != nullptr && mProfileListBox->bSelectionChanged && !mProfileNames.empty())
		{
			mSelectedProfileIndex = std::min(mProfileListBox->nSelectedItem, mProfileNames.size() - 1);
		}

		if (GetKey(olc::Key::ESCAPE).bPressed)
		{
			mState = GameState::MainMenu;
			return;
		}

		if (mButtonProfileUse->bReleased && !mProfileNames.empty())
		{
			setCurrentProfile(mProfileNames[mSelectedProfileIndex]);
			mProfileStatusText = "Switched to " + mCurrentPlayerName + ".";
			mState = GameState::MainMenu;
		}
		else if (mButtonProfileCreate->bReleased)
		{
			createProfile();
		}
		else if (mButtonProfileRemove->bReleased)
		{
			removeSelectedProfile();
		}
		else if (mButtonProfileBack->bReleased)
		{
			mState = GameState::MainMenu;
		}
	}

	void updatePauseMenu()
	{
		if (GetKey(olc::Key::ESCAPE).bPressed || GetKey(olc::Key::P).bPressed)
		{
			mState = GameState::Playing;
			return;
		}

		mPauseMenuGui.Update(this);

		if (mButtonPauseResume->bReleased)
		{
			mState = GameState::Playing;
		}
		else if (mButtonPauseRestart->bReleased)
		{
			startNewGame();
		}
		else if (mButtonPauseMenu->bReleased)
		{
			returnToMainMenu();
		}
	}

	void updateGameOverInput()
	{
		if (GetKey(olc::Key::R).bPressed || GetKey(olc::Key::ENTER).bPressed)
		{
			startNewGame();
		}
		else if (GetKey(olc::Key::M).bPressed || GetKey(olc::Key::ESCAPE).bPressed)
		{
			returnToMainMenu();
		}
	}

	void refreshSettingsButtonLabels()
	{
		// Reflect the draft config in the widget labels so the screen always shows the
		// values currently being edited, not only the last-applied settings.
		if (mButtonSettingsVSync != nullptr)
		{
			mButtonSettingsVSync->sText = mDraftConfig.vsync ? "On" : "Off";
		}

		if (mButtonSettingsFrameRate != nullptr)
		{
			mButtonSettingsFrameRate->sText = frameRateLabel(mDraftConfig.frameRateCap);
		}

		if (mButtonSettingsWindowMode != nullptr)
		{
			mButtonSettingsWindowMode->sText = toString(mDraftConfig.windowMode);
		}

		if (mResolutionListBox != nullptr && !mResolutionOptions.empty())
		{
			mSelectedResolutionIndex = findBestResolutionIndex(mResolutionOptions, mDraftConfig.windowSize);
			mResolutionListBox->nSelectedItem = mSelectedResolutionIndex;
			mResolutionListBox->nPreviouslySelectedItem = mSelectedResolutionIndex;
			mResolutionListBox->bSelectionChanged = false;
		}
	}

	void applyDisplaySettingsIfPossible()
	{
		if (mAppliedConfig.windowMode == WindowMode::Fullscreen)
		{
			return;
		}

		// Windowed and borderless changes can be applied live by resizing and recentering
		// the native window inside the current work area.
		const RECT workArea = getPrimaryWorkAreaRect();
		const olc::vi2d workAreaSize = { workArea.right - workArea.left, workArea.bottom - workArea.top };
		const olc::vi2d targetSize = {
			std::min(mAppliedConfig.windowSize.x, workAreaSize.x),
			std::min(mAppliedConfig.windowSize.y, workAreaSize.y)
		};

		const olc::vi2d targetPos = {
			workArea.left + (workAreaSize.x - targetSize.x) / 2,
			workArea.top + (workAreaSize.y - targetSize.y) / 2
		};

		ShowWindowFrame(mAppliedConfig.windowMode == WindowMode::Windowed);
		SetWindowSize(targetPos, targetSize);
	}

	void applySettingsFromDraft()
	{
		const AppConfig sanitizedDraft = sanitizeAppConfig(mDraftConfig);
		// PixelGameEngine applies VSync/fullscreen at construction time, so those options
		// are saved immediately but only fully take effect after restart.
		const bool restartRequired =
			sanitizedDraft.vsync != mAppliedConfig.vsync ||
			sanitizedDraft.windowMode == WindowMode::Fullscreen ||
			mAppliedConfig.windowMode == WindowMode::Fullscreen;

		mAppliedConfig = sanitizedDraft;
		mDraftConfig = mAppliedConfig;
		saveAppConfig(mSettingsPath, mAppliedConfig);
		refreshSettingsButtonLabels();
		resetFrameRateLimiter();

		if (!restartRequired)
		{
			applyDisplaySettingsIfPossible();
			mSettingsStatusText = "Saved. Windowed changes applied now.";
		}
		else
		{
			mSettingsStatusText = "Saved. Restart to apply VSync/fullscreen.";
		}
	}

	void resetFrameRateLimiter()
	{
		mNextFrameDeadline = std::chrono::steady_clock::time_point{};
	}

	void enforceFrameRateCap()
	{
		if (mAppliedConfig.frameRateCap <= 0)
		{
			resetFrameRateLimiter();
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		const auto frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<double>(1.0 / static_cast<double>(mAppliedConfig.frameRateCap))
		);

		// If the app stalls badly, reset the schedule instead of sleeping through a large
		// backlog of already-missed frame deadlines.
		if (mNextFrameDeadline.time_since_epoch().count() == 0 || now > mNextFrameDeadline + frameDuration * 2)
		{
			mNextFrameDeadline = now;
		}

		mNextFrameDeadline += frameDuration;
		if (mNextFrameDeadline > now)
		{
			std::this_thread::sleep_until(mNextFrameDeadline);
		}
		else
		{
			mNextFrameDeadline = now;
		}
	}

	void handleGameplayInput()
	{
		// Reject direct reversals to preserve classic snake movement rules.
		if (GetKey(olc::Key::ESCAPE).bPressed || GetKey(olc::Key::P).bPressed)
		{
			mState = GameState::Paused;
			return;
		}

		if (GetKey(olc::Key::R).bPressed)
		{
			startNewGame();
			return;
		}

		if (GetKey(olc::Key::UP).bPressed && mSnake.mDirVec2d != dir_vec::DOWN)
		{
			mSnake.toUp();
		}
		else if (GetKey(olc::Key::RIGHT).bPressed && mSnake.mDirVec2d != dir_vec::LEFT)
		{
			mSnake.toRight();
		}
		else if (GetKey(olc::Key::DOWN).bPressed && mSnake.mDirVec2d != dir_vec::UP)
		{
			mSnake.toDown();
		}
		else if (GetKey(olc::Key::LEFT).bPressed && mSnake.mDirVec2d != dir_vec::RIGHT)
		{
			mSnake.toLeft();
		}
	}

	void drawBackdrop()
	{
		Clear(olc::Pixel(7, 12, 20));

		// A subtle checkerboard keeps menus and gameplay visually connected.
		for (int y = 0; y < ScreenHeight(); y += SNAKE_SIZE)
		{
			for (int x = 0; x < ScreenWidth(); x += SNAKE_SIZE)
			{
				const bool darkCell = ((x / SNAKE_SIZE) + (y / SNAKE_SIZE)) % 2 == 0;
				FillRect(
					{ x, y },
					{ SNAKE_SIZE, SNAKE_SIZE },
					darkCell ? olc::Pixel(13, 22, 35) : olc::Pixel(10, 18, 29)
				);
			}
		}
	}

	void drawPlayfield()
	{
		// Gameplay objects are rendered only during run-related states so the main menu and
		// settings screens do not look like a hidden match has already started.
		for (const auto& apple : mApples)
		{
			apple.draw(*this);
		}

		mSnake.draw(*this);
	}

	void drawHud(const std::string& message)
	{
		// Keep HUD copy compact: score is stable on the left, current state/hint on the right.
		const olc::vi2d barSize = { ScreenWidth(), 28 };
		FillRect({ 0, 0 }, barSize, olc::Pixel(3, 7, 12));
		DrawStringProp({ 8, 8 }, "Score: " + std::to_string(mScore), olc::WHITE);

		const olc::vi2d messageSize = GetTextSizeProp(message);
		DrawStringProp(
			{ ScreenWidth() - messageSize.x - 8, 8 },
			message,
			olc::Pixel(173, 184, 197)
		);
	}

	void drawMainMenuCard()
	{
		fillOverlay();

		// The title card mirrors common game menus: strong title, clear primary actions,
		// and profile identity shown as supporting information rather than a blocker.
		const olc::vi2d panelPos = { 96, 56 };
		const olc::vi2d panelSize = { ScreenWidth() - 192, ScreenHeight() - 120 };

		drawCard(panelPos, panelSize, olc::Pixel(11, 20, 32), olc::Pixel(220, 234, 245), olc::Pixel(37, 83, 118));
		drawCenteredPropString("SNAKE", 86, olc::WHITE, 3);
		drawCenteredPropString("Arcade hunt for the highest score.", 136, olc::Pixel(168, 191, 214));
		drawCenteredPropString("Current Profile: " + mCurrentPlayerName, 390, olc::Pixel(244, 203, 73));
		drawCenteredPropString("Best Score: " + std::to_string(getCurrentPlayerHighScore()), 410, olc::WHITE);
		DrawStringProp({ 26, 478 }, "Click the profile button to switch players.", olc::Pixel(145, 163, 184));

		mMainMenuGui.Draw(this);
	}

	void drawLeaderboardCard()
	{
		fillOverlay();

		const olc::vi2d panelPos = { 74, 52 };
		const olc::vi2d panelSize = { ScreenWidth() - 148, ScreenHeight() - 104 };

		drawCard(panelPos, panelSize, olc::Pixel(11, 20, 32), olc::Pixel(220, 234, 245), olc::Pixel(37, 83, 118));
		drawCenteredPropString("Leaderboard", 82, olc::WHITE, 2);
		drawCenteredPropString("Top runs across every saved profile.", 114, olc::Pixel(168, 191, 214));
		drawLeaderboardList({ 132, 156 });

		mLeaderboardGui.Draw(this);
	}

	void drawSettingsCard()
	{
		fillOverlay();

		// The explanatory copy is part of the UX here because some settings are limited by
		// PGE and cannot all apply instantly in the current process.
		const olc::vi2d panelPos = { 36, 36 };
		const olc::vi2d panelSize = { ScreenWidth() - 72, ScreenHeight() - 72 };

		drawCard(panelPos, panelSize, olc::Pixel(11, 20, 32), olc::Pixel(220, 234, 245), olc::Pixel(37, 83, 118));
		drawCenteredPropString("Settings", 74, olc::WHITE, 2);
		drawCenteredPropString("Frame cap applies immediately.", 106, olc::Pixel(168, 191, 214));
		drawCenteredPropString("VSync and fullscreen apply on next launch.", 122, olc::Pixel(168, 191, 214));

		DrawStringProp({ 88, 176 }, "Vertical Sync", olc::WHITE);
		DrawStringProp({ 88, 216 }, "Max Frame Rate", olc::WHITE);
		DrawStringProp({ 88, 256 }, "Resolution", olc::WHITE);
		DrawStringProp({ 88, 338 }, "Display Mode", olc::WHITE);

		DrawStringProp({ 88, 432 }, "Resolution list is generated from your current screen size.", olc::Pixel(145, 163, 184));
		DrawStringProp({ 88, 448 }, "Fullscreen uses the monitor size on next launch.", olc::Pixel(145, 163, 184));
		DrawStringProp({ 88, 466 }, mSettingsStatusText, olc::Pixel(244, 203, 73));

		mSettingsGui.Draw(this);
	}

	void drawProfileSelectCard()
	{
		fillOverlay();

		// Profiles are presented as simple local save slots, not as a complex account system.
		const olc::vi2d panelPos = { 92, 66 };
		const olc::vi2d panelSize = { ScreenWidth() - 184, ScreenHeight() - 132 };

		drawCard(panelPos, panelSize, olc::Pixel(11, 20, 32), olc::Pixel(220, 234, 245), olc::Pixel(37, 83, 118));
		drawCenteredPropString("Profiles", 94, olc::WHITE, 2);
		drawCenteredPropString("Each profile keeps its own best score.", 126, olc::Pixel(168, 191, 214));
		drawCenteredPropString("Current: " + mCurrentPlayerName, 146, olc::Pixel(244, 203, 73));
		DrawStringProp({ 150, 334 }, "Use, create, or remove a saved profile.", olc::Pixel(145, 163, 184));
		DrawStringProp({ 150, 350 }, "The last remaining profile cannot be removed.", olc::Pixel(145, 163, 184));
		DrawStringProp({ 150, 430 }, mProfileStatusText, olc::Pixel(244, 203, 73));

		mProfileGui.Draw(this);
	}

	void drawPauseCard()
	{
		fillOverlay();

		const olc::vi2d panelPos = { 108, 122 };
		const olc::vi2d panelSize = { ScreenWidth() - 216, 246 };

		drawCard(panelPos, panelSize, olc::Pixel(11, 20, 32), olc::Pixel(220, 234, 245), olc::Pixel(37, 83, 118));
		drawCenteredPropString("Paused", 156, olc::WHITE, 2);
		drawCenteredPropString("Use the mouse to choose what to do next.", 188, olc::Pixel(168, 191, 214));

		mPauseMenuGui.Draw(this);
	}

	void drawGameOverCard()
	{
		fillOverlay();

		const olc::vi2d panelPos = { 96, 160 };
		const olc::vi2d panelSize = { ScreenWidth() - 192, 160 };

		drawCard(panelPos, panelSize, olc::Pixel(34, 12, 18), olc::Pixel(255, 128, 128), olc::Pixel(115, 26, 33));
		drawCenteredPropString("Game Over", 190, olc::Pixel(255, 168, 168), 2);
		drawCenteredPropString("Score submitted for " + mCurrentPlayerName, 226, olc::WHITE);
		drawCenteredPropString("Press Enter or R to retry.", 250, olc::Pixel(255, 220, 170));
		drawCenteredPropString("Press M or Esc for menu.", 270, olc::Pixel(255, 220, 170));
	}

	void drawLeaderboardList(const olc::vi2d& topLeft)
	{
		if (mLeaderboard.empty())
		{
			DrawStringProp(topLeft, "No scores yet.", olc::Pixel(145, 163, 184));
			return;
		}

		const size_t visibleRows = std::min(LEADERBOARD_LIMIT, mLeaderboard.size());
		// Cap visible rows to keep the screen readable; profile management belongs elsewhere.
		for (size_t i = 0; i < visibleRows; ++i)
		{
			const auto& entry = mLeaderboard[i];
			const int y = topLeft.y + static_cast<int>(i) * 22;
			const bool isCurrentPlayer = entry.playerName == mCurrentPlayerName;
			const olc::Pixel nameColor = isCurrentPlayer ? olc::Pixel(255, 241, 183) : olc::WHITE;
			const olc::Pixel scoreColor = isCurrentPlayer ? olc::Pixel(244, 203, 73) : olc::Pixel(168, 191, 214);

			if (isCurrentPlayer)
			{
				FillRect({ topLeft.x - 6, y - 3 }, { 174, 17 }, olc::Pixel(37, 54, 73));
			}

			DrawStringProp({ topLeft.x, y }, std::to_string(i + 1) + ".", scoreColor);
			DrawStringProp({ topLeft.x + 20, y }, entry.playerName, nameColor);

			const std::string scoreText = std::to_string(entry.highScore);
			const olc::vi2d scoreSize = GetTextSizeProp(scoreText);
			DrawStringProp(
				{ topLeft.x + 170 - scoreSize.x, y },
				scoreText,
				scoreColor
			);
		}
	}

	void drawCard(const olc::vi2d& pos, const olc::vi2d& size, olc::Pixel fill, olc::Pixel border, olc::Pixel innerBorder)
	{
		FillRect(pos, size, fill);
		DrawRect(pos, size, border);
		DrawRect(pos + olc::vi2d(4, 4), size - olc::vi2d(8, 8), innerBorder);
	}

	void fillOverlay()
	{
		SetPixelMode(olc::Pixel::ALPHA);
		FillRect({ 0, 0 }, { ScreenWidth(), ScreenHeight() }, olc::Pixel(0, 0, 0, 140));
		SetPixelMode(olc::Pixel::NORMAL);
	}

	void drawCenteredPropString(const std::string& text, int y, olc::Pixel color, uint32_t scale = 1)
	{
		const olc::vi2d textSize = GetTextSizeProp(text) * static_cast<int32_t>(scale);
		DrawStringProp(
			{ (ScreenWidth() - textSize.x) / 2, y },
			text,
			color,
			scale
		);
	}

	bool isSnakeOnCell(const olc::vi2d& pos) const
	{
		for (const auto& body : mSnake.mBody)
		{
			if (body.pos == pos)
			{
				return true;
			}
		}

		return false;
	}

	bool isAppleOnCell(const olc::vi2d& pos) const
	{
		for (const auto& apple : mApples)
		{
			if (apple.mPos == pos)
			{
				return true;
			}
		}

		return false;
	}

	void addApples()
	{
		std::vector<olc::vi2d> freeCells;
		const int cols = ScreenWidth() / SNAKE_SIZE;
		const int rows = (ScreenHeight() - HUD_HEIGHT) / SNAKE_SIZE;
		freeCells.reserve(static_cast<size_t>(cols * rows));

		// Build the list of legal spawn cells first so late-game apple placement cannot
		// get stuck in an endless random retry loop.
		for (int y = 0; y < rows; ++y)
		{
			for (int x = 0; x < cols; ++x)
			{
				const olc::vi2d pos = {
					x * SNAKE_SIZE,
					HUD_HEIGHT + y * SNAKE_SIZE
				};

				if (!isSnakeOnCell(pos) && !isAppleOnCell(pos))
				{
					freeCells.push_back(pos);
				}
			}
		}

		if (freeCells.empty())
		{
			return;
		}

		std::ranges::shuffle(freeCells, mRandEngine);
		// Shuffle once, then take the first N cells instead of repeatedly retrying random rolls.
		const size_t applesNeeded = NUM_OF_APPLES - mApples.size();
		const size_t applesToAdd = std::min(applesNeeded, freeCells.size());

		for (size_t i = 0; i < applesToAdd; ++i)
		{
			mApples.push_back({ freeCells[i] });
		}
	}

	bool isEatApple()
	{
		const auto headPos = mSnake.getSnakeHead().pos;

		for (size_t i = 0; i < mApples.size(); ++i)
		{
			if (mApples[i].mPos == headPos)
			{
				mApples.erase(mApples.begin() + static_cast<long long>(i));
				mSnake.extendBody();
				++mScore;
				return true;
			}
		}

		return false;
	}

	void recordScoreIfNeeded()
	{
		if (mRunScoreSaved)
		{
			return;
		}

		// Save once per run when the player dies, even if the game-over screen stays open.
		upsertLeaderboardEntry(mCurrentPlayerName, mScore, makeTimestamp());
		refreshProfileNames();
		refreshMainMenuButtonLabels();
		saveProfileData();
		mRunScoreSaved = true;
	}

	void ensureCurrentProfileEntry()
	{
		const std::string sanitizedName = sanitizePlayerName(mCurrentPlayerName, true);
		mCurrentPlayerName = sanitizedName.empty() ? DEFAULT_PLAYER_NAME : sanitizedName;

		// Profiles and leaderboard entries are intentionally the same backing record:
		// a profile exists because it has a stored best-score entry.
		auto it = std::ranges::find_if(mLeaderboard,
			[&](const LeaderboardEntry& entry) { return entry.playerName == mCurrentPlayerName; }
		);

		if (it == mLeaderboard.end())
		{
			mLeaderboard.push_back({ mCurrentPlayerName, 0, "-" });
		}
	}

	void upsertLeaderboardEntry(const std::string& rawName, uint64_t score, const std::string& timestamp)
	{
		const std::string playerName = sanitizePlayerName(rawName, true);
		if (playerName.empty())
		{
			return;
		}

		auto it = std::ranges::find_if(mLeaderboard,
			[&](const LeaderboardEntry& entry) { return entry.playerName == playerName; }
		);

		if (it == mLeaderboard.end())
		{
			mLeaderboard.push_back({ playerName, score, timestamp.empty() ? "-" : timestamp });
		}
		// Only replace an existing entry when the run matches or beats that profile's best.
		else if (score >= it->highScore)
		{
			it->highScore = score;
			it->updatedAt = timestamp.empty() ? it->updatedAt : timestamp;
		}

		sortLeaderboard();
	}

	void sortLeaderboard()
	{
		std::sort(
			mLeaderboard.begin(),
			mLeaderboard.end(),
			[](const LeaderboardEntry& a, const LeaderboardEntry& b)
			{
				if (a.highScore != b.highScore)
				{
					return a.highScore > b.highScore;
				}

				return a.playerName < b.playerName;
			}
		);
	}

	uint64_t getCurrentPlayerHighScore() const
	{
		const auto it = std::find_if(
			mLeaderboard.begin(),
			mLeaderboard.end(),
			[&](const LeaderboardEntry& entry) { return entry.playerName == mCurrentPlayerName; }
		);

		return it == mLeaderboard.end() ? 0 : it->highScore;
	}

	std::string sanitizePlayerName(const std::string& rawName, bool useDefaultIfEmpty) const
	{
		std::string filtered;
		filtered.reserve(rawName.size());

		// Keep names predictable for UI layout and file persistence: trim controls,
		// collapse whitespace, and cap visible length.
		bool previousWasSpace = false;
		for (unsigned char ch : rawName)
		{
			if (std::iscntrl(ch))
			{
				continue;
			}

			if (std::isspace(ch))
			{
				if (!filtered.empty() && !previousWasSpace)
				{
					filtered.push_back(' ');
					previousWasSpace = true;
				}
				continue;
			}

			if (filtered.size() >= MAX_PLAYER_NAME_LENGTH)
			{
				break;
			}

			filtered.push_back(static_cast<char>(ch));
			previousWasSpace = false;
		}

		while (!filtered.empty() && filtered.front() == ' ')
		{
			filtered.erase(filtered.begin());
		}

		while (!filtered.empty() && filtered.back() == ' ')
		{
			filtered.pop_back();
		}

		if (filtered.empty() && useDefaultIfEmpty)
		{
			return DEFAULT_PLAYER_NAME;
		}

		return filtered;
	}

	std::string makeTimestamp() const
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t timeNow = std::chrono::system_clock::to_time_t(now);
		std::tm localTime{};
		if (localtime_s(&localTime, &timeNow) != 0)
		{
			throw std::runtime_error("Failed to get local time for timestamp.");
		}

		std::ostringstream out;
		out << std::put_time(&localTime, "%Y-%m-%d %H:%M");
		return out.str();
	}

private:
	Snake mSnake;
	std::vector<Apple> mApples;
	std::vector<LeaderboardEntry> mLeaderboard;
	uint64_t mScore;
	float mMoveTimer;
	GameState mState = GameState::MainMenu;
	bool mShouldExit = false;
	bool mRunScoreSaved = false;

	std::string mCurrentPlayerName = DEFAULT_PLAYER_NAME;
	std::string mSettingsStatusText = "Change options, then press Apply.";
	std::string mProfileStatusText = "Select a profile or create a new one.";
	olc::vi2d mWorkAreaSize = { 1024, 1024 };
	std::vector<ResolutionOption> mResolutionOptions;
	std::vector<std::string> mResolutionLabels;
	std::vector<std::string> mProfileNames;
	size_t mSelectedResolutionIndex = 0;
	size_t mSelectedProfileIndex = 0;
	fs::path mProfilePath;
	fs::path mSettingsPath;
	AppConfig mAppliedConfig;
	AppConfig mDraftConfig;
	std::chrono::steady_clock::time_point mNextFrameDeadline{};

	std::default_random_engine mRandEngine;
	std::uniform_int_distribution<int32_t> mRandX;
	std::uniform_int_distribution<int32_t> mRandY;

	olc::QuickGUI::Manager mMainMenuGui;
	olc::QuickGUI::Manager mLeaderboardGui;
	olc::QuickGUI::Manager mPauseMenuGui;
	olc::QuickGUI::Manager mSettingsGui;
	olc::QuickGUI::Manager mProfileGui;
	olc::QuickGUI::ListBox* mResolutionListBox = nullptr;
	olc::QuickGUI::ListBox* mProfileListBox = nullptr;
	olc::QuickGUI::Button* mButtonMainStart = nullptr;
	olc::QuickGUI::Button* mButtonMainLeaderboard = nullptr;
	olc::QuickGUI::Button* mButtonMainSettings = nullptr;
	olc::QuickGUI::Button* mButtonMainQuit = nullptr;
	olc::QuickGUI::Button* mButtonMainProfile = nullptr;
	olc::QuickGUI::Button* mButtonLeaderboardBack = nullptr;
	olc::QuickGUI::Button* mButtonPauseResume = nullptr;
	olc::QuickGUI::Button* mButtonPauseRestart = nullptr;
	olc::QuickGUI::Button* mButtonPauseMenu = nullptr;
	olc::QuickGUI::Button* mButtonSettingsVSync = nullptr;
	olc::QuickGUI::Button* mButtonSettingsFrameRate = nullptr;
	olc::QuickGUI::Button* mButtonSettingsWindowMode = nullptr;
	olc::QuickGUI::Button* mButtonSettingsApply = nullptr;
	olc::QuickGUI::Button* mButtonSettingsBack = nullptr;
	olc::QuickGUI::Button* mButtonProfileUse = nullptr;
	olc::QuickGUI::Button* mButtonProfileCreate = nullptr;
	olc::QuickGUI::Button* mButtonProfileRemove = nullptr;
	olc::QuickGUI::Button* mButtonProfileBack = nullptr;
};

#ifndef _DEBUG
int WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main()
#endif
{
	const fs::path settingsPath = resolveExecutableFilePath("snake_settings.dat");
	const AppConfig startupConfig = loadAppConfig(settingsPath);

	Game game(startupConfig);
	if (game.Construct(
		SCREEN_SIZE,
		SCREEN_SIZE,
		PIXEL_SCALE,
		PIXEL_SCALE,
		startupConfig.windowMode == WindowMode::Fullscreen,
		startupConfig.vsync
	))
	{
		game.Start();
	}

	return 0;
}
