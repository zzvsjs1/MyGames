#define OLC_PGE_APPLICATION
#include "olcPixelGameEngine.h"

#include <random>
#include <vector>

namespace dir_vec
{
	const olc::vi2d UP = { 0, -1 };
	const olc::vi2d DOWN = { 0, 1 };
	const olc::vi2d LEFT = { -1, 0 };
	const olc::vi2d RIGHT = { 1, 0 };
}

constexpr int DEFAULT_SNAKE_LEN = 3;
constexpr int SNAKE_SIZE = 8;
constexpr int NUM_OF_APPLES = 3;
constexpr float MOVE_INTERVAL = 0.12f;

struct Apple
{
	olc::vi2d mPos;

	void draw(olc::PixelGameEngine& engine) const
	{
		engine.FillRect(mPos, { SNAKE_SIZE, SNAKE_SIZE }, olc::RED);
	}
};

struct Snake
{
	struct SnakeBody
	{
		olc::vi2d pos;
	};

	std::vector<SnakeBody> mBody;
	olc::vi2d mDirVec2d = dir_vec::LEFT;

	Snake() = default;

	Snake(int32_t width, int32_t height)
	{
		reset(width, height);
	}

	void reset(int32_t width, int32_t height)
	{
		mBody.clear();
		mDirVec2d = dir_vec::LEFT;

		const int startX = ((width / 2) / SNAKE_SIZE) * SNAKE_SIZE;
		const int startY = ((height / 2) / SNAKE_SIZE) * SNAKE_SIZE;

		// Head is at front, body extends to the right
		for (int i = 0; i < DEFAULT_SNAKE_LEN; ++i)
		{
			mBody.push_back({ { startX + i * SNAKE_SIZE, startY } });
		}
	}

	void stepUpdate()
	{
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
			engine.FillRect(
				mBody[i].pos,
				{ SNAKE_SIZE, SNAKE_SIZE },
				(i == 0) ? olc::YELLOW : olc::GREEN
			);
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

	bool isDeath(int32_t width, int32_t height) const noexcept
	{
		const auto& headPos = getSnakeHead().pos;

		if (headPos.x < 0 || headPos.y < 0 ||
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

class Game final : public olc::PixelGameEngine
{
public:
	Game()
		: olc::PixelGameEngine()
		, mScore(0)
		, isOver(false)
		, mMoveTimer(0.0f)
		, mRandEngine(std::random_device{}())
	{
		sAppName = "Snake";
	}

	bool OnUserCreate() override
	{
		rebuildRandomRange();
		resetGame();
		return true;
	}

	bool OnUserUpdate(float fElapsedTime) override
	{
		Clear(olc::BLACK);

		handleKeyInput();

		if (!isOver)
		{
			mMoveTimer += fElapsedTime;

			while (mMoveTimer >= MOVE_INTERVAL)
			{
				mMoveTimer -= MOVE_INTERVAL;

				mSnake.stepUpdate();

				if (isEatApple())
				{
					addApples();
				}

				isOver = mSnake.isDeath(ScreenWidth(), ScreenHeight());
				if (isOver)
					break;
			}
		}

		mSnake.draw(*this);
		for (const auto& apple : mApples)
			apple.draw(*this);

		DrawString(2, 2, "Score: " + std::to_string(mScore), olc::WHITE);

		if (isOver)
		{
			DrawString(40, ScreenHeight() / 2 - 10, "Game Over", olc::RED);
			DrawString(20, ScreenHeight() / 2 + 10, "Press R to restart", olc::WHITE);
		}

		return true;
	}

private:
	void rebuildRandomRange()
	{
		const int cols = ScreenWidth() / SNAKE_SIZE;
		const int rows = ScreenHeight() / SNAKE_SIZE;

		mRandX = std::uniform_int_distribution<int32_t>(0, cols - 1);
		mRandY = std::uniform_int_distribution<int32_t>(0, rows - 1);
	}

	void resetGame()
	{
		mApples.clear();
		mScore = 0;
		isOver = false;
		mMoveTimer = 0.0f;

		mSnake.reset(ScreenWidth(), ScreenHeight());
		addApples();
	}

	bool isSnakeOnCell(const olc::vi2d& pos) const
	{
		for (const auto& body : mSnake.mBody)
		{
			if (body.pos == pos)
				return true;
		}
		return false;
	}

	bool isAppleOnCell(const olc::vi2d& pos) const
	{
		for (const auto& apple : mApples)
		{
			if (apple.mPos == pos)
				return true;
		}
		return false;
	}

	void addApples()
	{
		while (mApples.size() < NUM_OF_APPLES)
		{
			olc::vi2d pos = {
				mRandX(mRandEngine) * SNAKE_SIZE,
				mRandY(mRandEngine) * SNAKE_SIZE
			};

			if (isSnakeOnCell(pos) || isAppleOnCell(pos))
				continue;

			mApples.push_back({ pos });
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

	void handleKeyInput()
	{
		if (GetKey(olc::Key::R).bPressed)
		{
			resetGame();
			return;
		}

		if (isOver)
			return;

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

private:
	Snake mSnake;
	std::vector<Apple> mApples;
	uint64_t mScore;
	bool isOver;
	float mMoveTimer;

	std::default_random_engine mRandEngine;
	std::uniform_int_distribution<int32_t> mRandX;
	std::uniform_int_distribution<int32_t> mRandY;
};

#ifndef _DEBUG
int WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main()
#endif
{
	Game game;
	if (game.Construct(256, 256, 4, 4, false, true))
	{
		game.Start();
	}
	return 0;
}