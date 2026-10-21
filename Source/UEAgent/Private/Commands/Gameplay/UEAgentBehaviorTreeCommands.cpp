// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Gameplay/GameplayActorLookup.h"
#include "Commands/Common/McpJsonPropertyApply.h"   // set/get_behavior_tree_node_property: dotted paths and the honest JSON applier
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
#include "BehaviorTree/ValueOrBBKey.h"
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

    /**
     * BBData and its parent chain, root blackboard first and BBData last: the order in which the engine numbers
     * their keys (UBlackboardData::UpdateKeyIDs starts a blackboard's IDs after all of its parent's). A Parent
     * loop is cut where an asset would repeat.
     */
    TArray<const UBlackboardData*> BehaviorTree_BlackboardChain(const UBlackboardData* BBData)
    {
        TArray<const UBlackboardData*> Chain;
        for (const UBlackboardData* It = BBData; It != nullptr && !Chain.Contains(It); It = It->Parent.Get())
        {
            Chain.Insert(It, 0);
        }
        return Chain;
    }

    /** Adds inherited (whether Owner is not BBData) and defined_in (Owner's path) to a key's JSON. */
    void BehaviorTree_AddKeyOwner(const TSharedPtr<FJsonObject>& KeyObj, const UBlackboardData* Owner, const UBlackboardData* BBData)
    {
        KeyObj->SetBoolField(TEXT("inherited"), Owner != BBData);
        KeyObj->SetStringField(TEXT("defined_in"), Owner->GetPathName());
    }

    /**
     * MakeKeyJson of every key BBData gives its users in key-ID order: the keys of its parent chain first, then
     * its own, each with inherited and defined_in. GetKeys() alone holds only the asset's own keys.
     */
    TArray<TSharedPtr<FJsonValue>> BehaviorTree_AllKeysToJson(const UBlackboardData* BBData)
    {
        TArray<TSharedPtr<FJsonValue>> KeysArr;
        for (const UBlackboardData* Owner : BehaviorTree_BlackboardChain(BBData))
        {
            for (const FBlackboardEntry& Entry : Owner->GetKeys())
            {
                TSharedPtr<FJsonObject> KeyObj = MakeKeyJson(Entry);
                BehaviorTree_AddKeyOwner(KeyObj, Owner, BBData);
                KeysArr.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
        }
        return KeysArr;
    }

    /** { blackboard_asset, name, parent?, keys, key_count } of a blackboard asset, its inherited keys included. */
    TSharedPtr<FJsonObject> BehaviorTree_BlackboardAssetData(const UBlackboardData* BBData)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("blackboard_asset"), BBData->GetPathName());
        Result->SetStringField(TEXT("name"), BBData->GetName());
        if (BBData->Parent)
        {
            Result->SetStringField(TEXT("parent"), BBData->Parent->GetPathName());
        }

        const TArray<TSharedPtr<FJsonValue>> KeysArr = BehaviorTree_AllKeysToJson(BBData);
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

// MCP:get_behavior_tree_info - Read a UBehaviorTree asset found by content path: its name, its blackboard asset with every key it holds in key-ID order, parent blackboard keys first (name, type, instance_synced, the base class or enum subsettings, inherited, and defined_in naming the blackboard that declares the key), and the node tree from RootNode -- each node's name and class, a composite's services and children, and the decorators on each child edge, nested up to 64 levels. Nothing is written. A missing path, or one that does not load as a behavior tree, is an error; a tree with no root gives root_node null. The runtime state of a tree running on an AI actor is get_behavior_tree_status; blackboard asset details: get_blackboard_data; build the tree with set_behavior_tree_root, add_behavior_tree_child, add_behavior_tree_decorator and add_behavior_tree_service. A node's settings: get_behavior_tree_node_properties.
//   @param behavior_tree_path - Content path of the UBehaviorTree asset, e.g. "/Game/AI/BT_Enemy"; a bare asset name is not resolved
//   @returns { behavior_tree_path: string, name: string, blackboard_asset: string|null, blackboard_keys: [{ name: string, type: string, instance_synced: bool, inherited: bool, defined_in: string }], root_node: object|null }

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
        Result->SetArrayField(TEXT("blackboard_keys"), BehaviorTree_AllKeysToJson(BBData));
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

// MCP:run_behavior_tree - Start a BehaviorTree asset with AAIController::RunBehaviorTree on an AIController or the pawn it possesses. The actor is searched in the editor world only, never PIE, where pawns are not auto-possessed, so it usually errors with no AIController. Returns started and ai_controller; nothing is saved.
//   @param actor_name - Object name or label of the pawn or AIController in the editor world (first match).
//   @param behavior_tree_path - Asset path of the UBehaviorTree, e.g. "/Game/AI/BT_Enemy"; a bare asset name is not resolved.
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

// MCP:stop_behavior_tree - Stop the behavior tree of an editor-world actor with StopTree (Safe mode), through its AIController brain component or else a UBehaviorTreeComponent on the actor. Runtime state only, nothing saved; editor world only, never PIE. was_running is read before the stop (running and not paused); stopped is true when a started tree, running or paused, is no longer started afterwards. A tree that was not started is not an error: success with stopped false and a note. A tree still started after the call (a task still aborting) is an error. An unknown actor, or one with no behavior tree component, is an error. Unlike pause_behavior_tree it cannot be continued with resume_behavior_tree: start again with restart_behavior_tree (the last tree) or run_behavior_tree.
//   @param actor_name - Object name or label of the pawn or AIController in the editor world (first match)
//   @returns { actor_name: string, was_running: bool, stopped: bool, note?: string }

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

    // A started tree (running or paused) is what StopTree tears down. In Safe mode it returns early, with the
    // tree still started, while a task finishes aborting or while the component has branch actions suspended,
    // so the stop is measured on the tree itself.
    const bool bWasRunning = BTComp->IsRunning();
    const bool bWasStarted = BTComp->TreeHasBeenStarted();
    BTComp->StopTree(EBTStopMode::Safe);
    const bool bStillStarted = BTComp->TreeHasBeenStarted();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetBoolField(TEXT("was_running"), bWasRunning);
    Result->SetBoolField(TEXT("stopped"), bWasStarted && !bStillStarted);
    if (bStillStarted)
    {
        return mcp_common::MakeOutcomeReply(Result, false, FString::Printf(
            TEXT("stop_behavior_tree: the tree of '%s' is still started after StopTree (a task is still aborting); call again"),
            *ActorName));
    }
    if (!bWasStarted)
    {
        Result->SetStringField(TEXT("note"), TEXT("No behavior tree was started on this actor; nothing was stopped."));
    }

    return mcp_common::MakeOkReply(Result);
}

//----------------------------------------------------------------------//
// Runtime blackboard
//----------------------------------------------------------------------//

// MCP:set_blackboard_value - Write one key of the running blackboard of an editor-world actor, converting the value string by key type. Not read back: set is always true, and an unparsable number or vector is written as 0. Runtime state only, nothing saved; editor world only, never PIE. Verify with get_blackboard_value.
//   @param actor_name - Name of the actor whose blackboard to modify
//   @param key_name - Name of the blackboard key to set
//   @param value - Value as a string, converted by key type: "42", "0.5", "true" or "1", a name, an enum index, a vector "X=1 Y=2 Z=3", a rotator "P=0 Y=90 R=0", an object or class path. Omitted means empty (0, false, none).
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

