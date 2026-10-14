#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Gameplay/GameplayActorLookup.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"

// AIModule (engine runtime module; enabled via AIModule dependency in UEAgent.Build.cs)
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/Blackboard/BlackboardKey.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
// AI perception runtime — ConfigureSense / GetCurrentlyPerceivedActors
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig.h"
#include "Perception/AISenseConfig_Sight.h"

/**
 * MCP domain for AI behavior: behavior tree and blackboard ASSETS (inspection and authoring of
 * keys, composites, tasks, decorators and services) and their RUNTIME side on AI-driven actors
 * (running, pausing and stopping trees, reading and writing blackboard values).
 */
namespace UEAgentBehaviorTreeCommands
{

// MCP-DOMAIN: BehaviorTree

namespace
{
    /** Map a friendly key-type token ("Bool"/"Int"/...) to its UBlackboardKeyType subclass. */
    UClass* ResolveBlackboardKeyTypeClass(const FString& TypeToken)
    {
        const FString T = TypeToken.TrimStartAndEnd();
        if (T.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))   { return UBlackboardKeyType_Bool::StaticClass(); }
        if (T.Equals(TEXT("Int"), ESearchCase::IgnoreCase))    { return UBlackboardKeyType_Int::StaticClass(); }
        if (T.Equals(TEXT("Float"), ESearchCase::IgnoreCase))  { return UBlackboardKeyType_Float::StaticClass(); }
        if (T.Equals(TEXT("Vector"), ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Vector::StaticClass(); }
        if (T.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase)){ return UBlackboardKeyType_Rotator::StaticClass(); }
        if (T.Equals(TEXT("String"), ESearchCase::IgnoreCase)) { return UBlackboardKeyType_String::StaticClass(); }
        if (T.Equals(TEXT("Name"), ESearchCase::IgnoreCase))   { return UBlackboardKeyType_Name::StaticClass(); }
        if (T.Equals(TEXT("Object"), ESearchCase::IgnoreCase)) { return UBlackboardKeyType_Object::StaticClass(); }
        if (T.Equals(TEXT("Class"), ESearchCase::IgnoreCase))  { return UBlackboardKeyType_Class::StaticClass(); }
        if (T.Equals(TEXT("Enum"), ESearchCase::IgnoreCase))   { return UBlackboardKeyType_Enum::StaticClass(); }
        return nullptr;
    }

    /** Human-readable name for a UBlackboardKeyType subclass. */
    FString DescribeKeyTypeClass(const UBlackboardKeyType* KeyType)
    {
        if (!KeyType)
        {
            return TEXT("None");
        }
        // e.g. "BlackboardKeyType_Int" -> "Int"
        FString ClassName = KeyType->GetClass()->GetName();
        FString Prefix(TEXT("BlackboardKeyType_"));
        if (ClassName.StartsWith(Prefix))
        {
            return ClassName.RightChop(Prefix.Len());
        }
        return ClassName;
    }

    /**
     * Add the subsettings set_blackboard_key_subsettings authors to Obj: base_class for an Object/Class key,
     * enum_type and enum_name for an Enum key (all public UPROPERTYs). Other key types add nothing.
     */
    void BehaviorTree_AddKeySubsettings(const TSharedPtr<FJsonObject>& Obj, const UBlackboardKeyType* KeyType)
    {
        if (const UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(KeyType))
        {
            Obj->SetStringField(TEXT("base_class"), ObjKey->BaseClass ? ObjKey->BaseClass->GetPathName() : FString());
        }
        else if (const UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(KeyType))
        {
            Obj->SetStringField(TEXT("base_class"), ClassKey->BaseClass ? ClassKey->BaseClass->GetPathName() : FString());
        }
        else if (const UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(KeyType))
        {
            Obj->SetStringField(TEXT("enum_type"), EnumKey->EnumType ? EnumKey->EnumType->GetPathName() : FString());
            Obj->SetStringField(TEXT("enum_name"), EnumKey->EnumName);
        }
    }

    /** Build a JSON object describing one blackboard entry. */
    TSharedPtr<FJsonObject> MakeKeyJson(const FBlackboardEntry& Entry)
    {
        TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
        KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
        KeyObj->SetStringField(TEXT("type"), DescribeKeyTypeClass(Entry.KeyType));
        KeyObj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced != 0);
        BehaviorTree_AddKeySubsettings(KeyObj, Entry.KeyType);
        return KeyObj;
    }

    /** MakeKeyJson of every key of BBData. */
    TArray<TSharedPtr<FJsonValue>> BehaviorTree_KeysToJson(const UBlackboardData* BBData)
    {
        TArray<TSharedPtr<FJsonValue>> KeysArr;
        for (const FBlackboardEntry& Entry : BBData->GetKeys())
        {
            KeysArr.Add(MakeShared<FJsonValueObject>(MakeKeyJson(Entry)));
        }
        return KeysArr;
    }

    /** { <PathField>, name, parent?, keys, key_count } of a blackboard asset. */
    TSharedPtr<FJsonObject> BehaviorTree_BlackboardAssetData(const UBlackboardData* BBData, const TCHAR* PathField)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(PathField, BBData->GetPathName());
        Result->SetStringField(TEXT("name"), BBData->GetName());
        if (BBData->Parent)
        {
            Result->SetStringField(TEXT("parent"), BBData->Parent->GetPathName());
        }

        const TArray<TSharedPtr<FJsonValue>> KeysArr = BehaviorTree_KeysToJson(BBData);
        Result->SetArrayField(TEXT("keys"), KeysArr);
        Result->SetNumberField(TEXT("key_count"), KeysArr.Num());
        return Result;
    }

    /** The optional index param as an int32, or -1 when it is absent or not a number. */
    int32 BehaviorTree_ReadIndexParam(const TSharedPtr<FJsonObject>& Params)
    {
        double IdxNum = 0.0;
        return Params->TryGetNumberField(TEXT("index"), IdxNum) ? static_cast<int32>(IdxNum) : -1;
    }

    /** Recursively serialize a behavior tree node graph. Bounded by depth to avoid runaway. */
    TSharedPtr<FJsonObject> SerializeBTNode(const UBTNode* Node, int32 Depth)
    {
        TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
        if (!Node)
        {
            return NodeObj;
        }

        NodeObj->SetStringField(TEXT("name"), Node->GetNodeName());
        NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());

        if (Depth >= 64)
        {
            return NodeObj;
        }

        if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
        {
            // Background services run while this composite is active (per composite).
            TArray<TSharedPtr<FJsonValue>> ServicesArr;
            for (const UBTService* Service : Composite->Services)
            {
                if (Service)
                {
                    TSharedPtr<FJsonObject> SvcObj = MakeShared<FJsonObject>();
                    SvcObj->SetStringField(TEXT("name"), Service->GetNodeName());
                    SvcObj->SetStringField(TEXT("class"), Service->GetClass()->GetName());
                    ServicesArr.Add(MakeShared<FJsonValueObject>(SvcObj));
                }
            }
            NodeObj->SetArrayField(TEXT("services"), ServicesArr);

            TArray<TSharedPtr<FJsonValue>> ChildrenArr;
            for (const FBTCompositeChild& Child : Composite->Children)
            {
                const UBTNode* ChildNode = Child.ChildComposite
                    ? static_cast<const UBTNode*>(Child.ChildComposite)
                    : static_cast<const UBTNode*>(Child.ChildTask);
                if (ChildNode)
                {
                    TSharedPtr<FJsonObject> ChildObj = SerializeBTNode(ChildNode, Depth + 1);
                    // Execution decorators (conditions) gate this child-edge (per FBTCompositeChild).
                    TArray<TSharedPtr<FJsonValue>> DecoratorsArr;
                    for (const UBTDecorator* Decorator : Child.Decorators)
                    {
                        if (Decorator)
                        {
                            TSharedPtr<FJsonObject> DecObj = MakeShared<FJsonObject>();
                            DecObj->SetStringField(TEXT("name"), Decorator->GetNodeName());
                            DecObj->SetStringField(TEXT("class"), Decorator->GetClass()->GetName());
                            DecoratorsArr.Add(MakeShared<FJsonValueObject>(DecObj));
                        }
                    }
                    ChildObj->SetArrayField(TEXT("decorators"), DecoratorsArr);
                    ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildObj));
                }
            }
            NodeObj->SetArrayField(TEXT("children"), ChildrenArr);
        }

        return NodeObj;
    }

    // --- BT child authoring helpers ------------------------------------
    // Resolve a concrete (non-abstract) subclass of Base from a short or full name.
    // Accepts e.g. "Wait"/"BTTask_Wait"/"UBTTask_Wait"/"/Script/AIModule.BTTask_Wait"
    // (Prefix = "BTTask_") and the composite equivalents (Prefix = "BTComposite_").
    UClass* ResolveBTNodeClass(const FString& In, UClass* Base, const TCHAR* Prefix)
    {
        if (In.IsEmpty() || !Base)
        {
            return nullptr;
        }
        auto Accept = [Base](UClass* C) -> UClass*
        {
            return (C && C->IsChildOf(Base) && !C->HasAnyClassFlags(CLASS_Abstract)) ? C : nullptr;
        };
        if (In.StartsWith(TEXT("/Script/")))
        {
            if (UClass* C = Accept(LoadObject<UClass>(nullptr, *In))) { return C; }
        }
        TArray<FString> Cands;
        Cands.Add(In);
        if (!In.StartsWith(Prefix)) { Cands.Add(FString(Prefix) + In); }
        for (const FString& N : Cands)
        {
            if (UClass* C = Accept(UClass::TryFindTypeSlow<UClass>(N))) { return C; }
            if (UClass* C = Accept(UClass::TryFindTypeSlow<UClass>(FString(TEXT("U")) + N))) { return C; }
            if (UClass* C = Accept(LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s"), *N)))) { return C; }
            if (UClass* C = Accept(FindFirstObject<UClass>(*N, EFindFirstObjectOptions::None))) { return C; }
        }
        return nullptr;
    }

    // Recursively find a composite node (by its NodeName) anywhere under Root.
    UBTCompositeNode* FindCompositeByName(UBTCompositeNode* Node, const FString& Name)
    {
        if (!Node)
        {
            return nullptr;
        }
        if (Node->NodeName == Name)
        {
            return Node;
        }
        for (const FBTCompositeChild& Child : Node->Children)
        {
            if (Child.ChildComposite)
            {
                if (UBTCompositeNode* Found = FindCompositeByName(Child.ChildComposite, Name))
                {
                    return Found;
                }
            }
        }
        return nullptr;
    }

    // Find a direct child of Parent whose node name matches (for idempotency).
    const UBTNode* FindChildByName(const UBTCompositeNode* Parent, const FString& Name)
    {
        if (!Parent)
        {
            return nullptr;
        }
        for (const FBTCompositeChild& Child : Parent->Children)
        {
            const UBTNode* C = Child.ChildComposite
                ? static_cast<const UBTNode*>(Child.ChildComposite)
                : static_cast<const UBTNode*>(Child.ChildTask);
            if (C && C->GetNodeName() == Name)
            {
                return C;
            }
        }
        return nullptr;
    }

    // Find the mutable child-edge (FBTCompositeChild) under Parent whose child node
    // matches Name — the anchor a decorator attaches to (decorators live per child-edge).
    FBTCompositeChild* FindChildEdgeByName(UBTCompositeNode* Parent, const FString& Name)
    {
        if (!Parent)
        {
            return nullptr;
        }
        for (FBTCompositeChild& Child : Parent->Children)
        {
            const UBTNode* C = Child.ChildComposite
                ? static_cast<const UBTNode*>(Child.ChildComposite)
                : static_cast<const UBTNode*>(Child.ChildTask);
            if (C && C->GetNodeName() == Name)
            {
                return &Child;
            }
        }
        return nullptr;
    }

    /** A child, decorator or service command's reply, opened with the tree, the resolved parent composite and the node. */
    TSharedPtr<FJsonObject> BehaviorTree_NodeReply(const UBehaviorTree* BTAsset, const FString& ParentName, const TCHAR* NodeField,
        const FString& NodeName)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("behavior_tree_path"), BTAsset->GetPathName());
        Reply->SetStringField(TEXT("parent_name"), ParentName);
        Reply->SetStringField(NodeField, NodeName);
        return Reply;
    }

    /**
     * The position in Nodes (Decorators or Services) of the node a remove command names, and its name: Index when set
     * (already range-checked), else the first node called Name; INDEX_NONE when none matches.
     */
    template <typename TNodeArray>
    int32 BehaviorTree_FindAuxNode(const TNodeArray& Nodes, int32 Index, const FString& Name, FString& OutFoundName)
    {
        if (Index >= 0)
        {
            const UBTNode* AtIdx = Nodes[Index];
            OutFoundName = AtIdx ? AtIdx->GetNodeName() : FString();
            return Index;
        }
        for (int32 i = 0; i < Nodes.Num(); ++i)
        {
            const UBTNode* Node = Nodes[i];
            if (Node && Node->GetNodeName() == Name)
            {
                OutFoundName = Name;
                return i;
            }
        }
        return INDEX_NONE;
    }

    /**
     * Load the behavior tree at BTPath and resolve the composite ParentName names in it: the root composite for an
     * empty name, "Root" or "RootNode", else the composite with that NodeName. On failure returns nullptr and fills
     * OutError: NoRootError when the tree has no root composite, a not-found error naming NotFoundLabel otherwise.
     */
    UBTCompositeNode* BehaviorTree_ResolveComposite(const FString& BTPath, const FString& ParentName,
        const TCHAR* NoRootError, const TCHAR* NotFoundLabel, UBehaviorTree*& OutBTAsset, TSharedPtr<FJsonObject>& OutError)
    {
        OutBTAsset = mcp_common::LoadAssetOrSoftPath<UBehaviorTree>(BTPath);
        if (!OutBTAsset)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("BehaviorTree asset not found: %s"), *BTPath));
            return nullptr;
        }
        if (!OutBTAsset->RootNode)
        {
            OutError = mcp_common::MakeErrorReply(NoRootError);
            return nullptr;
        }

        UBTCompositeNode* Composite = nullptr;
        if (ParentName.IsEmpty()
            || ParentName.Equals(TEXT("Root"), ESearchCase::IgnoreCase)
            || ParentName.Equals(TEXT("RootNode"), ESearchCase::IgnoreCase))
        {
            Composite = OutBTAsset->RootNode;
        }
        else
        {
            Composite = FindCompositeByName(OutBTAsset->RootNode, ParentName);
        }
        if (!Composite)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("%s '%s' not found in the tree (use \"Root\" or a composite child_name)"), NotFoundLabel, *ParentName));
        }
        return Composite;
    }
}

