// Copyright Epic Games, Inc. All Rights Reserved.

#include "dev_uedemoGameMode.h"
#include "dev_uedemoCharacter.h"
#include "UObject/ConstructorHelpers.h"

Adev_uedemoGameMode::Adev_uedemoGameMode()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}