// MCP:get_blackboard_value - Read one key of the running blackboard of an editor-world actor (its AIController blackboard, else one on the actor). value is a number for Int, Float and Enum keys, a bool for Bool, else a string (objects as paths). Read-only; editor world only, never PIE.
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

// MCP:get_blackboard_data - Dump a blackboard from its asset or from a running component. With blackboard_path, which wins when both are given, it reads the UBlackboardData asset: name, parent and key definitions (name, type, instance_synced, base class or enum subsettings). With actor_name, it reads the running UBlackboardComponent of that actor in the editor world, never PIE (its AIController blackboard, else one on the actor): the component's name, has_valid_asset and num_keys, the key definitions of its blackboard asset as keys, and entries with each key's name, type and current value as text; a component with no asset gives empty lists. Either way every key is listed in key-ID order, those inherited from parent blackboards first, each with inherited and defined_in naming the asset that declares it. Nothing is written. No parameter, an asset that does not load, an actor not found by object name or label, or no running blackboard is an error. One key: get_blackboard_value; write a value: set_blackboard_value; add a key: add_blackboard_key.
//   @param actor_name - Object name or label of the editor-world pawn, its AIController, or an actor with its own UBlackboardComponent, whose running blackboard to dump (first match); ignored when blackboard_path is given
//   @param blackboard_path - Content path of a UBlackboardData asset to read instead, e.g. "/Game/AI/BB_Enemy"; takes precedence over actor_name
//   @returns { actor_name: string?, blackboard_asset: string?, name: string?, parent: string?, component_name: string? (actor mode), has_valid_asset: bool? (actor mode), num_keys: number? (actor mode), keys: [{ name, type, instance_synced, base_class?, enum_type?, enum_name?, inherited, defined_in }], entries: [{ name, type, value, inherited, defined_in }]? (actor mode), key_count: number }

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

        return mcp_common::MakeOkReply(BehaviorTree_BlackboardAssetData(BBData));
    }

    // Live mode. actor_name is needed only here, so it is read as an optional param: blackboard_path alone is
    // a complete request.
    FString ActorName;
    const bool bHasActor = Params.IsValid() && Params->TryGetStringField(TEXT("actor_name"), ActorName) && !ActorName.IsEmpty();
    if (!bHasActor)
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
    Result->SetStringField(TEXT("component_name"), BB->GetName());
    Result->SetBoolField(TEXT("has_valid_asset"), BB->HasValidAsset());
    Result->SetNumberField(TEXT("num_keys"), BB->GetNumKeys());

    // The key definitions (instance_synced, base class or enum subsettings), which entries do not carry.
    UBlackboardData* BBData = BB->GetBlackboardAsset();
    if (BBData)
    {
        Result->SetStringField(TEXT("blackboard_asset"), BBData->GetPathName());
    }
    Result->SetArrayField(TEXT("keys"), BBData ? BehaviorTree_AllKeysToJson(BBData) : TArray<TSharedPtr<FJsonValue>>());

    // Every key the component holds, in key-ID order: the parent chain's keys first, then the asset's own.
    TArray<TSharedPtr<FJsonValue>> EntriesArr;
    if (BBData)
    {
        for (const UBlackboardData* Owner : BehaviorTree_BlackboardChain(BBData))
        {
            for (const FBlackboardEntry& Entry : Owner->GetKeys())
            {
                TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
                EntryObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
                EntryObj->SetStringField(TEXT("type"), DescribeKeyTypeClass(Entry.KeyType));
                EntryObj->SetStringField(TEXT("value"), BB->DescribeKeyValue(Entry.EntryName, EBlackboardDescription::OnlyValue));
                BehaviorTree_AddKeyOwner(EntryObj, Owner, BBData);
                EntriesArr.Add(MakeShared<FJsonValueObject>(EntryObj));
            }
        }
    }
    Result->SetArrayField(TEXT("entries"), EntriesArr);
    Result->SetNumberField(TEXT("key_count"), EntriesArr.Num());

    return mcp_common::MakeOkReply(Result);
}

//----------------------------------------------------------------------//
// Asset authoring (Blackboard + BehaviorTree)
//----------------------------------------------------------------------//

// MCP:create_blackboard_data - Create a UBlackboardData asset with no parent at package_path/name (default folder /Game/AI), register it and save it; no undo step. Its only key is the one the engine seeds every new blackboard with, the SelfActor Object key (while AISystem bAddBlackboardSelfKey is on, the default), counted in key_count. Idempotent: a blackboard already at that path is returned unchanged with already_exists=true, saved=false, a message and its key_count, nothing saved. An invalid package name, or a name taken by an asset of another class, is an error; a failed save of the new asset -- refused while PIE runs, under a read-only root, or a read-only file -- gives success false with saved=false and save_error. blackboard_path is the object path the other blackboard tools take. Add keys with add_blackboard_key and constrain them with set_blackboard_key_subsettings; read them with get_blackboard_data; bind it to a tree with create_behavior_tree.
//   @param name - Asset name (e.g. "BB_Enemy").
//   @param package_path - Target /Game package path (optional, default "/Game/AI")
//   @returns { name: string, package_path: string, blackboard_path: string, asset_path: string, key_count: number, already_exists: bool, saved: bool, save_error: string?, message: string? }

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
        // Returned unchanged: nothing was written, so nothing is saved.
        R->SetBoolField(TEXT("saved"), false);
        R->SetStringField(TEXT("message"), TEXT("BlackboardData already exists; returned it unchanged, nothing saved"));
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

