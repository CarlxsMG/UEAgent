#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class FCompilerResultsLog;
class UClass;
class UObject;
class UWorld;

/** Asset and object lookups, error replies and reply pieces shared by several command families. */
namespace AssetCommandHelpers
{
    /** "Parameter '<ParamName>' cannot be empty". */
    TSharedPtr<FJsonObject> EmptyParamError(const TCHAR* ParamName);

    /** Null while Value lies inside [Lowest, Highest]; otherwise the range error the readers send back. */
    TSharedPtr<FJsonObject> RangeError(const TCHAR* ParamName, int32 Value, int32 Lowest, int32 Highest);

    /** "Asset not found: <Name>". */
    TSharedPtr<FJsonObject> AssetNotFoundError(const FString& Name);

    /** An asset found by exact name, plus every same-named candidate when the /Game scan was ambiguous. */
    struct FNamedAsset
    {
        FAssetData Asset;
        TArray<TSharedPtr<FJsonValue>> AmbiguousMatches;
    };

    /**
     * The by-name lookup of the asset readers. A full package name is tried first (and is the only form that reaches
     * outside /Game); otherwise every /Game asset -- of exactly ExactClass when one is given, subclasses excluded --
     * whose short name is Name is a candidate. The first candidate is the one used; when the scan finds several they
     * are all listed, so the caller can show that the pick was arbitrary.
     */
    bool FindAssetByExactName(const FString& Name, const UClass* ExactClass, FNamedAsset& Out);

    /** Reply["ambiguous_matches"] = Candidates, when there are any. */
    void AppendAmbiguity(const TSharedPtr<FJsonObject>& Reply, const TArray<TSharedPtr<FJsonValue>>& Candidates);

    /**
     * The reply of Command refusing to delete Asset, which is not an ExpectedClassName: success false, its error naming
     * the class found, and { asset_path, asset_class, deleted: false, note } as its data. Nothing is deleted.
     */
    TSharedPtr<FJsonObject> WrongTypeRefusal(const TCHAR* Command, const FString& AssetPath, const UObject& Asset,
        const FString& ExpectedClassName);

    /**
     * The reply of Command once it has asked the engine to delete the asset at AssetPath: Data as it is, with success
     * bDeleted. A delete the engine refused is an error that names the usual causes.
     */
    TSharedPtr<FJsonObject> DeleteOutcomeReply(const TCHAR* Command, const TSharedPtr<FJsonObject>& Data, bool bDeleted,
        const FString& AssetPath);

    /**
     * The whole reply of a typed remove command: deletes the asset at AssetPath only when it loads as an ExpectedClass.
     * A path that does not load is a no-op, success with { asset_path, asset_class: "", deleted: false,
     * already_existed: false, note }; an asset of another class is WrongTypeRefusal; otherwise the data is
     * { asset_path, asset_class, package_name, deleted, already_existed: true } and success is whether the engine
     * deleted it (DeleteOutcomeReply).
     */
    TSharedPtr<FJsonObject> DeleteTypedAsset(const TCHAR* Command, const FString& AssetPath, const UClass* ExpectedClass);

    /**
     * The object a dotted "Actor[.Component[.Subobject]]" path names in World: the first segment matches a live actor
     * by object name or editor label, each further segment a component of that actor by name or any other subobject
     * by name. Null when World is null or a segment does not resolve.
     */
    UObject* FindDottedPathInWorld(const FString& Path, UWorld* World);

    /** Every message of a compiler results log as { severity: "error" | "warning" | "info", message }. */
    TArray<TSharedPtr<FJsonValue>> CompilerMessagesToJson(const FCompilerResultsLog& Results);

    /**
     * The default object of Class when it is a current native class (not Blueprint-generated, deprecated or
     * superseded) with a live CDO -- the CDOs the engine's asset-rename modal checks for references. Null otherwise.
     */
    UObject* NativeClassDefault(UClass* Class);

    /**
     * How many characters the longest package name the engine derives from a map's package name adds to it, for a
     * map whose actors live in their own packages (World Partition, One File Per Actor). Each actor is packaged at
     * <mount>/__ExternalActors__/<rest of the map path>/<2>/<2>/<21> (ULevel::GetActorPackageName: 47 more) and each
     * actor folder at <mount>/__ExternalObjects__/<rest>/<1>/<2>/<22> (FExternalPackageHelper::GetExternalPackageName:
     * 48 more), the last three parts being a 25-character base-36 GUID.
     */
    constexpr int32 MapExternalPackageOverhead = 48;

    /**
     * Why MapPackageName (a package or object path) cannot name a map whose actors live in their own packages, or empty
     * when it can. Two kinds of names derived from it reach NAME_SIZE first, and FName's checkf aborts the editor on
     * either: the paths of the world's objects (":PersistentLevel.WorldSettings", the minimap actor and its sprite,
     * each placed actor and its components), which mcp_common::InvalidPackageName limits through the object path; and
     * the packages of the actors and actor folders, MapExternalPackageOverhead past the package name, which that limit
     * already keeps under NAME_SIZE. So the longest map object path accepted is NAME_SIZE - 1 -
     * mcp_common::ObjectPathSubobjectMargin, or less for a long map name (InvalidPackageName says how much).
     */
    FString MapPathTooLongForExternalPackages(const FString& MapPackageName);

    /**
     * How many characters the path of the deepest object inside the loaded Asset reaches past Asset's own object path,
     * measured over every live object outered to it, recursively (a world's persistent level, its loaded actors and
     * their components included); OutDeepestSuffix, when given, gets that object's path past the object path. 0 for null.
     */
    int32 DeepestSubobjectPathDepth(const UObject* Asset, FString* OutDeepestSuffix = nullptr);

    /**
     * Why copying or renaming the loaded Asset to NewObjectPath (a package or object path) would name one of the objects
     * it carries past the FName limit, or empty: mcp_common::InvalidPackageName(NewObjectPath) as the floor, then the
     * destination's object path plus Asset's measured DeepestSubobjectPathDepth. Without Asset, the floor only.
     */
    FString CopiedObjectPathsTooLong(const UObject* Asset, const FString& NewObjectPath);
}