//----------------------------------------------------------------------//
// Helpers
//----------------------------------------------------------------------//

// Editor-world actor lookup (object name or label) and the AI components that drive it.

AActor* FindActorByName(const FString& NameOrLabel)
{
    // Editor world only, and an empty name answers nothing (no actor is visited).
    if (NameOrLabel.IsEmpty())
    {
        return nullptr;
    }
    return mcp_common::FindActorByName(mcp_common::GetEditorWorld(), NameOrLabel);
}

UBehaviorTreeComponent* ResolveBehaviorTreeComponent(AActor* Subject)
{
    if (!Subject)
    {
        return nullptr;
    }

    // Resolve the AIController driving this actor (pawn) or the actor itself if it is a controller.
    if (AAIController* AICon = GameplayActorLookup::FindAIController(Subject))
    {
        return Cast<UBehaviorTreeComponent>(AICon->GetBrainComponent());
    }

    // Fall back to a component directly on the actor.
    return Subject->FindComponentByClass<UBehaviorTreeComponent>();
}

UBlackboardComponent* GetBlackboardComponent(AActor* Subject)
{
    if (!Subject)
    {
        return nullptr;
    }

    if (AAIController* AICon = GameplayActorLookup::FindAIController(Subject))
    {
        if (UBlackboardComponent* BB = AICon->GetBlackboardComponent())
        {
            return BB;
        }
    }

    return Subject->FindComponentByClass<UBlackboardComponent>();
}

// The same lookups for a request, each null with the error reply in OutError when a step fails: the running
// blackboard of the actor ActorName; the behavior tree component of the actor the required actor_name names; and
// that actor's running blackboard with the type of the key the required key_name names.

UBlackboardComponent* FindLiveBlackboard(const FString& ActorName, TSharedPtr<FJsonObject>& OutError)
{
    AActor* Actor = FindActorByName(ActorName);
    if (!Actor)
    {
        OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        return nullptr;
    }
    UBlackboardComponent* BB = GetBlackboardComponent(Actor);
    if (!BB)
    {
        OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("No running BlackboardComponent found for actor '%s' (requires an AI-possessed pawn with an active blackboard)."), *ActorName));
    }
    return BB;
}

UBehaviorTreeComponent* ReadTreeComponent(const TSharedPtr<FJsonObject>& Params, FString& OutActorName,
    TSharedPtr<FJsonObject>& OutError)
{
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), OutActorName) || OutActorName.IsEmpty())
    {
        OutError = mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
        return nullptr;
    }
    AActor* Actor = FindActorByName(OutActorName);
    if (!Actor)
    {
        OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *OutActorName));
        return nullptr;
    }
    UBehaviorTreeComponent* BTC = ResolveBehaviorTreeComponent(Actor);
    if (!BTC)
    {
        OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("No UBehaviorTreeComponent on actor '%s'"), *OutActorName));
    }
    return BTC;
}

UBlackboardComponent* ReadBlackboardKey(const TSharedPtr<FJsonObject>& Params, FString& OutActorName,
    FString& OutKeyName, UClass*& OutKeyType, TSharedPtr<FJsonObject>& OutError)
{
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), OutActorName) || OutActorName.IsEmpty())
    {
        OutError = mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
        return nullptr;
    }
    OutError = mcp_common::ReadRequiredString(Params, TEXT("key_name"), OutKeyName);
    if (OutError.IsValid())
    {
        return nullptr;
    }
    UBlackboardComponent* BB = FindLiveBlackboard(OutActorName, OutError);
    if (!BB)
    {
        return nullptr;
    }
    const FBlackboard::FKey KeyID = BB->GetKeyID(FName(*OutKeyName));
    if (KeyID == FBlackboard::InvalidKey)
    {
        OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Blackboard key '%s' not found on actor '%s'"), *OutKeyName, *OutActorName));
        return nullptr;
    }
    OutKeyType = BB->GetKeyType(KeyID);
    return BB;
}

//----------------------------------------------------------------------//
// Asset-based reads
//----------------------------------------------------------------------//

// MCP:get_behavior_tree_info - Get information about a behavior tree's structure and properties
//   @param behavior_tree_path - Path or name of the behavior tree to query
//   @returns { behavior_tree_path: string, name: string, blackboard_asset: string|null, blackboard_keys: [{ name: string, type: string, instance_synced: bool }], root_node: object|null }

TSharedPtr<FJsonObject> HandleGetBehaviorTreeInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
    }

    UBehaviorTree* BTAsset = mcp_common::LoadAssetOrSoftPath<UBehaviorTree>(BTPath);
    if (!BTAsset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BehaviorTree asset not found: %s. Use a path like '/Game/AI/BT_MyTree.BT_MyTree'"), *BTPath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("behavior_tree_path"), BTPath);
    Result->SetStringField(TEXT("name"), BTAsset->GetName());

    // Blackboard asset + keys
    UBlackboardData* BBData = BTAsset->GetBlackboardAsset();
    if (BBData)
    {
        Result->SetStringField(TEXT("blackboard_asset"), BBData->GetPathName());
        Result->SetArrayField(TEXT("blackboard_keys"), BehaviorTree_KeysToJson(BBData));
    }
    else
    {
        Result->SetField(TEXT("blackboard_asset"), MakeShared<FJsonValueNull>());
        Result->SetArrayField(TEXT("blackboard_keys"), TArray<TSharedPtr<FJsonValue>>());
    }

    // Node graph (rooted at RootNode)
    if (BTAsset->RootNode)
    {
        Result->SetObjectField(TEXT("root_node"), SerializeBTNode(BTAsset->RootNode, 0));
    }
    else
    {
        Result->SetField(TEXT("root_node"), MakeShared<FJsonValueNull>());
    }

    return mcp_common::MakeOkReply(Result);
}

//----------------------------------------------------------------------//
// Runtime: run / stop
//----------------------------------------------------------------------//

// MCP:run_behavior_tree - Start execution of a behavior tree on an actor
//   @param actor_name - Name of the actor to run the behavior tree on
//   @param behavior_tree_path - Path or name of the behavior tree asset to execute
//   @returns { actor_name: string, behavior_tree_path: string, started: bool, ai_controller: string }

TSharedPtr<FJsonObject> HandleRunBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
    }

    FString BTPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("behavior_tree_path"), BTPath)) { return Missing; }

    AActor* Actor = FindActorByName(ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    UBehaviorTree* BTAsset = mcp_common::LoadAssetOrSoftPath<UBehaviorTree>(BTPath);
    if (!BTAsset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BehaviorTree asset not found: %s"), *BTPath));
    }

    // Resolve an AIController: the actor itself, or the controller possessing the pawn.
    AAIController* AICon = GameplayActorLookup::FindAIController(Actor);
    if (!AICon)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No AAIController found for actor '%s'. Behavior trees require an AI-possessed pawn (not available headless without PIE/possession)."), *ActorName));
    }

    const bool bStarted = AICon->RunBehaviorTree(BTAsset);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetStringField(TEXT("behavior_tree_path"), BTPath);
    Result->SetBoolField(TEXT("started"), bStarted);
    Result->SetStringField(TEXT("ai_controller"), AICon->GetName());

    if (!bStarted)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RunBehaviorTree failed for actor '%s' (controller could not start the tree)."), *ActorName));
    }

    return mcp_common::MakeOkReply(Result);
}

// MCP:stop_behavior_tree - Stop execution of a running behavior tree on an actor
//   @param actor_name - Name of the actor running the behavior tree
//   @returns { actor_name: string, was_running: bool, stopped: bool }

TSharedPtr<FJsonObject> HandleStopBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
    }

    AActor* Actor = FindActorByName(ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    UBehaviorTreeComponent* BTComp = ResolveBehaviorTreeComponent(Actor);
    if (!BTComp)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No running BehaviorTreeComponent found for actor '%s' (no AIController/brain component)."), *ActorName));
    }

    const bool bWasRunning = BTComp->IsRunning();
    BTComp->StopTree(EBTStopMode::Safe);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetBoolField(TEXT("was_running"), bWasRunning);
    Result->SetBoolField(TEXT("stopped"), true);

    return mcp_common::MakeOkReply(Result);
}

//----------------------------------------------------------------------//
// Runtime blackboard
//----------------------------------------------------------------------//

// MCP:get_blackboard_component - Get the blackboard component from an actor
//   @param actor_name - Name of the actor to query for blackboard component
//   @returns { actor_name: string, component_name: string, has_valid_asset: bool, num_keys: number, blackboard_asset: string?, keys: [{ name: string, type: string, instance_synced: bool }]? }