// MCP:add_blackboard_key - Append one key to the own Keys of a UBlackboardData asset loaded by path, re-index its key IDs and save it; no undo step. key_type is Bool, Int, Float, Vector, Rotator, String, Name, Object, Class or Enum (case-insensitive); instance_synced defaults to false. An Object, Class or Enum key gets no base class or enum here: use set_blackboard_key_subsettings. Idempotent: a key of that name and type already there is a success with already_exists=true and saved=false, nothing added or saved (its instance_synced untouched); the same name with another type is an error. A name a parent blackboard already has is an error naming that parent, nothing added: a key of that name would shadow the parent's. verified confirms the key resolves by name. A failed save gives success false, saved=false and save_error, with the key added in memory. Inverse: remove_blackboard_key; read: get_blackboard_data; create the asset: create_blackboard_data.
//   @param blackboard_path - Path to the UBlackboardData asset.
//   @param key_name - New key name (must be unique on the asset).
//   @param key_type - One of Bool|Int|Float|Vector|Rotator|String|Name|Object|Class|Enum.
//   @param instance_synced - Whether the key is instance-synced (optional, default false)
//   @returns { blackboard_path: string, key_name: string, key_type: string, key_count: number, verified: bool, saved: bool, already_exists: bool?, save_error: string?, note: string?, keys: [{ name, type, instance_synced }] }

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

    // A name a parent blackboard already has is refused: the engine numbers the parent's keys first, so a key of
    // the same name here would shadow it. The parent chain is walked once; a cycle stops the walk.
    TSet<const UBlackboardData*> Visited;
    Visited.Add(BBData);
    for (const UBlackboardData* Ancestor = BBData->Parent; Ancestor && !Visited.Contains(Ancestor); Ancestor = Ancestor->Parent)
    {
        Visited.Add(Ancestor);
        for (const FBlackboardEntry& Inherited : Ancestor->Keys)
        {
            if (Inherited.EntryName == KeyFName)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Blackboard key '%s' already exists in the parent blackboard %s (type %s); a key of that name on %s ")
                    TEXT("would shadow it. Nothing was added."),
                    *KeyName, *Ancestor->GetPathName(), *DescribeKeyTypeClass(Inherited.KeyType), *BBData->GetName()));
            }
        }
    }

    // Idempotent: if key already exists with the same type, return success. The type is compared as resolved
    // (KeyTypeClass), not as the token was spelled.
    for (const FBlackboardEntry& Existing : BBData->Keys)
    {
        if (Existing.EntryName == KeyFName)
        {
            // Check if type matches
            FString ExistingType = DescribeKeyTypeClass(Existing.KeyType);
            if (Existing.KeyType && Existing.KeyType->GetClass() == KeyTypeClass)
            {
                // Key already exists with same type — return success (idempotent)
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetStringField(TEXT("blackboard_path"), BBData->GetPathName());
                Result->SetStringField(TEXT("key_name"), KeyName);
                Result->SetStringField(TEXT("key_type"), ExistingType);
                Result->SetNumberField(TEXT("key_count"), BBData->Keys.Num());
                Result->SetBoolField(TEXT("verified"), true);
                // Nothing was added, so nothing is saved.
                Result->SetBoolField(TEXT("saved"), false);
                Result->SetBoolField(TEXT("already_exists"), true);
                Result->SetStringField(TEXT("note"), TEXT("a key of that name and type already exists; nothing added, nothing saved"));
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
                        *KeyName, *BBData->GetName(), *ExistingType, *KeyTypeToken.TrimStartAndEnd()));
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

// MCP:set_blackboard_key_subsettings - Constrain an existing blackboard key's subsettings on a UBlackboardData asset and save. This is the SECOND HALF of add_blackboard_key, which only sets EntryName/KeyType/bInstanceSynced: it fills the per-type filter the editor would otherwise make you pick by hand -- BaseClass for an Object/Class key, EnumType for an Enum key. At least one of base_class / enum_type is required, and the one you supply must match the key's type. A value already in place is a no-op: already_set=true and saved=false, nothing saved.
//   @param blackboard_path - Path to the UBlackboardData asset.
//   @param key_name - Name of the existing key to constrain.
//   @param base_class - Class path/name (e.g. "/Script/Engine.Actor" or "Actor") for an Object/Class key (optional).
//   @param enum_type - Enum path/name (e.g. "/Script/Engine.ECollisionChannel") for an Enum key (optional).
//   @returns { blackboard_path: string, key_name: string, key_type: string, base_class: string?, enum_type: string?, enum_name: string?, already_set: bool, saved: bool, save_error: string?, note: string?, keys: [...] }

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
    Result->SetBoolField(TEXT("saved"), false);
    Result->SetStringField(TEXT("note"), TEXT("the key already has these subsettings; nothing changed, nothing saved"));
    Result->SetArrayField(TEXT("keys"), BehaviorTree_KeysToJson(BBData));

    return mcp_common::MakeOkReply(Result);
}

