/* DDNet TAS Patch — tas.h */

#pragma once

#include <engine/console.h>
#include <game/client/component.h>
#include <game/client/prediction/gameworld.h>

#include <generated/protocol.h>
#include <vector>

struct STasInput
{
	int m_Direction;
	int m_TargetX;
	int m_TargetY;
	int m_Jump;
	int m_Fire;
	int m_Hook;
	int m_PlayerFlags;
	int m_WantedWeapon;
	int m_NextWeapon;
	int m_PrevWeapon;
};

struct STasFrame
{
	int       m_Tick;
	STasInput m_Input;
};

enum class ETasMode
{
	IDLE,
	RECORDING,
	PLAYBACK,
};

class CTas : public CComponent
{
public:
	CTas();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnConsoleInit() override;

	bool GetPlaybackInput(int *pData) const;
	bool IsPlayback()  const { return m_Mode == ETasMode::PLAYBACK; }
	bool IsRecording() const { return m_Mode == ETasMode::RECORDING; }

private:
	ETasMode m_Mode = ETasMode::IDLE;

	std::vector<STasFrame> m_vFrames;
	int   m_PlaybackIndex   = 0;

	// Ghost world
	CGameWorld        m_GhostWorld;
	bool              m_GhostWorldReady = false;
	int               m_GhostTick       = 0;

	// Ghost render data (CNetObj_Character для RenderPlayer)
	CNetObj_Character m_GhostCharCur;
	CNetObj_Character m_GhostCharPrev;

	// Slowmo
	float m_Slowmo      = 0.25f;
	float m_SlowmoAccum = 0.0f;
	bool  m_Paused      = false;
	bool  m_FrameAdvance= false;

	STasInput m_PrevGhostInput = {};
	int m_LastGameTick = -1;

	static void ConTasRecord      (IConsole::IResult *pResult, void *pUser);
	static void ConTasStopRecord  (IConsole::IResult *pResult, void *pUser);
	static void ConTasPlay        (IConsole::IResult *pResult, void *pUser);
	static void ConTasStop        (IConsole::IResult *pResult, void *pUser);
	static void ConTasRewind      (IConsole::IResult *pResult, void *pUser);
	static void ConTasSlowmo      (IConsole::IResult *pResult, void *pUser);
	static void ConTasClear       (IConsole::IResult *pResult, void *pUser);
	static void ConTasFrameAdvance(IConsole::IResult *pResult, void *pUser);
	static void ConTasPause       (IConsole::IResult *pResult, void *pUser);

	void StartRecord();
	void StopRecord();
	void StartPlayback();
	void Stop();
	void DoRewind(int Ticks);
	void StepGhostWorld();
	void InitGhostWorld();
	void RenderGhost();
	void RenderHud();
	const char *ModeStr() const;
};