TSharedPtr<FJsonObject> HandleGetBlackboardComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
    }

    TSharedPtr<FJsonObject> LookupError;
    UBlackboardComponent* BB = FindLiveBlackboard(ActorName, LookupError);
    if (!BB)
    {
        return LookupError;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetStringField(TEXT("component_name"), BB->GetName());
    Result->SetBoolField(TEXT("has_valid_asset"), BB->HasValidAsset());
    Result->SetNumberField(TEXT("num_keys"), BB->GetNumKeys());

    if (UBlackboardData* BBData = BB->GetBlackboardAsset())
    {
        Result->SetStringField(TEXT("blackboard_asset"), BBData->GetPathName());
        Result->SetArrayField(TEXT("keys"), BehaviorTree_KeysToJson(BBData));
    }

    return mcp_common::MakeOkReply(Result);
}

// MCP:set_blackboard_value - Set a value in an actor's blackboard
//   @param actor_name - Name of the actor whose blackboard to modify
//   @param key_name - Name of the blackboard key to set
//   @param value - Value to set in the blackboard (as string, converted by key type)
//   @returns { actor_name: string, key_name: string, key_type: string, value: string, set: bool }

TSharedPtr<FJsonObject> HandleSetBlackboardValue(const TSharedPtr<FJsonObject>& Params)
{
    FString Target;
    FString KeyText;
    UClass* KeyTypeClass = nullptr;
    TSharedPtr<FJsonObject> LookupError;
    UBlackboardComponent* BB = ReadBlackboardKey(Params, Target, KeyText, KeyTypeClass, LookupError);
    if (!BB)
    {
        return LookupError;
    }
    const FName Key(*KeyText);
    if (!KeyTypeClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Blackboard key '%s' has no resolvable type"), *KeyText));
    }

    // Read the requested value as string (covers int/float/bool/name/string via parse).
    FString ValueStr;
    Params->TryGetStringField(TEXT("value"), ValueStr);

    bool bApplied = true;
    if (KeyTypeClass == UBlackboardKeyType_Int::StaticClass())
    {
        BB->SetValueAsInt(Key, FCString::Atoi(*ValueStr));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Float::StaticClass())
    {
        BB->SetValueAsFloat(Key, FCString::Atof(*ValueStr));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Bool::StaticClass())
    {
        const bool bVal = ValueStr.ToBool() || ValueStr == TEXT("1");
        BB->SetValueAsBool(Key, bVal);
    }
    else if (KeyTypeClass == UBlackboardKeyType_String::StaticClass())
    {
        BB->SetValueAsString(Key, ValueStr);
    }
    else if (KeyTypeClass == UBlackboardKeyType_Name::StaticClass())
    {
        BB->SetValueAsName(Key, FName(*ValueStr));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Enum::StaticClass())
    {
        BB->SetValueAsEnum(Key, (uint8)FCString::Atoi(*ValueStr));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Vector::StaticClass())
    {
        FVector Vec = FVector::ZeroVector;
        Vec.InitFromString(ValueStr);
        BB->SetValueAsVector(Key, Vec);
    }
    else if (KeyTypeClass == UBlackboardKeyType_Rotator::StaticClass())
    {
        FRotator Rot = FRotator::ZeroRotator;
        Rot.InitFromString(ValueStr);
        BB->SetValueAsRotator(Key, Rot);
    }
    else if (KeyTypeClass == UBlackboardKeyType_Object::StaticClass())
    {
        UObject* Obj = mcp_common::ResolveObjectByPath(ValueStr);
        BB->SetValueAsObject(Key, Obj);
    }
    else if (KeyTypeClass == UBlackboardKeyType_Class::StaticClass())
    {
        UClass* Cls = ValueStr.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *ValueStr);
        BB->SetValueAsClass(Key, Cls);
    }
    else
    {
        bApplied = false;
    }

    if (!bApplied)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unsupported blackboard key type '%s' for key '%s'"), *KeyTypeClass->GetName(), *KeyText));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Target);
    Result->SetStringField(TEXT("key_name"), KeyText);
    Result->SetStringField(TEXT("key_type"), KeyTypeClass->GetName());
    Result->SetStringField(TEXT("value"), ValueStr);
    Result->SetBoolField(TEXT("set"), true);

    return mcp_common::MakeOkReply(Result);
}

// MCP:get_blackboard_value - Retrieve a specific value from an actor's blackboard
//   @param actor_name - Name of the actor whose blackboard to query
//   @param key_name - Name of the blackboard key to retrieve
//   @returns { actor_name: string, key_name: string, key_type: string, value: number|string|bool }

TSharedPtr<FJsonObject> HandleGetBlackboardValue(const TSharedPtr<FJsonObject>& Params)
{
    FString Target;
    FString KeyText;
    UClass* KeyTypeClass = nullptr;
    TSharedPtr<FJsonObject> LookupError;
    UBlackboardComponent* BB = ReadBlackboardKey(Params, Target, KeyText, KeyTypeClass, LookupError);
    if (!BB)
    {
        return LookupError;
    }
    const FName Key(*KeyText);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Target);
    Result->SetStringField(TEXT("key_name"), KeyText);
    Result->SetStringField(TEXT("key_type"), KeyTypeClass ? KeyTypeClass->GetName() : TEXT("None"));

    if (KeyTypeClass == UBlackboardKeyType_Int::StaticClass())
    {
        Result->SetNumberField(TEXT("value"), BB->GetValueAsInt(Key));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Float::StaticClass())
    {
        Result->SetNumberField(TEXT("value"), BB->GetValueAsFloat(Key));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Bool::StaticClass())
    {
        Result->SetBoolField(TEXT("value"), BB->GetValueAsBool(Key));
    }
    else if (KeyTypeClass == UBlackboardKeyType_String::StaticClass())
    {
        Result->SetStringField(TEXT("value"), BB->GetValueAsString(Key));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Name::StaticClass())
    {
        Result->SetStringField(TEXT("value"), BB->GetValueAsName(Key).ToString());
    }
    else if (KeyTypeClass == UBlackboardKeyType_Enum::StaticClass())
    {
        Result->SetNumberField(TEXT("value"), BB->GetValueAsEnum(Key));
    }
    else if (KeyTypeClass == UBlackboardKeyType_Vector::StaticClass())
    {
        Result->SetStringField(TEXT("value"), BB->GetValueAsVector(Key).ToString());
    }
    else if (KeyTypeClass == UBlackboardKeyType_Rotator::StaticClass())
    {
        Result->SetStringField(TEXT("value"), BB->GetValueAsRotator(Key).ToString());
    }
    else if (KeyTypeClass == UBlackboardKeyType_Object::StaticClass())
    {
        UObject* Obj = BB->GetValueAsObject(Key);
        Result->SetStringField(TEXT("value"), Obj ? Obj->GetPathName() : FString());
    }
    else if (KeyTypeClass == UBlackboardKeyType_Class::StaticClass())
    {
        UClass* Cls = BB->GetValueAsClass(Key);
        Result->SetStringField(TEXT("value"), Cls ? Cls->GetPathName() : FString());
    }
    else
    {
        // Unknown / custom type: provide a textual description.
        Result->SetStringField(TEXT("value"), BB->DescribeKeyValue(Key, EBlackboardDescription::OnlyValue));
    }

    return mcp_common::MakeOkReply(Result);
}

// MCP:get_blackboard_keys - List all available keys in an actor's blackboard
//   @param actor_name - Name of the actor whose blackboard to enumerate (optional, use blackboard_path for asset-based read)
//   @param blackboard_path - Path to a UBlackboardData asset (alternative to actor_name)
//   @returns { actor_name: string?, blackboard_asset: string?, keys: [{ name: string, type: string, instance_synced: bool }], key_count: number }

TSharedPtr<FJsonObject> HandleGetBlackboardKeys(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    Params.IsValid() ? Params->TryGetStringField(TEXT("actor_name"), ActorName) : false;

    // Allow blackboard-data-asset path as an alternative source for keys.
    FString BBPath;
    const bool bHasPath = Params.IsValid() && Params->TryGetStringField(TEXT("blackboard_path"), BBPath) && !BBPath.IsEmpty();

    UBlackboardData* BBData = nullptr;

    if (!ActorName.IsEmpty())
    {
        TSharedPtr<FJsonObject> LookupError;
        UBlackboardComponent* BB = FindLiveBlackboard(ActorName, LookupError);
        if (!BB)
        {
            return LookupError;
        }
        BBData = BB->GetBlackboardAsset();
    }
    else if (bHasPath)
    {
        BBData = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(BBPath);
        if (!BBData)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("BlackboardData asset not found: %s"), *BBPath));
        }
    }
    else
    {
        return mcp_common::MakeErrorReply(TEXT("Provide 'actor_name' (running blackboard) or 'blackboard_path' (asset)"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    if (!ActorName.IsEmpty())
    {
        Result->SetStringField(TEXT("actor_name"), ActorName);
    }

    TArray<TSharedPtr<FJsonValue>> KeysArr;
    if (BBData)
    {
        Result->SetStringField(TEXT("blackboard_asset"), BBData->GetPathName());
        KeysArr = BehaviorTree_KeysToJson(BBData);
    }
    Result->SetArrayField(TEXT("keys"), KeysArr);
    Result->SetNumberField(TEXT("key_count"), KeysArr.Num());

    return mcp_common::MakeOkReply(Result);
}

// MCP:get_blackboard_data - Retrieve all key-value pairs and complete data from an actor's blackboard
//   @param actor_name - Name of the actor whose blackboard to dump (use blackboard_path for asset-based read)
//   @param blackboard_path - Path to a UBlackboardData asset (alternative to actor_name)
//   @returns { actor_name: string?, blackboard_asset: string?, name: string?, parent: string?, keys: [{ name, type, instance_synced }]?, entries: [{ name, type, value }]?, key_count: number }

TSharedPtr<FJsonObject> HandleGetBlackboardData(const TSharedPtr<FJsonObject>& Params)
{
    // Two modes:
    //  (a) blackboard_path -> load a UBlackboardData asset and report its key definitions
    //  (b) actor_name      -> read live values from a running UBlackboardComponent
    FString BBPath;
    const bool bHasPath = Params.IsValid() && Params->TryGetStringField(TEXT("blackboard_path"), BBPath) && !BBPath.IsEmpty();

    if (bHasPath)
    {
        UBlackboardData* BBData = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(BBPath);
        if (!BBData)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("BlackboardData asset not found: %s"), *BBPath));
        }

        return mcp_common::MakeOkReply(BehaviorTree_BlackboardAssetData(BBData, TEXT("blackboard_asset")));
    }

    // Live mode
    FString ActorName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Provide 'blackboard_path' (asset) or 'actor_name' (running blackboard)"));
    }

    TSharedPtr<FJsonObject> LookupError;
    UBlackboardComponent* BB = FindLiveBlackboard(ActorName, LookupError);
    if (!BB)
    {
        return LookupError;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);

    UBlackboardData* BBData = BB->GetBlackboardAsset();
    if (BBData)
    {
        Result->SetStringField(TEXT("blackboard_asset"), BBData->GetPathName());
    }

    TArray<TSharedPtr<FJsonValue>> EntriesArr;
    if (BBData)
    {
        for (const FBlackboardEntry& Entry : BBData->GetKeys())
        {
            TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
            EntryObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
            EntryObj->SetStringField(TEXT("type"), DescribeKeyTypeClass(Entry.KeyType));
            EntryObj->SetStringField(TEXT("value"), BB->DescribeKeyValue(Entry.EntryName, EBlackboardDescription::OnlyValue));
            EntriesArr.Add(MakeShared<FJsonValueObject>(EntryObj));
        }
    }
    Result->SetArrayField(TEXT("entries"), EntriesArr);
    Result->SetNumberField(TEXT("key_count"), EntriesArr.Num());

    return mcp_common::MakeOkReply(Result);
}