// MCP:create_behavior_tree - Create a UBehaviorTree asset at package_path/name (default folder /Game/AI), optionally bound to the UBlackboardData at blackboard_path, register it and save it; no undo step, and the tree starts with no root node (add one with set_behavior_tree_root). An existing tree at that path is returned with already_exists=true: when blackboard_path names a different blackboard it is rebound and saved (blackboard_rebound=true), otherwise it is left unchanged with saved=false and a message, nothing saved, so a repeat call is idempotent. A blackboard_path that does not load, or a name taken by an asset of another class, is an error; a failed save of a new or rebound tree gives success false with saved=false and save_error. Make the blackboard with create_blackboard_data; read the tree with get_behavior_tree_info; add nodes with add_behavior_tree_child.
//   @param name - Asset name (e.g. "BT_Enemy").
//   @param package_path - Target /Game package path (optional, default "/Game/AI")
//   @param blackboard_path - Path to a UBlackboardData asset to bind to the new BT (optional)
//   @returns { name: string, package_path: string, behavior_tree_path: string, asset_path: string, blackboard_asset: string?, already_exists: bool, blackboard_rebound: bool?, saved: bool, save_error: string?, message: string? }

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
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

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
        // Saved only when the blackboard binding changed; an existing tree left as it is saves nothing.
        bool bSavedExisting = false;
        FString SaveErr;
        if (bUpdated)
        {
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
        R->SetBoolField(TEXT("blackboard_rebound"), bUpdated);
        if (bUpdated)
        {
            return mcp_common::MakeSavedReply(R, bSavedExisting, TEXT("create_behavior_tree"), SaveErr);
        }
        R->SetBoolField(TEXT("saved"), false);
        R->SetStringField(TEXT("message"), TEXT("BehaviorTree already exists; returned it unchanged, nothing saved"));
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

    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_behavior_tree"), SaveError);
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
    // add_behavior_tree_child: a composite child when 'composite' is given, a task child when 'task_class' is.
    TSharedPtr<FJsonObject> AddBTChildImpl(const TSharedPtr<FJsonObject>& Params)
    {
        FString BTPath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
        }
        // Exactly one of the two class params: it decides the kind of the child edge.
        FString CompositeStr;
        Params->TryGetStringField(TEXT("composite"), CompositeStr);
        FString TaskClassStr;
        Params->TryGetStringField(TEXT("task_class"), TaskClassStr);
        if (CompositeStr.IsEmpty() == TaskClassStr.IsEmpty())
        {
            return mcp_common::MakeErrorReply(CompositeStr.IsEmpty()
                ? TEXT("Provide 'composite' (a composite child, e.g. Selector) or 'task_class' (a task child, e.g. Wait)")
                : TEXT("Provide only one of 'composite' and 'task_class': a child is either a composite or a task"));
        }
        const bool bComposite = !CompositeStr.IsEmpty();
        const FString NodeClassStr = bComposite ? CompositeStr : TaskClassStr;
        UClass* Base = bComposite ? UBTCompositeNode::StaticClass() : UBTTaskNode::StaticClass();
        const TCHAR* Prefix = bComposite ? TEXT("BTComposite_") : TEXT("BTTask_");
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

// MCP:add_behavior_tree_child - Add a child under a BehaviorTree composite: a composite (Selector/Sequence/SimpleParallel) when composite is given, or a leaf task (Wait/MoveTo/PlaySound/RunBehavior/...) when task_class is given; exactly one of the two. The child is appended to the parent's Children (array order = execution order). Authoring is data-only: ParentNode/ExecutionIndex are assigned by the BT runtime at load (run_behavior_tree). Idempotent by child_name within the parent, whatever its kind. The returned child_name is how you address a composite as a parent for further children. Configure the node (a MoveTo's key, a Wait's time) with set_behavior_tree_node_property; read its settings with get_behavior_tree_node_properties.
//   @param behavior_tree_path - Path to the UBehaviorTree asset (its RootNode must already be set via set_behavior_tree_root).
//   @param composite [string] - Concrete composite class: "Selector" | "Sequence" | "SimpleParallel" (or a BTComposite_* class name). Give this or task_class, not both.
//   @param task_class [string] - Concrete task class: "Wait" | "MoveTo" | "PlaySound" | "RunBehavior" | "FinishWithResult" (or a BTTask_* class name). Give this or composite, not both.
//   @param parent_name - Optional parent composite name (a child_name from a prior add); "Root" or omitted targets the RootNode.
//   @param child_name - Optional stable name for the new node (its identity for idempotency/addressing); defaults to the class type without its BTComposite_/BTTask_ prefix.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, child_class: string, child_kind: "composite"|"task", already_exists: bool, child_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddBehaviorTreeChild(const TSharedPtr<FJsonObject>& Params)
{
    return AddBTChildImpl(Params);
}

// ===========================================================================
// Inverse CRUD of add_behavior_tree_child. Removes one entry from the parent
// composite's Children (a TArray<FBTCompositeChild>) — the exact array the add
// appends to. Addressing mirrors the add path: parent_name (empty/"Root" ->
// RootNode) resolves the composite; the child is then located by its stable
// child_name (a returned identity from a prior add) or by index. An optional kind
// (composite -> ChildComposite set; task -> ChildTask set) restricts the match to
// that edge kind; omitted, an edge of either kind matches.
// Idempotent: a missing child reports removed:false / already_absent:true.
// The freed node is a plain UObject (not a UEdGraphNode) held only by the dropped
// FBTCompositeChild wrapper, so RemoveAt is sufficient — GC reclaims it, mirroring
// the data-only NewObject+.Add of the add (no DestroyNode to mirror).
// ===========================================================================
namespace
{
    // The node an edge holds when it is of the wanted kind (any kind when KindFilter is empty), else null.
    const UBTNode* EdgeNodeOfKind(const FBTCompositeChild& Edge, const FString& KindFilter)
    {
        if (Edge.ChildComposite && (KindFilter.IsEmpty() || KindFilter == TEXT("composite")))
        {
            return Edge.ChildComposite;
        }
        if (Edge.ChildTask && (KindFilter.IsEmpty() || KindFilter == TEXT("task")))
        {
            return Edge.ChildTask;
        }
        return nullptr;
    }

    // remove_behavior_tree_child.
    TSharedPtr<FJsonObject> RemoveBTChildImpl(const TSharedPtr<FJsonObject>& Params)
    {
        FString BTPath;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("behavior_tree_path"), BTPath) || BTPath.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter 'behavior_tree_path'"));
        }
        // Optional edge kind: "composite" or "task" (any case); omitted, either kind matches.
        FString KindFilter;
        Params->TryGetStringField(TEXT("kind"), KindFilter);
        KindFilter = KindFilter.ToLower();
        if (!KindFilter.IsEmpty() && KindFilter != TEXT("composite") && KindFilter != TEXT("task"))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("kind must be 'composite' or 'task' (or omitted for either), got '%s'"), *KindFilter));
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

        // Locate the matching child edge. By index the edge kind must match kind when it is given;
        // by name only edges of that kind are scanned, so a node of the other kind is untouched.
        int32 FoundIndex = INDEX_NONE;
        FString FoundName;
        FString FoundKind;
        if (ChildIndex >= 0)
        {
            if (!Parent->Children.IsValidIndex(ChildIndex))
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("index %d out of range (parent '%s' has %d children)"), ChildIndex, *ResolvedParentName, Parent->Children.Num()));
            }
            const FBTCompositeChild& Edge = Parent->Children[ChildIndex];
            const UBTNode* AtIdx = EdgeNodeOfKind(Edge, KindFilter);
            if (!AtIdx)
            {
                return mcp_common::MakeErrorReply(KindFilter.IsEmpty()
                    ? FString::Printf(TEXT("Child at index %d under '%s' holds no node"), ChildIndex, *ResolvedParentName)
                    : FString::Printf(TEXT("Child at index %d under '%s' is not a %s"), ChildIndex, *ResolvedParentName, *KindFilter));
            }
            FoundIndex = ChildIndex;
            FoundName = AtIdx->GetNodeName();
            FoundKind = AtIdx->IsA<UBTCompositeNode>() ? TEXT("composite") : TEXT("task");
        }
        else
        {
            for (int32 i = 0; i < Parent->Children.Num(); ++i)
            {
                const UBTNode* Node = EdgeNodeOfKind(Parent->Children[i], KindFilter);
                if (Node && Node->GetNodeName() == ChildName)
                {
                    FoundIndex = i;
                    FoundName = ChildName;
                    FoundKind = Node->IsA<UBTCompositeNode>() ? TEXT("composite") : TEXT("task");
                    break;
                }
            }
        }

        // Idempotent: nothing matched -> report already_absent without touching the asset.
        if (FoundIndex == INDEX_NONE)
        {
            TSharedPtr<FJsonObject> R = BehaviorTree_NodeReply(BTAsset, ResolvedParentName, TEXT("child_name"), ChildName);
            if (!KindFilter.IsEmpty())
            {
                R->SetStringField(TEXT("child_kind"), KindFilter);
            }
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
        R->SetStringField(TEXT("child_kind"), FoundKind);
        R->SetBoolField(TEXT("removed"), true);
        R->SetBoolField(TEXT("already_absent"), false);
        R->SetNumberField(TEXT("child_count"), Parent->Children.Num());
        mcp_common::AddSaveOutcome(R, bSaved, SaveError);
        return mcp_common::MakeOkReply(R);
    }
}

// MCP:remove_behavior_tree_child - Remove a child, composite or task, from a BehaviorTree composite.  Inverse of add_behavior_tree_child: drops the matching entry from the parent's Children array and saves. With kind, only child-edges of that kind are considered (a node of the other kind with the same name is untouched, and an index pointing at the other kind is an error); omitted, an edge of either kind matches. child_kind reports the kind of the edge removed. Idempotent: a missing child returns removed:false / already_absent:true.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param child_name - Name of the child to remove (a child_name from a prior add_behavior_tree_child); provide this or 'index'.
//   @param index [number] - Alternative to child_name: 0-based position in the parent's Children array.
//   @param kind [string] - Optional edge kind to match: "composite" or "task"; omitted matches either. Any other value is an error.
//   @param parent_name - Optional parent composite name owning the child; "Root" or omitted targets the RootNode.
//   @returns { behavior_tree_path: string, parent_name: string, child_name: string, child_kind: "composite"|"task" (the edge removed; the requested kind, or absent, when nothing matched), removed: bool, already_absent: bool, child_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveBehaviorTreeChild(const TSharedPtr<FJsonObject>& Params)
{
    return RemoveBTChildImpl(Params);
}

// ===========================================================================
// Decorator/service authoring (the conditions + background ticks that the
// composites/tasks above lack). Both are UBTAuxiliaryNode subclasses authored
// DATA-ONLY (NewObject owned by the BT package + .Add), exactly like composite children:
// ParentNode/ExecutionIndex are assigned by UBehaviorTreeManager::LoadTree at load.
// A decorator gates ONE child-edge (FBTCompositeChild::Decorators); a service ticks
// while ONE composite is active (UBTCompositeNode::Services). Idempotent by NodeName.
// ===========================================================================

