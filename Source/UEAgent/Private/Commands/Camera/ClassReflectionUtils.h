// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

// Reflection helpers for the commands that set an object's properties by name.
namespace ClassReflectionUtils
{
    /** Writes Value to the property Name of Target; false, with OutError, when it cannot. */
    using FPropertyWriter = bool (*)(UObject* Target, const FString& Name, const TSharedPtr<FJsonValue>& Value, FString& OutError);

    /** The outcome of WriteProperties, as a set-properties reply reports it. */
    struct FPropertyWrites
    {
        /** The names written. */
        TArray<TSharedPtr<FJsonValue>> Set;
        /** {name, error} of each name not written. */
        TArray<TSharedPtr<FJsonValue>> Failed;
        /** The value each named property of Target stores afterwards, as export text, written or not. */
        TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    };

    /** Writes each entry of Properties to Target with Write and reads back what every named property stores. */
    inline FPropertyWrites WriteProperties(UObject* Target, const FJsonObject& Properties, FPropertyWriter Write)
    {
        FPropertyWrites Writes;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties.Values)
        {
            FString Error;
            if (Write(Target, Pair.Key, Pair.Value, Error))
            {
                Writes.Set.Add(MakeShared<FJsonValueString>(Pair.Key));
            }
            else
            {
                TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
                Failure->SetStringField(TEXT("name"), Pair.Key);
                Failure->SetStringField(TEXT("error"), Error);
                Writes.Failed.Add(MakeShared<FJsonValueObject>(Failure));
            }
            if (FProperty* Property = Target->GetClass()->FindPropertyByName(*Pair.Key))
            {
                FString StoredText;
                Property->ExportTextItem_Direct(StoredText, Property->ContainerPtrToValuePtr<void>(Target), nullptr, Target, PPF_None);
                Writes.Applied->SetStringField(Pair.Key, StoredText);
            }
        }
        return Writes;
    }
}