//----------------------------------------------------------------------//
// Asset authoring (Blackboard + BehaviorTree)
//----------------------------------------------------------------------//

// MCP:create_blackboard_data - Create and save a new UBlackboardData asset.
//   @param name - Asset name (e.g. "BB_Enemy").
//   @param package_path - Target /Game package path (optional, default "/Game/AI")
//   @returns { name: string, package_path: string, blackboard_path: string, asset_path: string, key_count: number, already_exists: bool, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleCreateBlackboardData(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'name'"));
    }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        PackagePath = TEXT("/Game/AI");
    }
    PackagePath.RemoveFromEnd(TEXT("/"));

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if an asset already exists, load and return it.
    UBlackboardData* Existing = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(FullPath);
    if (Existing)
    {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("name"), Existing->GetName());
        R->SetStringField(TEXT("package_path"), PackagePath);
        R->SetStringField(TEXT("blackboard_path"), Existing->GetPathName());
        R->SetStringField(TEXT("asset_path"), Existing->GetPathName());
        R->SetNumberField(TEXT("key_count"), Existing->GetKeys().Num());
        R->SetBoolField(TEXT("already_exists"), true);
        R->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(R);
    }

    FString CreateError;
    UBlackboardData* NewBB = mcp_common::NewAssetInPackage<UBlackboardData>(
        FullPath, Name, TEXT("Failed to create UBlackboardData"), CreateError);
    if (!NewBB)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(NewBB);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewBB->GetOutermost(), NewBB, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewBB->GetName());
    Result->SetStringField(TEXT("package_path"), PackagePath);
    Result->SetStringField(TEXT("blackboard_path"), NewBB->GetPathName());
    Result->SetStringField(TEXT("asset_path"), NewBB->GetPathName());
    Result->SetNumberField(TEXT("key_count"), NewBB->GetKeys().Num());
    Result->SetBoolField(TEXT("already_exists"), false);

    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_blackboard_data"), SaveError);
}

// MCP:add_blackboard_key - Append a key to a UBlackboardData asset and save it.
//   @param blackboard_path - Path to the UBlackboardData asset.
//   @param key_name - New key name (must be unique on the asset).
//   @param key_type - One of Bool|Int|Float|Vector|Rotator|String|Name|Object|Class|Enum.
//   @param instance_synced - Whether the key is instance-synced (optional, default false)
//   @returns { blackboard_path: string, key_name: string, key_type: string, key_count: number, verified: bool, saved: bool, already_exists: bool?, save_error: string?, keys: [{ name, type, instance_synced }] }

TSharedPtr<FJsonObject> HandleAddBlackboardKey(const TSharedPtr<FJsonObject>& Params)
{
    FString BBPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("blackboard_path"), BBPath) || BBPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'blackboard_path'"));
    }

    FString KeyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key_name"), KeyName)) { return Missing; }

    FString KeyTypeToken;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key_type"), KeyTypeToken)) { return Missing; }

    UClass* KeyTypeClass = ResolveBlackboardKeyTypeClass(KeyTypeToken);
    if (!KeyTypeClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported key_type '%s'. Use Bool|Int|Float|Vector|Rotator|String|Name|Object|Class|Enum"), *KeyTypeToken));
    }

    UBlackboardData* BBData = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(BBPath);
    if (!BBData)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BlackboardData asset not found: %s"), *BBPath));
    }

    const FName KeyFName(*KeyName);

    // Idempotent: if key already exists with the same type, return success.
    for (const FBlackboardEntry& Existing : BBData->Keys)
    {
        if (Existing.EntryName == KeyFName)
        {
            // Check if type matches
            FString ExistingType = DescribeKeyTypeClass(Existing.KeyType);
            if (ExistingType == KeyTypeToken)
            {
                // Key already exists with same type — return success (idempotent)
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetStringField(TEXT("blackboard_path"), BBData->GetPathName());
                Result->SetStringField(TEXT("key_name"), KeyName);
                Result->SetStringField(TEXT("key_type"), ExistingType);
                Result->SetNumberField(TEXT("key_count"), BBData->Keys.Num());
                Result->SetBoolField(TEXT("verified"), true);
                Result->SetBoolField(TEXT("saved"), true);
                Result->SetBoolField(TEXT("already_exists"), true);
                // Add 'keys' array to match the expected response format
                TArray<TSharedPtr<FJsonValue>> KeysArray;
                for (const FBlackboardEntry& Entry : BBData->Keys)
                {
                    TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                    KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
                    KeyObj->SetStringField(TEXT("type"), DescribeKeyTypeClass(Entry.KeyType));
                    KeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
                }
                Result->SetArrayField(TEXT("keys"), KeysArray);
                return mcp_common::MakeOkReply(Result);
            }
            else
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Blackboard key '%s' already exists on %s with different type (%s vs %s)"),
                        *KeyName, *BBData->GetName(), *ExistingType, *KeyTypeToken));
            }
        }
    }

    BBData->Modify();

    // Mirror UBlackboardData::UpdatePersistentKey: KeyType is an Instanced UObject
    // owned by the BlackboardData asset.
    FBlackboardEntry NewEntry;
    NewEntry.EntryName = KeyFName;
    NewEntry.KeyType = NewObject<UBlackboardKeyType>(BBData, KeyTypeClass);
    NewEntry.bInstanceSynced = 0;

    bool bInstanceSynced = false;
    if (Params->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced))
    {
        NewEntry.bInstanceSynced = bInstanceSynced ? 1 : 0;
    }

    BBData->Keys.Add(NewEntry);
    BBData->UpdateKeyIDs();

    // Persist back to disk.
    UPackage* Package = BBData->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BBData, SaveError);

    // Read-back verification: confirm the key now resolves by name.
    const FBlackboard::FKey KeyID = BBData->GetKeyID(KeyFName);
    const bool bVerified = (KeyID != FBlackboard::InvalidKey);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blackboard_path"), BBData->GetPathName());
    Result->SetStringField(TEXT("key_name"), KeyName);
    Result->SetStringField(TEXT("key_type"), DescribeKeyTypeClass(NewEntry.KeyType));
    Result->SetNumberField(TEXT("key_count"), BBData->Keys.Num());
    Result->SetBoolField(TEXT("verified"), bVerified);

    // Emit the full key list so callers can confirm names/types after the add.
    Result->SetArrayField(TEXT("keys"), BehaviorTree_KeysToJson(BBData));

    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("add_blackboard_key"), SaveError);
}

// MCP:remove_blackboard_key - Remove a key from a UBlackboardData asset and save it.  Inverse of add_blackboard_key: drops the matching FBlackboardEntry from UBlackboardData::Keys (then UpdateKeyIDs to re-index). Addresses the key by key_name (as add_blackboard_key created it) or by index into the asset's own Keys array (parent-chain keys are not touched). Idempotent: a missing key returns removed:false / already_absent:true.
//   @param blackboard_path - Path to the UBlackboardData asset.
//   @param key_name - Name of the key to remove (as passed to add_blackboard_key); provide this or 'index'.
//   @param index [number] - Alternative to key_name: 0-based position in this asset's own Keys array (excludes inherited parent keys).
//   @returns { blackboard_path: string, key_name: string, key_type: string?, removed: bool, already_absent: bool, key_count: number, saved: bool, save_error?: string, keys: [{ name, type, instance_synced }] }

TSharedPtr<FJsonObject> HandleRemoveBlackboardKey(const TSharedPtr<FJsonObject>& Params)
{
    FString BBPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("blackboard_path"), BBPath) || BBPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'blackboard_path'"));
    }

    FString KeyName;
    Params->TryGetStringField(TEXT("key_name"), KeyName);
    const int32 KeyIndex = BehaviorTree_ReadIndexParam(Params);
    if (KeyName.IsEmpty() && KeyIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Provide 'key_name' (as passed to add_blackboard_key) or 'index'"));
    }

    UBlackboardData* BBData = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(BBPath);
    if (!BBData)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BlackboardData asset not found: %s"), *BBPath));
    }

    // Locate the entry in this asset's own Keys (RemoveAt on Keys; parent keys are a
    // separate transient cache we must not disturb — mirror of add's Keys.Add path).
    int32 FoundIndex = INDEX_NONE;
    FString FoundName;
    FString FoundType;
    if (KeyIndex >= 0)
    {
        if (!BBData->Keys.IsValidIndex(KeyIndex))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("index %d out of range (%s has %d own keys)"), KeyIndex, *BBData->GetName(), BBData->Keys.Num()));
        }
        FoundIndex = KeyIndex;
        FoundName = BBData->Keys[KeyIndex].EntryName.ToString();
        FoundType = DescribeKeyTypeClass(BBData->Keys[KeyIndex].KeyType);
    }
    else
    {
        const FName KeyFName(*KeyName);
        for (int32 i = 0; i < BBData->Keys.Num(); ++i)
        {
            if (BBData->Keys[i].EntryName == KeyFName)
            {
                FoundIndex = i;
                FoundName = KeyName;
                FoundType = DescribeKeyTypeClass(BBData->Keys[i].KeyType);
                break;
            }
        }
    }

    // Idempotent: nothing matched -> already_absent, asset untouched.
    if (FoundIndex == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("blackboard_path"), BBData->GetPathName());
        R->SetStringField(TEXT("key_name"), KeyName);
        R->SetBoolField(TEXT("removed"), false);
        R->SetBoolField(TEXT("already_absent"), true);
        R->SetNumberField(TEXT("key_count"), BBData->Keys.Num());
        R->SetBoolField(TEXT("saved"), false);
        R->SetArrayField(TEXT("keys"), BehaviorTree_KeysToJson(BBData));
        return mcp_common::MakeOkReply(R);
    }

    BBData->Modify();
    BBData->Keys.RemoveAt(FoundIndex, 1, EAllowShrinking::No);
    // Re-index after mutating Keys, mirroring add_blackboard_key's UpdateKeyIDs().
    BBData->UpdateKeyIDs();

    UPackage* Package = BBData->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BBData, SaveError);

    // Read-back verification: the removed key must not resolve by name.
    const bool bStillPresent = (BBData->GetKeyID(FName(*FoundName)) != FBlackboard::InvalidKey);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blackboard_path"), BBData->GetPathName());
    Result->SetStringField(TEXT("key_name"), FoundName);
    Result->SetStringField(TEXT("key_type"), FoundType);
    Result->SetBoolField(TEXT("removed"), !bStillPresent);
    Result->SetBoolField(TEXT("already_absent"), false);
    Result->SetNumberField(TEXT("key_count"), BBData->Keys.Num());
    Result->SetArrayField(TEXT("keys"), BehaviorTree_KeysToJson(BBData));

    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("remove_blackboard_key"), SaveError);
}

// MCP:get_blackboard_keys_asset - Read key definitions from a UBlackboardData asset (no live component needed).
//   @param blackboard_path - Path to the UBlackboardData asset.
//   @returns { blackboard_path: string, name: string, parent: string?, keys: [{ name: string, type: string, instance_synced: bool }], key_count: number }

TSharedPtr<FJsonObject> HandleGetBlackboardKeysAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString BBPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("blackboard_path"), BBPath) || BBPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'blackboard_path'"));
    }

    UBlackboardData* BBData = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(BBPath);
    if (!BBData)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BlackboardData asset not found: %s"), *BBPath));
    }

    return mcp_common::MakeOkReply(BehaviorTree_BlackboardAssetData(BBData, TEXT("blackboard_path")));
}