// MCP:add_behavior_tree_decorator - Attach a decorator (execution condition) to a specific child-edge of a BehaviorTree composite.  The decorator is appended to that child's Decorators array and gates whether the child may run. Authoring is data-only: ParentNode/ExecutionIndex are assigned by the BT runtime at load (run_behavior_tree). Idempotent by decorator_name within the child-edge. Configure it (key, condition) with set_behavior_tree_node_property; read: get_behavior_tree_node_properties.
//   @param behavior_tree_path - Path to the UBehaviorTree asset (its RootNode must already be set, with the target child added).
//   @param child_name [string!] - Name of the existing child (a child_name from a prior add_behavior_tree_child) whose edge the decorator gates.
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
            FString::Printf(TEXT("Child '%s' not found under parent composite (add it via add_behavior_tree_child first)"), *ChildName));
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
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("add_behavior_tree_decorator"), SaveError);
}

// MCP:remove_behavior_tree_decorator - Remove a decorator (execution condition) from a specific child-edge of a BehaviorTree composite.  Inverse of add_behavior_tree_decorator: drops the matching entry from that child's Decorators array (FBTCompositeChild::Decorators). Idempotent: a missing decorator returns removed:false / already_absent:true.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param child_name [string!] - Name of the existing child (a child_name from a prior add) whose edge the decorator gates.
//   @param decorator_name - Name of the decorator to remove (a decorator_name from a prior add_behavior_tree_decorator); provide this or 'index'.
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
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("remove_behavior_tree_decorator"), SaveError);
}

// MCP:add_behavior_tree_service - Attach a service (background tick) to a BehaviorTree composite.  The service is appended to the composite's Services array and ticks while that composite branch is active. Authoring is data-only: ParentNode/ExecutionIndex are assigned by the BT runtime at load (run_behavior_tree). Idempotent by service_name within the composite. Configure it with set_behavior_tree_node_property; read: get_behavior_tree_node_properties.
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
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("add_behavior_tree_service"), SaveError);
}

// MCP:remove_behavior_tree_service - Remove a service (background tick) from a BehaviorTree composite.  Inverse of add_behavior_tree_service: drops the matching entry from the composite's Services array (UBTCompositeNode::Services). Idempotent: a missing service returns removed:false / already_absent:true.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param service_name - Name of the service to remove (a service_name from a prior add_behavior_tree_service); provide this or 'index'.
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
    return mcp_common::MakeSavedReply(R, bSaved, TEXT("remove_behavior_tree_service"), SaveError);
}

// ============================================================================
// Runtime BT lifecycle (pause/resume/restart/status) + AI perception (sight radius / perceived actors)
// ============================================================================

// MCP:pause_behavior_tree - Pause the behavior tree of an editor-world actor (its AIController brain component, else a UBehaviorTreeComponent on the actor) with PauseLogic. paused is always true, not read back; was_running is measured. Editor world only, never PIE. Continue it with resume_behavior_tree.
//   @param actor_name - Object name or label of the pawn, its AIController, or an actor with a UBehaviorTreeComponent, in the editor world (first match).
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

// MCP:resume_behavior_tree - Resume a behavior tree paused by pause_behavior_tree (ResumeLogic) on an editor-world actor, through its AIController brain component or a UBehaviorTreeComponent on the actor. Returns was_paused and is_paused, both read from the component. Editor world only, never PIE.
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

// MCP:restart_behavior_tree - Restart the current behavior tree of an editor-world actor from its root (RestartTree), through its AIController brain component or a UBehaviorTreeComponent on the actor. restarted is always true, not measured: check get_behavior_tree_status. Editor world only, never PIE.
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

// MCP:get_behavior_tree_status - Read the live state of the behavior tree component driving one actor in the editor world: has_bt (whether a UBehaviorTreeComponent was found, through the actor's AIController brain or on the actor itself), is_running, is_paused and bt_asset_name, the tree it is running. It does not report the active task or a status enum. Read-only. The actor is searched in the editor world only, never PIE, so it sees a tree started there with run_behavior_tree; an actor without a tree is a success with has_bt false, and an actor that is not found is an error. Inspect the tree asset itself with get_behavior_tree_info and blackboard values with get_blackboard_value; change the run state with pause_behavior_tree, resume_behavior_tree, restart_behavior_tree or stop_behavior_tree.
//   @param actor_name - Object name or label of the AI-possessed pawn, or of its AIController, in the editor world, e.g. 'BP_Guard_C_0'.
//   @returns { actor_name, has_bt: bool, is_running: bool, is_paused: bool, bt_asset_name: string }
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

// ===========================================================================
// Node settings: get_behavior_tree_node_properties / set_behavior_tree_node_property. The add_* tools place a node
// with a class and a name only; what a MoveTo moves to, how long a Wait waits, which key a Blackboard decorator
// tests are the node's own UPROPERTYs, reached here by reflection. Every node of the tree is addressed by its
// NodeName: composites, tasks, the decorators of each child-edge, services and the root decorators.
// ===========================================================================
namespace
{
    struct FBTNodeHit
    {
        UBTNode* Node = nullptr;
        FString Role;        // root | composite | task | decorator | service | root_decorator
        FString ParentName;  // the composite (or task, for a task's service) that holds it; "Root" for the root
    };

    void CollectBTComposite(UBTCompositeNode* Composite, const FString& ParentName, bool bIsRoot, TArray<FBTNodeHit>& Out)
    {
        if (!Composite)
        {
            return;
        }
        Out.Add({ Composite, bIsRoot ? TEXT("root") : TEXT("composite"), ParentName });
        const FString Me = bIsRoot ? FString(TEXT("Root")) : Composite->GetNodeName();
        for (UBTService* Service : Composite->Services)
        {
            if (Service) { Out.Add({ Service, TEXT("service"), Me }); }
        }
        for (FBTCompositeChild& Edge : Composite->Children)
        {
            for (UBTDecorator* Decorator : Edge.Decorators)
            {
                if (Decorator) { Out.Add({ Decorator, TEXT("decorator"), Me }); }
            }
            if (Edge.ChildComposite)
            {
                CollectBTComposite(Edge.ChildComposite, Me, false, Out);
            }
            else if (Edge.ChildTask)
            {
                Out.Add({ Edge.ChildTask, TEXT("task"), Me });
                for (UBTService* Service : Edge.ChildTask->Services)
                {
                    if (Service) { Out.Add({ Service, TEXT("service"), Edge.ChildTask->GetNodeName() }); }
                }
            }
        }
    }

