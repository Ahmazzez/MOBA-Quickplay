#include "MistCryptSaveGame.h"

UMistCryptSaveGame::UMistCryptSaveGame()
{
    CooldownSeconds.Init(0.f, 6);
    SavedUtcTicks = FDateTime::UtcNow().GetTicks();
}