// MCP:set_blackboard_key_subsettings - Constrain an existing blackboard key's subsettings on a UBlackboardData asset and save. This is the SECOND HALF of add_blackboard_key, which only sets EntryName/KeyType/bInstanceSynced: it fills the per-type filter the editor would otherwise make you pick by hand -- BaseClass for an Object/Class key, EnumType for an Enum key. At least one of base_class / enum_type is required, and the one you supply must match the key's type.
//   @param blackboard_path - Path to the UBlackboardData asset.
//   @param key_name - Name of the existing key to constrain.
//   @param base_class - Class path/name (e.g. "/Script/Engine.Actor" or "Actor") for an Object/Class key (optional).
//   @param enum_type - Enum path/name (e.g. "/Script/Engine.ECollisionChannel") for an Enum key (optional).
//   @returns { blackboard_path: string, key_name: string, key_type: string, base_class: string?, enum_type: string?, enum_name: string?, already_set: bool, saved: bool, save_error: string?, keys: [...] }

TSharedPtr<FJsonObject> HandleSetBlackboardKeySubsettings(const TSharedPtr<FJsonObject>& Params)
{
    FString BBPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("blackboard_path"), BBPath) || BBPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'blackboard_path'"));
    }

    FString KeyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("key_name"), KeyName)) { return Missing; }

    FString BaseClassStr;
    Params->TryGetStringField(TEXT("base_class"), BaseClassStr);
    BaseClassStr.TrimStartAndEndInline();
    FString EnumTypeStr;
    Params->TryGetStringField(TEXT("enum_type"), EnumTypeStr);
    EnumTypeStr.TrimStartAndEndInline();

    const bool bWantBaseClass = !BaseClassStr.IsEmpty();
    const bool bWantEnumType = !EnumTypeStr.IsEmpty();
    if (!bWantBaseClass && !bWantEnumType)
    {
        return mcp_common::MakeErrorReply(
            TEXT("At least one of 'base_class' (Object/Class key) or 'enum_type' (Enum key) is required"));
    }

    UBlackboardData* BBData = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(BBPath);
    if (!BBData)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BlackboardData asset not found: %s"), *BBPath));
    }

    // Find the existing entry by name (mutable: we mutate KeyType in place — replacing
    // KeyType would orphan the Instanced subobject the editor relies on).
    const FName KeyFName(*KeyName);
    FBlackboardEntry* Entry = nullptr;
    for (FBlackboardEntry& Candidate : BBData->Keys)
    {
        if (Candidate.EntryName == KeyFName)
        {
            Entry = &Candidate;
            break;
        }
    }
    if (!Entry)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Blackboard key '%s' not found on %s"), *KeyName, *BBData->GetName()));
    }

    const FString KeyTypeName = DescribeKeyTypeClass(Entry->KeyType);
    const UBlackboardKeyType* KeyType = Entry->KeyType;

    // Type-check up front (honest error): each subsetting only applies to its key type.
    UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(Entry->KeyType);
    UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(Entry->KeyType);
    UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry->KeyType);

    if (bWantBaseClass && !ObjKey && !ClassKey)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("'base_class' only applies to an Object or Class key; '%s' is of type %s"),
                *KeyName, *KeyTypeName));
    }
    if (bWantEnumType && !EnumKey)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("'enum_type' only applies to an Enum key; '%s' is of type %s"),
                *KeyName, *KeyTypeName));
    }

    // Resolve the requested types BEFORE mutating, so an unresolved name is a clean
    // no-op error rather than a half-applied write.
    UClass* ResolvedBaseClass = nullptr;
    if (bWantBaseClass)
    {
        // Full object path first (e.g. "/Script/Engine.Actor"); fall back to a short
        // type lookup (e.g. "Actor"), mirroring the engine's name resolution.
        ResolvedBaseClass = LoadObject<UClass>(nullptr, *BaseClassStr);
        if (!ResolvedBaseClass)
        {
            ResolvedBaseClass = UClass::TryFindTypeSlow<UClass>(BaseClassStr, EFindFirstObjectOptions::ExactClass);
        }
        if (!ResolvedBaseClass)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not resolve base_class '%s' to a UClass"), *BaseClassStr));
        }
    }

    UEnum* ResolvedEnum = nullptr;
    if (bWantEnumType)
    {
        // The editor's PostEditChangeProperty path resolves enums via this exact call;
        // every headless reader uses EnumType directly, so we must set it ourselves.
        ResolvedEnum = UClass::TryFindTypeSlow<UEnum>(EnumTypeStr, EFindFirstObjectOptions::ExactClass);
        if (!ResolvedEnum)
        {
            // /Game user-defined enums live in a package; load by path.
            ResolvedEnum = LoadObject<UEnum>(nullptr, *EnumTypeStr);
        }
        if (!ResolvedEnum)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not resolve enum_type '%s' to a UEnum"), *EnumTypeStr));
        }
    }

    // Detect a true no-op so re-running reports already_set without a redundant save.
    bool bChanged = false;
    if (ObjKey)
    {
        bChanged = (ObjKey->BaseClass != ResolvedBaseClass);
    }
    else if (ClassKey)
    {
        bChanged = (ClassKey->BaseClass != ResolvedBaseClass);
    }
    else if (EnumKey)
    {
        const FString EnumName = ResolvedEnum->GetName();
        bChanged = (EnumKey->EnumType != ResolvedEnum)
            || (EnumKey->EnumName != EnumName)
            || (EnumKey->bIsEnumNameValid == 0);
    }

    bool bSaved = true;
    if (bChanged)
    {
        BBData->Modify();

        if (ObjKey)
        {
            ObjKey->BaseClass = ResolvedBaseClass;
        }
        else if (ClassKey)
        {
            ClassKey->BaseClass = ResolvedBaseClass;
        }
        else if (EnumKey)
        {
            // ENUM GOTCHA: EnumName->EnumType resolution is editor-only; set all three
            // fields the way PostEditChangeProperty would so headless readers see it.
            EnumKey->EnumType = ResolvedEnum;
            EnumKey->EnumName = ResolvedEnum->GetName();
            EnumKey->bIsEnumNameValid = (ResolvedEnum != nullptr && !EnumKey->EnumName.IsEmpty()) ? 1 : 0;
        }

        BBData->UpdateKeyIDs();

        UPackage* Package = BBData->GetOutermost();
        FString SaveError;
        bSaved = mcp_common::SaveAssetPackage(Package, BBData, SaveError);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("blackboard_path"), BBData->GetPathName());
        Result->SetStringField(TEXT("key_name"), KeyName);
        Result->SetStringField(TEXT("key_type"), KeyTypeName);
        BehaviorTree_AddKeySubsettings(Result, KeyType);
        Result->SetBoolField(TEXT("already_set"), false);
        Result->SetArrayField(TEXT("keys"), BehaviorTree_KeysToJson(BBData));

        return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_blackboard_key_subsettings"), SaveError);
    }

    // Idempotent no-op: value already matches; report already_set without re-saving.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blackboard_path"), BBData->GetPathName());
    Result->SetStringField(TEXT("key_name"), KeyName);
    Result->SetStringField(TEXT("key_type"), KeyTypeName);
    BehaviorTree_AddKeySubsettings(Result, KeyType);
    Result->SetBoolField(TEXT("already_set"), true);
    Result->SetBoolField(TEXT("saved"), true);
    Result->SetArrayField(TEXT("keys"), BehaviorTree_KeysToJson(BBData));

    return mcp_common::MakeOkReply(Result);
}

// MCP:create_behavior_tree - Create and save a new UBehaviorTree asset, optionally binding a blackboard.
//   @param name - Asset name (e.g. "BT_Enemy").
//   @param package_path - Target /Game package path (optional, default "/Game/AI")
//   @param blackboard_path - Path to a UBlackboardData asset to bind to the new BT (optional)
//   @returns { name: string, package_path: string, behavior_tree_path: string, asset_path: string, blackboard_asset: string?, already_exists: bool, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleCreateBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'name'"));
    }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        PackagePath = TEXT("/Game/AI");
    }
    PackagePath.RemoveFromEnd(TEXT("/"));

    // Optional blackboard asset to bind.
    FString BBPath;
    Params->TryGetStringField(TEXT("blackboard_path"), BBPath);
    UBlackboardData* BBData = nullptr;
    if (!BBPath.IsEmpty())
    {
        BBData = mcp_common::LoadAssetOrSoftPath<UBlackboardData>(BBPath);
        if (!BBData)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("blackboard_path provided but BlackboardData asset not found: %s"), *BBPath));
        }
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);

    // Idempotent: load + (optionally) bind blackboard + return.
    UBehaviorTree* Existing = mcp_common::LoadAssetOrSoftPath<UBehaviorTree>(FullPath);
    if (Existing)
    {
        bool bUpdated = false;
        if (BBData && Existing->BlackboardAsset != BBData)
        {
            Existing->Modify();
            Existing->BlackboardAsset = BBData;
            bUpdated = true;
        }
        bool bSavedExisting = true;
        if (bUpdated)
        {
            FString SaveErr;
            bSavedExisting = mcp_common::SaveAssetPackage(Existing->GetOutermost(), Existing, SaveErr);
        }

        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("name"), Existing->GetName());
        R->SetStringField(TEXT("package_path"), PackagePath);
        R->SetStringField(TEXT("behavior_tree_path"), Existing->GetPathName());
        R->SetStringField(TEXT("asset_path"), Existing->GetPathName());
        if (UBlackboardData* Bound = Existing->GetBlackboardAsset())
        {
            R->SetStringField(TEXT("blackboard_asset"), Bound->GetPathName());
        }
        R->SetBoolField(TEXT("already_exists"), true);
        R->SetBoolField(TEXT("saved"), bSavedExisting);
        return mcp_common::MakeOkReply(R);
    }

    FString CreateError;
    UBehaviorTree* NewBT = mcp_common::NewAssetInPackage<UBehaviorTree>(
        FullPath, Name, TEXT("Failed to create UBehaviorTree"), CreateError);
    if (!NewBT)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    if (BBData)
    {
        NewBT->BlackboardAsset = BBData;
    }

    FAssetRegistryModule::AssetCreated(NewBT);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewBT->GetOutermost(), NewBT, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), NewBT->GetName());
    Result->SetStringField(TEXT("package_path"), PackagePath);
    Result->SetStringField(TEXT("behavior_tree_path"), NewBT->GetPathName());
    Result->SetStringField(TEXT("asset_path"), NewBT->GetPathName());
    if (UBlackboardData* Bound = NewBT->GetBlackboardAsset())
    {
        Result->SetStringField(TEXT("blackboard_asset"), Bound->GetPathName());
    }
    Result->SetBoolField(TEXT("already_exists"), false);
    mcp_common::AddSaveOutcome(Result, bSaved, SaveError);

    return mcp_common::MakeOkReply(Result);
}