    /**
     * The node of the tree at BTPath called NodeName (narrowed to the ones ParentName holds when it is given), with
     * its tree; the error reply when the tree, the node or a single match cannot be found -- the reply of an
     * ambiguous name lists each match's role and parent.
     */
    TSharedPtr<FJsonObject> BehaviorTree_FindNamedNode(const FString& BTPath, const FString& NodeName, const FString& ParentName,
        UBehaviorTree*& OutTree, FBTNodeHit& OutHit)
    {
        OutTree = mcp_common::LoadAssetOrSoftPath<UBehaviorTree>(BTPath);
        if (!OutTree)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("BehaviorTree asset not found: %s"), *BTPath));
        }
        TArray<FBTNodeHit> All;
        for (UBTDecorator* Decorator : OutTree->RootDecorators)
        {
            if (Decorator) { All.Add({ Decorator, TEXT("root_decorator"), TEXT("Root") }); }
        }
        CollectBTComposite(OutTree->RootNode, FString(), true, All);

        TArray<FBTNodeHit> Matches;
        for (const FBTNodeHit& Hit : All)
        {
            const bool bNameOk = Hit.Node->GetNodeName() == NodeName
                || (Hit.Role == TEXT("root") && NodeName.Equals(TEXT("Root"), ESearchCase::IgnoreCase));
            if (bNameOk && (ParentName.IsEmpty() || Hit.ParentName.Equals(ParentName, ESearchCase::IgnoreCase)))
            {
                Matches.Add(Hit);
            }
        }
        if (Matches.Num() == 1)
        {
            OutHit = Matches[0];
            return nullptr;
        }
        if (Matches.Num() == 0)
        {
            TArray<FString> Names;
            for (const FBTNodeHit& Hit : All)
            {
                Names.Add(FString::Printf(TEXT("%s (%s under %s)"), *Hit.Node->GetNodeName(), *Hit.Role, *Hit.ParentName));
            }
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("No node '%s'%s in %s; its nodes: %s"), *NodeName,
                ParentName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" under '%s'"), *ParentName), *BTPath,
                Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none (set_behavior_tree_root first)")));
        }
        TArray<FString> Where;
        for (const FBTNodeHit& Hit : Matches)
        {
            Where.Add(FString::Printf(TEXT("%s under %s"), *Hit.Role, *Hit.ParentName));
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("%d nodes are named '%s' (%s); pass parent_name to pick one"),
            Matches.Num(), *NodeName, *FString::Join(Where, TEXT("; "))));
    }

    /** True for a struct that is a blackboard-key-or-value pair (FValueOrBBKey_*): it has both Key and DefaultValue. */
    bool IsValueOrBBKeyStruct(const UScriptStruct* Struct)
    {
        return Struct && Struct->FindPropertyByName(TEXT("Key")) && Struct->FindPropertyByName(TEXT("DefaultValue"));
    }

    /** One property of a node as JSON: a key selector reads as its key name, everything else as typed JSON. */
    TSharedPtr<FJsonObject> BTPropertyJson(FProperty* Property, UBTNode* Node)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("name"), Property->GetName());
        Out->SetStringField(TEXT("type"), Property->GetCPPType());
        Out->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
        const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
        if (StructProperty && StructProperty->Struct == FBlackboardKeySelector::StaticStruct())
        {
            const FBlackboardKeySelector* Selector = static_cast<const FBlackboardKeySelector*>(ValuePtr);
            Out->SetStringField(TEXT("value"), Selector->SelectedKeyName.IsNone() ? FString() : Selector->SelectedKeyName.ToString());
            Out->SetBoolField(TEXT("blackboard_key"), true);
            TArray<FString> Allowed;
            for (const UBlackboardKeyType* Type : Selector->AllowedTypes)
            {
                if (Type) { Allowed.Add(Type->GetClass()->GetName()); }
            }
            Out->SetArrayField(TEXT("allowed_key_types"), mcp_common::StringsToJson(Allowed));
            return Out;
        }
        Out->SetField(TEXT("value"), mcp_common::PropertyValueToJson(Property, ValuePtr));
        if (StructProperty && IsValueOrBBKeyStruct(StructProperty->Struct))
        {
            Out->SetBoolField(TEXT("value_or_blackboard_key"), true);
        }
        return Out;
    }

    /** The blackboard entry KeyName names in Blackboard or one of its parents, or null. */
    const FBlackboardEntry* FindBlackboardEntry(const UBlackboardData* Blackboard, const FName KeyName)
    {
        for (const UBlackboardData* It = Blackboard; It; It = It->Parent)
        {
            for (const FBlackboardEntry& Entry : It->Keys)
            {
                if (Entry.EntryName == KeyName)
                {
                    return &Entry;
                }
            }
        }
        return nullptr;
    }
}

// MCP:get_behavior_tree_node_properties - Read the settings of one node of a BehaviorTree asset -- a composite, a task, a decorator or a service, found by its node name anywhere in the tree -- as typed JSON: every editable property of its class (name, type, category, value), a blackboard key selector read as its key name with the key types it accepts (allowed_key_types), a value-or-key pair (a Wait's WaitTime) as {Key, DefaultValue}. property_name narrows it to one. Read-only. It reads what set_behavior_tree_node_property writes; the tree's shape and node names come from get_behavior_tree_info. A missing tree or node is an error listing the nodes; a name several nodes share is an error naming each, to narrow with parent_name.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param node_name - Name of the node: the child_name, decorator_name or service_name given when it was added; "Root" for the root composite.
//   @param parent_name - Optional name of the composite (or, for a task's service, the task) holding the node, to pick one of several nodes with the same name; "Root" for the root.
//   @param property_name - Optional: read only this property (a dotted path into a struct is accepted, e.g. "WaitTime.DefaultValue").
//   @returns { behavior_tree_path, node_name, node_class, role: "root"|"composite"|"task"|"decorator"|"service"|"root_decorator", parent_name, blackboard_asset, property_count, properties: [{name, type, category, value, blackboard_key?, allowed_key_types?, value_or_blackboard_key?}] }
TSharedPtr<FJsonObject> HandleGetBehaviorTreeNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath, NodeName, ParentName, PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("behavior_tree_path"), BTPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_name"), NodeName)) { return Missing; }
    Params->TryGetStringField(TEXT("parent_name"), ParentName);
    Params->TryGetStringField(TEXT("property_name"), PropertyName);

    UBehaviorTree* BTAsset = nullptr;
    FBTNodeHit Hit;
    if (TSharedPtr<FJsonObject> Error = BehaviorTree_FindNamedNode(BTPath, NodeName, ParentName, BTAsset, Hit)) { return Error; }

    TArray<TSharedPtr<FJsonValue>> Properties;
    if (!PropertyName.IsEmpty())
    {
        FProperty* Leaf = nullptr;
        void* LeafPtr = nullptr;
        FString PathError;
        if (!mcp_common::ResolvePropertyPath(Hit.Node->GetClass(), Hit.Node, PropertyName, Leaf, LeafPtr, PathError))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Property '%s' not found on %s: %s"), *PropertyName, *Hit.Node->GetClass()->GetName(), *PathError));
        }
        if (PropertyName.Contains(TEXT(".")) || PropertyName.Contains(TEXT("[")))
        {
            TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();
            One->SetStringField(TEXT("name"), PropertyName);
            One->SetStringField(TEXT("type"), Leaf->GetCPPType());
            One->SetField(TEXT("value"), mcp_common::PropertyValueToJson(Leaf, LeafPtr));
            Properties.Add(MakeShared<FJsonValueObject>(One));
        }
        else
        {
            Properties.Add(MakeShared<FJsonValueObject>(BTPropertyJson(Leaf, Hit.Node)));
        }
    }
    else
    {
        for (TFieldIterator<FProperty> It(Hit.Node->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Edit) && Property->GetOwnerClass() != UObject::StaticClass())
            {
                Properties.Add(MakeShared<FJsonValueObject>(BTPropertyJson(Property, Hit.Node)));
            }
        }
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("behavior_tree_path"), BTAsset->GetPathName());
    Reply->SetStringField(TEXT("node_name"), Hit.Node->GetNodeName());
    Reply->SetStringField(TEXT("node_class"), Hit.Node->GetClass()->GetName());
    Reply->SetStringField(TEXT("role"), Hit.Role);
    Reply->SetStringField(TEXT("parent_name"), Hit.ParentName);
    Reply->SetStringField(TEXT("blackboard_asset"), BTAsset->BlackboardAsset ? BTAsset->BlackboardAsset->GetPathName() : FString());
    Reply->SetNumberField(TEXT("property_count"), Properties.Num());
    Reply->SetArrayField(TEXT("properties"), Properties);
    return mcp_common::MakeOkReply(Reply);
}

