/* DDNet TAS Patch — tas.h
 *
 * Ghost-World TAS:
 *   При tas_record основной персонаж ЗАМИРАЕТ на сервере (отправляем нулевой инпут).
 *   Локально запускается "призрачный мир" (копия PredictedWorld) со slowmo.
 *   Ты управляешь призраком — каждый его тик записывается.
 *   tas_stoprecord — запись применяется, персонаж начинает двигаться по записи.
 *   tas_play — воспроизводит запись (подаёт инпуты на сервер потик).
 *   tas_rewind [N] — стирает последние N тиков записи.
 *   tas_slowmo <0.05-1.0> — скорость ghost-мира.
 *   tas_pause / tas_frame_advance — пауза и покадровое управление.
 *   tas_clear — очистить запись.
 *
 * Биндинги (autoexec.cfg):
 *   bind f4  tas_rewind
 *   bind f5  tas_frame_advance
 *   bind f6  tas_pause
 *   bind f7  tas_record
 *   bind f8  tas_stoprecord
 *   bind f9  tas_play
 *   bind f10 tas_stop
 */

#pragma once

#include <engine/console.h>
#include <game/client/component.h>
#include <game/client/prediction/gameworld.h>

#include <vector>

// Один записанный тик (поля CNetObj_PlayerInput без include generated/protocol.h в хедере)
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
	RECORDING,   // ghost-мир активен, основной персонаж заморожен
	PLAYBACK,    // воспроизводим запись на сервер
};

class CTas : public CComponent
{
public:
	CTas();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnConsoleInit() override;

	// Вызывается из OnSnapInput
	bool GetPlaybackInput(int *pData) const;
	bool IsPlayback() const { return m_Mode == ETasMode::PLAYBACK; }

	// Вызывается из OnSnapInput при recording — замораживаем персонажа
	bool IsRecording() const { return m_Mode == ETasMode::RECORDING; }

	// Рендер ghost-персонажа (вызывается из компонента Players или отдельно)
	void RenderGhost();

private:
	ETasMode m_Mode = ETasMode::IDLE;

	// Запись
	std::vector<STasFrame> m_vFrames;
	int   m_PlaybackIndex   = 0;

	// Ghost-мир — копия предсказательного мира со slowmo
	CGameWorld m_GhostWorld;
	bool       m_GhostWorldReady = false;
	int        m_GhostTick       = 0;   // тик внутри ghost-мира

	// Slowmo: накапливаем дробное время
	float m_Slowmo        = 0.25f; // 0.25 = 25% скорости
	float m_SlowmoAccum   = 0.0f;  // накопленное время
	bool  m_Paused        = false;
	bool  m_FrameAdvance  = false;

	// Предыдущий инпут ghost-мира (для корректного счётчика прыжков/огня)
	STasInput m_PrevGhostInput = {};

	int m_LastGameTick = -1;

	// Консольные команды
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
	void StepGhostWorld();       // продвинуть ghost-мир на 1 тик
	void InitGhostWorld();       // скопировать текущий predicted world в ghost
	void RenderHud();
	const char *ModeStr() const;
};