// MCP:set_behavior_tree_root - Set a BehaviorTree's RootNode to a new composite (Selector or Sequence) and save.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param composite - Root composite type: "Selector" or "Sequence" (optional, default "Selector")
//   @returns { behavior_tree_path: string, composite: string, has_root_node: bool, root_node_class: string, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleSetBehaviorTreeRoot(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
    }

    FString Composite;
    if (!Params->TryGetStringField(TEXT("composite"), Composite) || Composite.IsEmpty())
    {
        // Default to a Selector root if not specified.
        Composite = TEXT("Selector");
    }

    UClass* CompositeClass = nullptr;
    if (Composite.Equals(TEXT("Selector"), ESearchCase::IgnoreCase))
    {
        CompositeClass = UBTComposite_Selector::StaticClass();
    }
    else if (Composite.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
    {
        CompositeClass = UBTComposite_Sequence::StaticClass();
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported composite '%s'. Use Selector or Sequence."), *Composite));
    }

    UBehaviorTree* BTAsset = mcp_common::LoadAssetOrSoftPath<UBehaviorTree>(BTPath);
    if (!BTAsset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BehaviorTree asset not found: %s"), *BTPath));
    }

    BTAsset->Modify();

    // UBehaviorTree::RootNode is a UBTCompositeNode*. Create the composite owned by
    // the BT asset so it is saved in the same package.
    UBTCompositeNode* RootComposite = NewObject<UBTCompositeNode>(
        BTAsset, CompositeClass, NAME_None, RF_Transactional);
    if (!RootComposite)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create composite node of class %s"), *CompositeClass->GetName()));
    }

    BTAsset->RootNode = RootComposite;

    UPackage* Package = BTAsset->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BTAsset, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("behavior_tree_path"), BTAsset->GetPathName());
    Result->SetStringField(TEXT("composite"), Composite);
    Result->SetBoolField(TEXT("has_root_node"), BTAsset->RootNode != nullptr);
    // Read-back verification of the root composite class.
    Result->SetStringField(TEXT("root_node_class"),
        BTAsset->RootNode ? BTAsset->RootNode->GetClass()->GetName() : FString(TEXT("None")));

    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_behavior_tree_root"), SaveError);
}

// ===========================================================================
// Composite/task child authoring (root-only shell -> functional tree)
// Populates a composite's Children (a TArray<FBTCompositeChild>) — the one piece
// set_behavior_tree_root does not do. Authoring is DATA-ONLY: ParentNode /
// ExecutionIndex / TreeAsset are assigned by UBehaviorTreeManager::LoadTree ->
// InitializeNodeHelper at load time (which run_behavior_tree triggers), so we only
// create the node owned by the BT package and append a child wrapper. Children are
// addressed/deduplicated by a stable NodeName, making re-adds idempotent.
// ===========================================================================
namespace
{
    // Shared implementation for add_bt_composite_child / add_bt_task_child.
    TSharedPtr<FJsonObject> AddBTChildImpl(const TSharedPtr<FJsonObject>& Params, bool bComposite)
    {
        const TCHAR* ClassParam = bComposite ? TEXT("composite") : TEXT("task_class");
        UClass* Base = bComposite ? UBTCompositeNode::StaticClass() : UBTTaskNode::StaticClass();
        const TCHAR* Prefix = bComposite ? TEXT("BTComposite_") : TEXT("BTTask_");

        FString BTPath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
        }
        FString NodeClassStr;
        if (!Params->TryGetStringField(ClassParam, NodeClassStr) || NodeClassStr.IsEmpty())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Missing required parameter '%s'"), ClassParam));
        }
        FString ParentName;
        Params->TryGetStringField(TEXT("parent_name"), ParentName);
        FString ChildName;
        Params->TryGetStringField(TEXT("child_name"), ChildName);

        UBehaviorTree* BTAsset = nullptr;
        TSharedPtr<FJsonObject> Err;
        UBTCompositeNode* Parent = BehaviorTree_ResolveComposite(BTPath, ParentName,
            TEXT("BehaviorTree has no root composite; call set_behavior_tree_root first"), TEXT("Parent composite"), BTAsset, Err);
        if (!Parent)
        {
            return Err;
        }

        UClass* NodeClass = ResolveBTNodeClass(NodeClassStr, Base, Prefix);
        if (!NodeClass)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not resolve '%s' to a concrete %s subclass"),
                    *NodeClassStr, bComposite ? TEXT("UBTCompositeNode (e.g. Selector/Sequence)") : TEXT("UBTTaskNode (e.g. Wait/MoveTo)")));
        }

        // Default the stable child name to the class type (prefix stripped).
        if (ChildName.IsEmpty())
        {
            ChildName = NodeClass->GetName();
            ChildName.RemoveFromStart(Prefix);
        }

        const FString ResolvedParentName = (Parent == BTAsset->RootNode) ? FString(TEXT("Root")) : Parent->GetNodeName();

        // Idempotent: a child with this name already exists under the parent.
        if (const UBTNode* ExistingChild = FindChildByName(Parent, ChildName))
        {
            TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
            R->SetStringField(TEXT("child_class"), ExistingChild->GetClass()->GetName());
            R->SetStringField(TEXT("child_kind"), ExistingChild->IsA<UBTCompositeNode>() ? TEXT("composite") : TEXT("task"));
            R->SetBoolField(TEXT("already_exists"), true);
            R->SetNumberField(TEXT("child_count"), Parent->Children.Num());
            R->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOkReply(R);
        }

        BTAsset->Modify();

        UBTNode* NewNode = NewObject<UBTNode>(BTAsset, NodeClass, NAME_None, RF_Transactional);
        if (!NewNode)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to create node of class %s"), *NodeClass->GetName()));
        }
        NewNode->NodeName = ChildName;

        FBTCompositeChild NewChild;
        if (bComposite)
        {
            NewChild.ChildComposite = Cast<UBTCompositeNode>(NewNode);
        }
        else
        {
            NewChild.ChildTask = Cast<UBTTaskNode>(NewNode);
        }
        Parent->Children.Add(NewChild);

        UPackage* Package = BTAsset->GetOutermost();
        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Package, BTAsset, SaveError);

        TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
        R->SetStringField(TEXT("child_class"), NodeClass->GetName());
        R->SetStringField(TEXT("child_kind"), bComposite ? TEXT("composite") : TEXT("task"));
        R->SetBoolField(TEXT("already_exists"), false);
        R->SetNumberField(TEXT("child_count"), Parent->Children.Num());
        mcp_common::AddSaveOutcome(R, bSaved, SaveError);
        return mcp_common::MakeOkReply(R);
    }
}

// MCP:add_bt_composite_child - Add a child composite (Selector/Sequence/SimpleParallel) under a BehaviorTree composite.  The child is appended to the parent's Children (array order = execution order). Authoring is data-only: ParentNode/ExecutionIndex are assigned by the BT runtime at load (run_behavior_tree). Idempotent by child_name within the parent. The returned child_name is how you address this composite as a parent for further children.
//   @param behavior_tree_path - Path to the UBehaviorTree asset (its RootNode must already be set via set_behavior_tree_root).
//   @param composite [string!] - Concrete composite class: "Selector" | "Sequence" | "SimpleParallel" (or a BTComposite_* class name).
//   @param parent_name - Optional parent composite name (a child_name from a prior add); "Root" or omitted targets the RootNode.
//   @param child_name - Optional stable name for the new node (its identity for idempotency/addressing); defaults to the composite type.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, child_class: string, child_kind: string, already_exists: bool, child_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddBTCompositeChild(const TSharedPtr<FJsonObject>& Params)
{
    return AddBTChildImpl(Params, /*bComposite=*/true);
}

// MCP:add_bt_task_child - Add a leaf task node (Wait/MoveTo/PlaySound/RunBehavior/...) under a BehaviorTree composite.  The task is appended to the parent's Children (array order = execution order). Authoring is data-only: ParentNode/ExecutionIndex are assigned by the BT runtime at load. Idempotent by child_name within the parent.
//   @param behavior_tree_path - Path to the UBehaviorTree asset (its RootNode must already be set via set_behavior_tree_root).
//   @param task_class [string!] - Concrete task class: "Wait" | "MoveTo" | "PlaySound" | "RunBehavior" | "FinishWithResult" (or a BTTask_* class name).
//   @param parent_name - Optional parent composite name; "Root" or omitted targets the RootNode.
//   @param child_name - Optional stable name for the new node; defaults to the task type.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, child_class: string, child_kind: string, already_exists: bool, child_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddBTTaskChild(const TSharedPtr<FJsonObject>& Params)
{
    return AddBTChildImpl(Params, /*bComposite=*/false);
}

// ===========================================================================
// Inverse CRUD of add_bt_composite_child / add_bt_task_child. Removes one entry
// from the parent composite's Children (a TArray<FBTCompositeChild>) — the exact
// array the add_* twins append to. Addressing mirrors the add path: parent_name
// (empty/"Root" -> RootNode) resolves the composite; the child is then located by
// its stable child_name (a returned identity from a prior add) or by index. Only
// the matching KIND is removed (bComposite -> ChildComposite set; !bComposite ->
// ChildTask set) so remove_bt_task_child never drops a composite edge and vice
// versa. Idempotent: a missing child reports removed:false / already_absent:true.
// The freed node is a plain UObject (not a UEdGraphNode) held only by the dropped
// FBTCompositeChild wrapper, so RemoveAt is sufficient — GC reclaims it, mirroring
// the data-only NewObject+.Add of the add twins (no DestroyNode to mirror).
// ===========================================================================
namespace
{
    // Shared implementation for remove_bt_composite_child / remove_bt_task_child.
    TSharedPtr<FJsonObject> RemoveBTChildImpl(const TSharedPtr<FJsonObject>& Params, bool bComposite)
    {
        FString BTPath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
        }
        FString ChildName;
        Params->TryGetStringField(TEXT("child_name"), ChildName);
        const int32 ChildIndex = BehaviorTree_ReadIndexParam(Params);
        if (ChildName.IsEmpty() && ChildIndex < 0)
        {
            return mcp_common::MakeErrorReply(TEXT("Provide 'child_name' (a child_name from a prior add) or 'index'"));
        }
        FString ParentName;
        Params->TryGetStringField(TEXT("parent_name"), ParentName);

        UBehaviorTree* BTAsset = nullptr;
        TSharedPtr<FJsonObject> Err;
        UBTCompositeNode* Parent = BehaviorTree_ResolveComposite(BTPath, ParentName,
            TEXT("BehaviorTree has no root composite; nothing to remove"), TEXT("Parent composite"), BTAsset, Err);
        if (!Parent)
        {
            return Err;
        }

        const FString ResolvedParentName = (Parent == BTAsset->RootNode) ? FString(TEXT("Root")) : Parent->GetNodeName();
        const TCHAR* KindStr = bComposite ? TEXT("composite") : TEXT("task");

        // Locate the matching child edge. By index the edge kind must match; by name we
        // scan only edges of the requested kind so remove_bt_composite_child and
        // remove_bt_task_child stay non-overlapping.
        int32 FoundIndex = INDEX_NONE;
        FString FoundName;
        if (ChildIndex >= 0)
        {
            if (!Parent->Children.IsValidIndex(ChildIndex))
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("index %d out of range (parent '%s' has %d children)"), ChildIndex, *ResolvedParentName, Parent->Children.Num()));
            }
            const FBTCompositeChild& Edge = Parent->Children[ChildIndex];
            const UBTNode* AtIdx = bComposite
                ? static_cast<const UBTNode*>(Edge.ChildComposite)
                : static_cast<const UBTNode*>(Edge.ChildTask);
            if (!AtIdx)
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Child at index %d under '%s' is not a %s"), ChildIndex, *ResolvedParentName, KindStr));
            }
            FoundIndex = ChildIndex;
            FoundName = AtIdx->GetNodeName();
        }
        else
        {
            for (int32 i = 0; i < Parent->Children.Num(); ++i)
            {
                const FBTCompositeChild& Edge = Parent->Children[i];
                const UBTNode* Node = bComposite
                    ? static_cast<const UBTNode*>(Edge.ChildComposite)
                    : static_cast<const UBTNode*>(Edge.ChildTask);
                if (Node && Node->GetNodeName() == ChildName)
                {
                    FoundIndex = i;
                    FoundName = ChildName;
                    break;
                }
            }
        }

        // Idempotent: nothing matched -> report already_absent without touching the asset.
        if (FoundIndex == INDEX_NONE)
        {
            TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
            R->SetStringField(TEXT("child_kind"), KindStr);
            R->SetBoolField(TEXT("removed"), false);
            R->SetBoolField(TEXT("already_absent"), true);
            R->SetNumberField(TEXT("child_count"), Parent->Children.Num());
            R->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOkReply(R);
        }

        BTAsset->Modify();
        Parent->Children.RemoveAt(FoundIndex, 1, EAllowShrinking::No);

        UPackage* Package = BTAsset->GetOutermost();
        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Package, BTAsset, SaveError);

        TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), FoundName);
        R->SetStringField(TEXT("child_kind"), KindStr);
        R->SetBoolField(TEXT("removed"), true);
        R->SetBoolField(TEXT("already_absent"), false);
        R->SetNumberField(TEXT("child_count"), Parent->Children.Num());
        mcp_common::AddSaveOutcome(R, bSaved, SaveError);
        return mcp_common::MakeOkReply(R);
    }
}