// MCP:set_behavior_tree_node_property - Write one setting of one node of a BehaviorTree asset -- a composite, task, decorator or service found by its node name -- then save the tree; no undo step. This is how a placed node is told what to do: a MoveTo's BlackboardKey, a Wait's WaitTime, a Blackboard decorator's key and condition. property_name is an editable property of the node's class or a dotted path into it. A blackboard key selector takes the key's NAME as a string: the key must exist in the tree's blackboard (or a parent) and be a type the selector accepts, else an error naming the keys; a value-or-key pair (WaitTime) takes a plain value for its DefaultValue or {"key": "<blackboard key>"}, a key of a type the pair accepts (a float pair takes a Float key); anything else takes typed JSON (numbers, bools, enum names, structs as objects). A property the class does not have, one that is not editable, or a value that does not fit is an error and nothing is written. The reply re-reads the property (value_after). Read: get_behavior_tree_node_properties; place nodes: add_behavior_tree_child / add_behavior_tree_decorator / add_behavior_tree_service.
//   @param behavior_tree_path - Path to the UBehaviorTree asset.
//   @param node_name - Name of the node (its child_name, decorator_name or service_name); "Root" for the root composite.
//   @param property_name - Editable property of the node's class (e.g. "BlackboardKey", "WaitTime", "AcceptableRadius") or a dotted path into one ("WaitTime.DefaultValue").
//   @param value - The value: a key name string for a blackboard key selector; a plain value or {"key": name} for a value-or-key pair; typed JSON otherwise.
//   @param parent_name - Optional name of the composite (or task) holding the node, to pick one of several nodes with the same name.
//   @returns { behavior_tree_path, node_name, node_class, property_name, value_after, key_type?, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetBehaviorTreeNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString BTPath, NodeName, PropertyName, ParentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("behavior_tree_path"), BTPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_name"), NodeName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }
    // A guard on the field itself, so the catalog publishes value as required (of any JSON type).
    if (!Params->HasField(TEXT("value")))
    {
        return mcp_common::MissingParameterReply(TEXT("value"));
    }
    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    Params->TryGetStringField(TEXT("parent_name"), ParentName);

    UBehaviorTree* BTAsset = nullptr;
    FBTNodeHit Hit;
    if (TSharedPtr<FJsonObject> Error = BehaviorTree_FindNamedNode(BTPath, NodeName, ParentName, BTAsset, Hit)) { return Error; }
    UBTNode* Node = Hit.Node;

    // The root property of the path has to be one the Details panel edits.
    int32 Cut = INDEX_NONE;
    FString RootName = PropertyName;
    if (RootName.FindChar(TEXT('.'), Cut)) { RootName.LeftInline(Cut); }
    if (RootName.FindChar(TEXT('['), Cut)) { RootName.LeftInline(Cut); }
    FProperty* RootProperty = nullptr;
    for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
    {
        if (It->GetName().Equals(RootName, ESearchCase::IgnoreCase))
        {
            RootProperty = *It;
            break;
        }
    }
    if (!RootProperty || !RootProperty->HasAnyPropertyFlags(CPF_Edit))
    {
        TArray<FString> Editable;
        for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_Edit) && It->GetOwnerClass() != UObject::StaticClass())
            {
                Editable.Add(It->GetName());
            }
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s has no editable property '%s' (its editable properties: %s); nothing was written"),
            *Node->GetClass()->GetName(), *RootName, *FString::Join(Editable, TEXT(", "))));
    }

    FProperty* Leaf = nullptr;
    void* LeafPtr = nullptr;
    FString PathError;
    if (!mcp_common::ResolvePropertyPath(Node->GetClass(), Node, PropertyName, Leaf, LeafPtr, PathError))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Property path '%s' does not resolve on %s: %s; nothing was written"),
            *PropertyName, *Node->GetClass()->GetName(), *PathError));
    }

    // A blackboard key, by name, checked against the tree's blackboard and the selector's allowed types.
    const UBlackboardData* Blackboard = BTAsset->BlackboardAsset;
    const auto CheckKey = [Blackboard](const FName KeyName, const TArray<TObjectPtr<UBlackboardKeyType>>* AllowedTypes, FString& OutError) -> const FBlackboardEntry*
    {
        if (!Blackboard)
        {
            OutError = TEXT("the tree has no blackboard asset (create_behavior_tree blackboard_path), so no key can be selected");
            return nullptr;
        }
        const FBlackboardEntry* Entry = FindBlackboardEntry(Blackboard, KeyName);
        if (!Entry || !Entry->KeyType)
        {
            TArray<FString> Keys;
            for (const UBlackboardData* It = Blackboard; It; It = It->Parent)
            {
                for (const FBlackboardEntry& Key : It->Keys) { Keys.Add(Key.EntryName.ToString()); }
            }
            OutError = FString::Printf(TEXT("blackboard %s has no key '%s' (its keys: %s)"), *Blackboard->GetName(), *KeyName.ToString(), *FString::Join(Keys, TEXT(", ")));
            return nullptr;
        }
        if (AllowedTypes && AllowedTypes->Num() > 0)
        {
            bool bAllowed = false;
            TArray<FString> AllowedNames;
            for (const TObjectPtr<UBlackboardKeyType>& Allowed : *AllowedTypes)
            {
                if (!Allowed) { continue; }
                AllowedNames.Add(Allowed->GetClass()->GetName());
                bAllowed |= Entry->KeyType->IsAllowedByFilter(Allowed);
            }
            if (!bAllowed)
            {
                OutError = FString::Printf(TEXT("key '%s' is a %s, which this selector does not accept (it takes: %s)"),
                    *KeyName.ToString(), *Entry->KeyType->GetClass()->GetName(), *FString::Join(AllowedNames, TEXT(", ")));
                return nullptr;
            }
        }
        return Entry;
    };

    FString KeyTypeOut;
    FString WriteError;
    const FStructProperty* LeafStruct = CastField<FStructProperty>(Leaf);
    if (LeafStruct && LeafStruct->Struct == FBlackboardKeySelector::StaticStruct())
    {
        FString KeyName;
        if (!Value->TryGetString(KeyName))
        {
            return mcp_common::MakeErrorReply(TEXT("A blackboard key selector takes the key's name as a string; nothing was written"));
        }
        FBlackboardKeySelector* Selector = static_cast<FBlackboardKeySelector*>(LeafPtr);
        const FBlackboardEntry* Entry = CheckKey(FName(*KeyName), &Selector->AllowedTypes, WriteError);
        if (!Entry)
        {
            return mcp_common::MakeErrorReply(WriteError + TEXT("; nothing was written"));
        }
        BTAsset->Modify();
        Node->Modify();
        Selector->SelectedKeyName = Entry->EntryName;
        Selector->ResolveSelectedKey(*Blackboard);
        KeyTypeOut = Selector->SelectedKeyType ? Selector->SelectedKeyType->GetName() : FString();
    }
    else if (LeafStruct && IsValueOrBBKeyStruct(LeafStruct->Struct) && Value->Type == EJson::Object
             && (Value->AsObject()->HasField(TEXT("key")) || Value->AsObject()->HasField(TEXT("Key"))))
    {
        FString KeyName;
        if (!Value->AsObject()->TryGetStringField(TEXT("key"), KeyName))
        {
            Value->AsObject()->TryGetStringField(TEXT("Key"), KeyName);
        }
        const FBlackboardEntry* KeyEntry = nullptr;
        if (!KeyName.IsEmpty())
        {
            KeyEntry = CheckKey(FName(*KeyName), nullptr, WriteError);
            if (!KeyEntry)
            {
                return mcp_common::MakeErrorReply(WriteError + TEXT("; nothing was written"));
            }
            // The pair's own type rule (FValueOrBBKey_Float takes a float key, ...), the one its Details panel filters by.
            if (LeafStruct->Struct->IsChildOf(FValueOrBlackboardKeyBase::StaticStruct())
                && !static_cast<const FValueOrBlackboardKeyBase*>(LeafPtr)->IsCompatibleType(KeyEntry->KeyType))
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("key '%s' is a %s, which this %s does not accept; nothing was written"),
                    *KeyName, *KeyEntry->KeyType->GetClass()->GetName(), *LeafStruct->Struct->GetName()));
            }
            KeyTypeOut = KeyEntry->KeyType->GetClass()->GetName();
        }
        FNameProperty* KeyProperty = CastField<FNameProperty>(LeafStruct->Struct->FindPropertyByName(TEXT("Key")));
        if (!KeyProperty)
        {
            return mcp_common::MakeErrorReply(TEXT("The value-or-key struct has no FName Key; nothing was written"));
        }
        BTAsset->Modify();
        Node->Modify();
        KeyProperty->SetPropertyValue_InContainer(LeafPtr, KeyName.IsEmpty() ? NAME_None : FName(*KeyName));
    }
    else
    {
        // A value-or-key pair given a plain value writes its DefaultValue.
        FProperty* Target = Leaf;
        void* TargetPtr = LeafPtr;
        FString TargetPath = PropertyName;
        if (LeafStruct && IsValueOrBBKeyStruct(LeafStruct->Struct) && Value->Type != EJson::Object)
        {
            Target = LeafStruct->Struct->FindPropertyByName(TEXT("DefaultValue"));
            TargetPtr = Target->ContainerPtrToValuePtr<void>(LeafPtr);
            TargetPath += TEXT(".DefaultValue");
        }
        // Applied to a scratch copy first, so a value that does not fit writes nothing.
        void* Scratch = Target->AllocateAndInitializeValue();
        Target->CopyCompleteValue(Scratch, TargetPtr);
        mcp_common::FJsonApplyReport Report;
        const bool bApplied = mcp_common::ApplyJsonToProperty(Target, Scratch, Value, TargetPath, Report);
        if (!bApplied || Report.HasFailures())
        {
            Target->DestroyAndFreeValue(Scratch);
            TArray<FString> Reasons;
            for (const TPair<FString, FString>& Failure : Report.Errors) { Reasons.Add(Failure.Key + TEXT(": ") + Failure.Value); }
            for (const FString& Ignored : Report.Ignored) { Reasons.Add(Ignored + TEXT(": no such field")); }
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("The value does not fit %s (%s)%s; nothing was written"),
                *TargetPath, *Target->GetCPPType(), Reasons.Num() ? *FString::Printf(TEXT(": %s"), *FString::Join(Reasons, TEXT("; "))) : TEXT("")));
        }
        BTAsset->Modify();
        Node->Modify();
        Target->CopyCompleteValue(TargetPtr, Scratch);
        Target->DestroyAndFreeValue(Scratch);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(BTAsset->GetOutermost(), BTAsset, SaveError);

    // The re-read: the property's value on the node now.
    FProperty* ReadLeaf = nullptr;
    void* ReadPtr = nullptr;
    FString ReadError;
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("behavior_tree_path"), BTAsset->GetPathName());
    Reply->SetStringField(TEXT("node_name"), Node->GetNodeName());
    Reply->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    Reply->SetStringField(TEXT("property_name"), PropertyName);
    if (mcp_common::ResolvePropertyPath(Node->GetClass(), Node, PropertyName, ReadLeaf, ReadPtr, ReadError))
    {
        const FStructProperty* ReadStruct = CastField<FStructProperty>(ReadLeaf);
        if (ReadStruct && ReadStruct->Struct == FBlackboardKeySelector::StaticStruct())
        {
            Reply->SetStringField(TEXT("value_after"), static_cast<const FBlackboardKeySelector*>(ReadPtr)->SelectedKeyName.ToString());
        }
        else
        {
            Reply->SetField(TEXT("value_after"), mcp_common::PropertyValueToJson(ReadLeaf, ReadPtr));
        }
    }
    if (!KeyTypeOut.IsEmpty())
    {
        Reply->SetStringField(TEXT("key_type"), KeyTypeOut);
    }
    return mcp_common::MakeSavedReply(Reply, bSaved, TEXT("set_behavior_tree_node_property"), SaveError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_behavior_tree_node_properties"), &HandleGetBehaviorTreeNodeProperties },
    { TEXT("set_behavior_tree_node_property"), &HandleSetBehaviorTreeNodeProperty },
    { TEXT("add_blackboard_key"), &HandleAddBlackboardKey },
    { TEXT("add_behavior_tree_child"), &HandleAddBehaviorTreeChild },
    { TEXT("add_behavior_tree_decorator"), &HandleAddBTDecorator },
    { TEXT("add_behavior_tree_service"), &HandleAddBTService },
    { TEXT("create_behavior_tree"), &HandleCreateBehaviorTree },
    { TEXT("create_blackboard_data"), &HandleCreateBlackboardData },
    { TEXT("get_behavior_tree_info"), &HandleGetBehaviorTreeInfo },
    { TEXT("get_behavior_tree_status"), &HandleGetBehaviorTreeStatus },
    { TEXT("get_blackboard_data"), &HandleGetBlackboardData },
    { TEXT("get_blackboard_value"), &HandleGetBlackboardValue },
    { TEXT("pause_behavior_tree"), &HandlePauseBehaviorTree },
    { TEXT("remove_blackboard_key"), &HandleRemoveBlackboardKey },
    { TEXT("remove_behavior_tree_child"), &HandleRemoveBehaviorTreeChild },
    { TEXT("remove_behavior_tree_decorator"), &HandleRemoveBTDecorator },
    { TEXT("remove_behavior_tree_service"), &HandleRemoveBTService },
    { TEXT("restart_behavior_tree"), &HandleRestartBehaviorTree },
    { TEXT("resume_behavior_tree"), &HandleResumeBehaviorTree },
    { TEXT("run_behavior_tree"), &HandleRunBehaviorTree },
    { TEXT("set_behavior_tree_root"), &HandleSetBehaviorTreeRoot },
    { TEXT("set_blackboard_key_subsettings"), &HandleSetBlackboardKeySubsettings },
    { TEXT("set_blackboard_value"), &HandleSetBlackboardValue },
    { TEXT("stop_behavior_tree"), &HandleStopBehaviorTree },
};
} // namespace UEAgentBehaviorTreeCommands
