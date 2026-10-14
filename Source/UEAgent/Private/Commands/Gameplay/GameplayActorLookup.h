#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

// The AIController lookup shared by the behavior tree and AI perception command families.
namespace GameplayActorLookup
{
    /** The AIController Subject is, or the one possessing Subject when it is a pawn; nullptr otherwise. */
    inline AAIController* FindAIController(AActor* Subject)
    {
        AAIController* AICon = Cast<AAIController>(Subject);
        if (!AICon)
        {
            if (APawn* Pawn = Cast<APawn>(Subject))
            {
                AICon = Cast<AAIController>(Pawn->GetController());
            }
        }
        return AICon;
    }
}