// MCP:remove_bt_composite_child - Remove a child composite (Selector/Sequence/...) from a BehaviorTree composite.  Inverse of add_bt_composite_child: drops the matching entry from the parent's Children array. Only composite child-edges are considered, so a task with the same name is untouched. Idempotent: a missing child returns removed:false / already_absent:true.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param child_name - Name of the composite child to remove (a child_name from a prior add_bt_composite_child); provide this or 'index'.
//   @param index [number] - Alternative to child_name: 0-based position in the parent's Children array (the edge at that index must be a composite).
//   @param parent_name - Optional parent composite name owning the child; "Root" or omitted targets the RootNode.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, child_kind: string, removed: bool, already_absent: bool, child_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveBTCompositeChild(const TSharedPtr<FJsonObject>& Params)
{
    return RemoveBTChildImpl(Params, /*bComposite=*/true);
}

// MCP:remove_bt_task_child - Remove a leaf task node (Wait/MoveTo/...) from a BehaviorTree composite.  Inverse of add_bt_task_child: drops the matching entry from the parent's Children array. Only task child-edges are considered, so a composite with the same name is untouched. Idempotent: a missing child returns removed:false / already_absent:true.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param child_name - Name of the task child to remove (a child_name from a prior add_bt_task_child); provide this or 'index'.
//   @param index [number] - Alternative to child_name: 0-based position in the parent's Children array (the edge at that index must be a task).
//   @param parent_name - Optional parent composite name owning the child; "Root" or omitted targets the RootNode.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, child_kind: string, removed: bool, already_absent: bool, child_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveBTTaskChild(const TSharedPtr<FJsonObject>& Params)
{
    return RemoveBTChildImpl(Params, /*bComposite=*/false);
}

// ===========================================================================
// Decorator/service authoring (the conditions + background ticks that the
// composites/tasks above lack). Both are UBTAuxiliaryNode subclasses authored
// DATA-ONLY (NewObject owned by the BT package + .Add), exactly like composite children:
// ParentNode/ExecutionIndex are assigned by UBehaviorTreeManager::LoadTree at load.
// A decorator gates ONE child-edge (FBTCompositeChild::Decorators); a service ticks
// while ONE composite is active (UBTCompositeNode::Services). Idempotent by NodeName.
// ===========================================================================

// MCP:add_bt_decorator - Attach a decorator (execution condition) to a specific child-edge of a BehaviorTree composite.  The decorator is appended to that child's Decorators array and gates whether the child may run. Authoring is data-only: ParentNode/ExecutionIndex are assigned by the BT runtime at load (run_behavior_tree). Idempotent by decorator_name within the child-edge.
//   @param behavior_tree_path - Path to the UBehaviorTree asset (its RootNode must already be set, with the target child added).
//   @param child_name [string!] - Name of the existing child (a child_name from a prior add_bt_task_child/add_bt_composite_child) whose edge the decorator gates.
//   @param decorator_class [string!] - Concrete decorator class: "Cooldown" | "Loop" | "TimeLimit" | "Blackboard" | ... (or a BTDecorator_* class name).
//   @param parent_name - Optional parent composite name owning the child-edge; "Root" or omitted targets the RootNode.
//   @param decorator_name - Optional stable name for the new decorator (its identity for idempotency); defaults to the decorator type.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, decorator_name: string, decorator_class: string, already_exists: bool, decorator_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddBTDecorator(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
    }
    FString ChildName;
    if (!Params->TryGetStringField(TEXT("child_name"), ChildName) || ChildName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'child_name' (the child-edge the decorator gates)"));
    }
    FString DecoratorClassStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("decorator_class"), DecoratorClassStr)) { return Missing; }
    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName);
    FString DecoratorName;
    Params->TryGetStringField(TEXT("decorator_name"), DecoratorName);

    // Resolve the parent composite owning the child-edge.
    UBehaviorTree* BTAsset = nullptr;
    TSharedPtr<FJsonObject> Err;
    UBTCompositeNode* Parent = BehaviorTree_ResolveComposite(BTPath, ParentName,
        TEXT("BehaviorTree has no root composite; call set_behavior_tree_root first"), TEXT("Parent composite"), BTAsset, Err);
    if (!Parent)
    {
        return Err;
    }

    FBTCompositeChild* Edge = FindChildEdgeByName(Parent, ChildName);
    if (!Edge)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Child '%s' not found under parent composite (add it via add_bt_task_child/add_bt_composite_child first)"), *ChildName));
    }

    UClass* DecoratorClass = ResolveBTNodeClass(DecoratorClassStr, UBTDecorator::StaticClass(), TEXT("BTDecorator_"));
    if (!DecoratorClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not resolve '%s' to a concrete UBTDecorator subclass (e.g. Cooldown/Loop/TimeLimit/Blackboard)"), *DecoratorClassStr));
    }

    // Default the stable decorator name to the class type (prefix stripped).
    if (DecoratorName.IsEmpty())
    {
        DecoratorName = DecoratorClass->GetName();
        DecoratorName.RemoveFromStart(TEXT("BTDecorator_"));
    }

    const FString ResolvedParentName = (Parent == BTAsset->RootNode) ? FString(TEXT("Root")) : Parent->GetNodeName();

    // Idempotent: a decorator with this name already gates this child-edge.
    for (const UBTDecorator* Existing : Edge->Decorators)
    {
        if (Existing && Existing->GetNodeName() == DecoratorName)
        {
            TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
            R->SetStringField(TEXT("decorator_name"), DecoratorName);
            R->SetStringField(TEXT("decorator_class"), Existing->GetClass()->GetName());
            R->SetBoolField(TEXT("already_exists"), true);
            R->SetNumberField(TEXT("decorator_count"), Edge->Decorators.Num());
            R->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOkReply(R);
        }
    }

    BTAsset->Modify();

    UBTDecorator* NewDecorator = NewObject<UBTDecorator>(BTAsset, DecoratorClass, NAME_None, RF_Transactional);
    if (!NewDecorator)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create decorator of class %s"), *DecoratorClass->GetName()));
    }
    NewDecorator->NodeName = DecoratorName;
    Edge->Decorators.Add(NewDecorator);

    UPackage* Package = BTAsset->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BTAsset, SaveError);

    TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
    R->SetStringField(TEXT("decorator_name"), DecoratorName);
    R->SetStringField(TEXT("decorator_class"), DecoratorClass->GetName());
    R->SetBoolField(TEXT("already_exists"), false);
    R->SetNumberField(TEXT("decorator_count"), Edge->Decorators.Num());
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("add_bt_decorator"), SaveError);
}

// MCP:remove_bt_decorator - Remove a decorator (execution condition) from a specific child-edge of a BehaviorTree composite.  Inverse of add_bt_decorator: drops the matching entry from that child's Decorators array (FBTCompositeChild::Decorators). Idempotent: a missing decorator returns removed:false / already_absent:true.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param child_name [string!] - Name of the existing child (a child_name from a prior add) whose edge the decorator gates.
//   @param decorator_name - Name of the decorator to remove (a decorator_name from a prior add_bt_decorator); provide this or 'index'.
//   @param index [number] - Alternative to decorator_name: 0-based position in the child-edge's Decorators array.
//   @param parent_name - Optional parent composite name owning the child-edge; "Root" or omitted targets the RootNode.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, decorator_name: string, removed: bool, already_absent: bool, decorator_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveBTDecorator(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
    }
    FString ChildName;
    if (!Params->TryGetStringField(TEXT("child_name"), ChildName) || ChildName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'child_name' (the child-edge the decorator gates)"));
    }
    FString DecoratorName;
    Params->TryGetStringField(TEXT("decorator_name"), DecoratorName);
    const int32 DecoratorIndex = BehaviorTree_ReadIndexParam(Params);
    if (DecoratorName.IsEmpty() && DecoratorIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Provide 'decorator_name' (a decorator_name from a prior add) or 'index'"));
    }
    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName);

    // Resolve the parent composite owning the child-edge.
    UBehaviorTree* BTAsset = nullptr;
    TSharedPtr<FJsonObject> Err;
    UBTCompositeNode* Parent = BehaviorTree_ResolveComposite(BTPath, ParentName,
        TEXT("BehaviorTree has no root composite; nothing to remove"), TEXT("Parent composite"), BTAsset, Err);
    if (!Parent)
    {
        return Err;
    }

    FBTCompositeChild* Edge = FindChildEdgeByName(Parent, ChildName);
    if (!Edge)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Child '%s' not found under parent composite (nothing to remove a decorator from)"), *ChildName));
    }

    const FString ResolvedParentName = (Parent == BTAsset->RootNode) ? FString(TEXT("Root")) : Parent->GetNodeName();

    // Locate the decorator by index or by NodeName (mirror of add's idempotency scan).
    if (DecoratorIndex >= 0 && !Edge->Decorators.IsValidIndex(DecoratorIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("index %d out of range (child-edge '%s' has %d decorators)"), DecoratorIndex, *ChildName, Edge->Decorators.Num()));
    }
    FString FoundName;
    const int32 FoundIndex = BehaviorTree_FindAuxNode(Edge->Decorators, DecoratorIndex, DecoratorName, FoundName);

    // Idempotent: nothing matched -> already_absent, asset untouched.
    if (FoundIndex == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
        R->SetStringField(TEXT("decorator_name"), DecoratorName);
        R->SetBoolField(TEXT("removed"), false);
        R->SetBoolField(TEXT("already_absent"), true);
        R->SetNumberField(TEXT("decorator_count"), Edge->Decorators.Num());
        R->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(R);
    }

    BTAsset->Modify();
    Edge->Decorators.RemoveAt(FoundIndex, 1, EAllowShrinking::No);

    UPackage* Package = BTAsset->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BTAsset, SaveError);

    TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
    R->SetStringField(TEXT("decorator_name"), FoundName);
    R->SetBoolField(TEXT("removed"), true);
    R->SetBoolField(TEXT("already_absent"), false);
    R->SetNumberField(TEXT("decorator_count"), Edge->Decorators.Num());
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("remove_bt_decorator"), SaveError);
}

// MCP:add_bt_service - Attach a service (background tick) to a BehaviorTree composite.  The service is appended to the composite's Services array and ticks while that composite branch is active. Authoring is data-only: ParentNode/ExecutionIndex are assigned by the BT runtime at load (run_behavior_tree). Idempotent by service_name within the composite.
//   @param behavior_tree_path - Path to the UBehaviorTree asset (its RootNode must already be set via set_behavior_tree_root).
//   @param service_class [string!] - Concrete service class: "DefaultFocus" | "RunEQS" | ... (or a BTService_* class name).
//   @param parent_name - Optional composite name to attach the service to; "Root" or omitted targets the RootNode.
//   @param service_name - Optional stable name for the new service (its identity for idempotency); defaults to the service type.
//   @returns { behavior_tree_path: string, parent_name: string, service_name: string, service_class: string, already_exists: bool, service_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddBTService(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
    }
    FString ServiceClassStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("service_class"), ServiceClassStr)) { return Missing; }
    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName);
    FString ServiceName;
    Params->TryGetStringField(TEXT("service_name"), ServiceName);

    // Resolve the composite to attach the service to.
    UBehaviorTree* BTAsset = nullptr;
    TSharedPtr<FJsonObject> Err;
    UBTCompositeNode* Composite = BehaviorTree_ResolveComposite(BTPath, ParentName,
        TEXT("BehaviorTree has no root composite; call set_behavior_tree_root first"), TEXT("Composite"), BTAsset, Err);
    if (!Composite)
    {
        return Err;
    }

    UClass* ServiceClass = ResolveBTNodeClass(ServiceClassStr, UBTService::StaticClass(), TEXT("BTService_"));
    if (!ServiceClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not resolve '%s' to a concrete UBTService subclass (e.g. DefaultFocus/RunEQS)"), *ServiceClassStr));
    }

    // Default the stable service name to the class type (prefix stripped).
    if (ServiceName.IsEmpty())
    {
        ServiceName = ServiceClass->GetName();
        ServiceName.RemoveFromStart(TEXT("BTService_"));
    }

    const FString ResolvedParentName = (Composite == BTAsset->RootNode) ? FString(TEXT("Root")) : Composite->GetNodeName();

    // Idempotent: a service with this name already ticks on this composite.
    for (const UBTService* Existing : Composite->Services)
    {
        if (Existing && Existing->GetNodeName() == ServiceName)
        {
            TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("service_name"), ServiceName);
            R->SetStringField(TEXT("service_class"), Existing->GetClass()->GetName());
            R->SetBoolField(TEXT("already_exists"), true);
            R->SetNumberField(TEXT("service_count"), Composite->Services.Num());
            R->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOkReply(R);
        }
    }

    BTAsset->Modify();

    UBTService* NewService = NewObject<UBTService>(BTAsset, ServiceClass, NAME_None, RF_Transactional);
    if (!NewService)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create service of class %s"), *ServiceClass->GetName()));
    }
    NewService->NodeName = ServiceName;
    Composite->Services.Add(NewService);

    UPackage* Package = BTAsset->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BTAsset, SaveError);

    TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("service_name"), ServiceName);
    R->SetStringField(TEXT("service_class"), ServiceClass->GetName());
    R->SetBoolField(TEXT("already_exists"), false);
    R->SetNumberField(TEXT("service_count"), Composite->Services.Num());
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("add_bt_service"), SaveError);
}

// MCP:remove_bt_service - Remove a service (background tick) from a BehaviorTree composite.  Inverse of add_bt_service: drops the matching entry from the composite's Services array (UBTCompositeNode::Services). Idempotent: a missing service returns removed:false / already_absent:true.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param service_name - Name of the service to remove (a service_name from a prior add_bt_service); provide this or 'index'.
//   @param index [number] - Alternative to service_name: 0-based position in the composite's Services array.
//   @param parent_name - Optional composite name the service is attached to; "Root" or omitted targets the RootNode.
//   @returns { behavior_tree_path: string, parent_name: string, service_name: string, removed: bool, already_absent: bool, service_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveBTService(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
    }
    FString ServiceName;
    Params->TryGetStringField(TEXT("service_name"), ServiceName);
    const int32 ServiceIndex = BehaviorTree_ReadIndexParam(Params);
    if (ServiceName.IsEmpty() && ServiceIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Provide 'service_name' (a service_name from a prior add) or 'index'"));
    }
    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName);

    // Resolve the composite the service is attached to.
    UBehaviorTree* BTAsset = nullptr;
    TSharedPtr<FJsonObject> Err;
    UBTCompositeNode* Composite = BehaviorTree_ResolveComposite(BTPath, ParentName,
        TEXT("BehaviorTree has no root composite; nothing to remove"), TEXT("Composite"), BTAsset, Err);
    if (!Composite)
    {
        return Err;
    }

    const FString ResolvedParentName = (Composite == BTAsset->RootNode) ? FString(TEXT("Root")) : Composite->GetNodeName();

    // Locate the service by index or by NodeName (mirror of add's idempotency scan).
    if (ServiceIndex >= 0 && !Composite->Services.IsValidIndex(ServiceIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("index %d out of range (composite '%s' has %d services)"), ServiceIndex, *ResolvedParentName, Composite->Services.Num()));
    }
    FString FoundName;
    const int32 FoundIndex = BehaviorTree_FindAuxNode(Composite->Services, ServiceIndex, ServiceName, FoundName);

    // Idempotent: nothing matched -> already_absent, asset untouched.
    if (FoundIndex == INDEX_NONE)
    {
        TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("service_name"), ServiceName);
        R->SetBoolField(TEXT("removed"), false);
        R->SetBoolField(TEXT("already_absent"), true);
        R->SetNumberField(TEXT("service_count"), Composite->Services.Num());
        R->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(R);
    }

    BTAsset->Modify();
    Composite->Services.RemoveAt(FoundIndex, 1, EAllowShrinking::No);

    UPackage* Package = BTAsset->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, BTAsset, SaveError);

    TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("service_name"), FoundName);
    R->SetBoolField(TEXT("removed"), true);
    R->SetBoolField(TEXT("already_absent"), false);
    R->SetNumberField(TEXT("service_count"), Composite->Services.Num());
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("remove_bt_service"), SaveError);
}

// ============================================================================
// Runtime BT lifecycle (pause/resume/restart/status) + AI perception (sight radius / perceived actors)
// ============================================================================

// MCP:pause_behavior_tree - Pause the runtime BT on an AI-possessed pawn (UBehaviorTreeComponent::PauseLogic).
//   @param actor_name - Required.
//   @returns { actor_name, paused, was_running }
TSharedPtr<FJsonObject> HandlePauseBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> LookupError;
    UBehaviorTreeComponent* BTC = ReadTreeComponent(Params, ActorName, LookupError);
    if (!BTC)
    {
        return LookupError;
    }
    const bool bWasRunning = BTC->IsRunning();
    BTC->PauseLogic(TEXT("mcp_w152_pause"));
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetBoolField(TEXT("paused"), true);
    R->SetBoolField(TEXT("was_running"), bWasRunning);
    return mcp_common::MakeOkReply(R);
}

// MCP:resume_behavior_tree - Resume the runtime BT on a paused AI pawn.
//   @param actor_name - Name or label of the AI-possessed pawn (or AIController) whose UBehaviorTreeComponent to resume.
TSharedPtr<FJsonObject> HandleResumeBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> LookupError;
    UBehaviorTreeComponent* BTC = ReadTreeComponent(Params, ActorName, LookupError);
    if (!BTC)
    {
        return LookupError;
    }
    const bool bWasPaused = BTC->IsPaused();
    BTC->ResumeLogic(TEXT("mcp_w152_resume"));
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetBoolField(TEXT("was_paused"), bWasPaused);
    R->SetBoolField(TEXT("is_paused"), BTC->IsPaused());
    return mcp_common::MakeOkReply(R);
}

// MCP:restart_behavior_tree - Restart the runtime BT from the root (RestartTree).
//   @param actor_name - Name or label of the AI-possessed pawn (or AIController) whose UBehaviorTreeComponent to restart.
TSharedPtr<FJsonObject> HandleRestartBehaviorTree(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> LookupError;
    UBehaviorTreeComponent* BTC = ReadTreeComponent(Params, ActorName, LookupError);
    if (!BTC)
    {
        return LookupError;
    }
    BTC->RestartTree();
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    R->SetBoolField(TEXT("restarted"), true);
    return mcp_common::MakeOkReply(R);
}

// MCP:get_behavior_tree_status - Reader: runtime BT status + active task name (if any) on an AI pawn.
//   @param actor_name - Name or label of the AI-possessed pawn (or AIController) whose UBehaviorTreeComponent to query.
//   @returns { actor_name, run_status, run_status_name, active_task_name, bt_asset_name }
TSharedPtr<FJsonObject> HandleGetBehaviorTreeStatus(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'actor_name'"));
    }
    AActor* Actor = FindActorByName(ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }
    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_name"), ActorName);
    UBehaviorTreeComponent* BTC = ResolveBehaviorTreeComponent(Actor);
    if (!BTC)
    {
        R->SetBoolField(TEXT("has_bt"), false);
        return mcp_common::MakeOkReply(R);
    }
    R->SetBoolField(TEXT("has_bt"), true);
    R->SetBoolField(TEXT("is_running"), BTC->IsRunning());
    R->SetBoolField(TEXT("is_paused"), BTC->IsPaused());
    R->SetStringField(TEXT("bt_asset_name"), BTC->GetCurrentTree() ? BTC->GetCurrentTree()->GetName() : FString());
    return mcp_common::MakeOkReply(R);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_blackboard_key"), &HandleAddBlackboardKey },
    { TEXT("add_bt_composite_child"), &HandleAddBTCompositeChild },
    { TEXT("add_bt_decorator"), &HandleAddBTDecorator },
    { TEXT("add_bt_service"), &HandleAddBTService },
    { TEXT("add_bt_task_child"), &HandleAddBTTaskChild },
    { TEXT("create_behavior_tree"), &HandleCreateBehaviorTree },
    { TEXT("create_blackboard_data"), &HandleCreateBlackboardData },
    { TEXT("get_behavior_tree_info"), &HandleGetBehaviorTreeInfo },
    { TEXT("get_behavior_tree_status"), &HandleGetBehaviorTreeStatus },
    { TEXT("get_blackboard_component"), &HandleGetBlackboardComponent },
    { TEXT("get_blackboard_data"), &HandleGetBlackboardData },
    { TEXT("get_blackboard_keys"), &HandleGetBlackboardKeys },
    { TEXT("get_blackboard_keys_asset"), &HandleGetBlackboardKeysAsset },
    { TEXT("get_blackboard_value"), &HandleGetBlackboardValue },
    { TEXT("pause_behavior_tree"), &HandlePauseBehaviorTree },
    { TEXT("remove_blackboard_key"), &HandleRemoveBlackboardKey },
    { TEXT("remove_bt_composite_child"), &HandleRemoveBTCompositeChild },
    { TEXT("remove_bt_decorator"), &HandleRemoveBTDecorator },
    { TEXT("remove_bt_service"), &HandleRemoveBTService },
    { TEXT("remove_bt_task_child"), &HandleRemoveBTTaskChild },
    { TEXT("restart_behavior_tree"), &HandleRestartBehaviorTree },
    { TEXT("resume_behavior_tree"), &HandleResumeBehaviorTree },
    { TEXT("run_behavior_tree"), &HandleRunBehaviorTree },
    { TEXT("set_behavior_tree_root"), &HandleSetBehaviorTreeRoot },
    { TEXT("set_blackboard_key_subsettings"), &HandleSetBlackboardKeySubsettings },
    { TEXT("set_blackboard_value"), &HandleSetBlackboardValue },
    { TEXT("stop_behavior_tree"), &HandleStopBehaviorTree },
};
} // namespace UEAgentBehaviorTreeCommands
