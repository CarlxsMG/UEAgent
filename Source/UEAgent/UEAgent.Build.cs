// Copyright (c) 2026 CarlxsMG.

using UnrealBuildTool;

public class UEAgent : ModuleRules
{
	public UEAgent(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Unity build: same-named helpers in anonymous namespaces of different files collide inside
		// one unity blob, so a file-private helper gets a name unique to its file or lives in a
		// namespace named after the file. A -DisableAdaptiveUnity build (full unity, as a clean
		// build is) is the check.
		bUseUnity = true;

		// No gameplay runtime module: the plugin is primitives-only (an agent builds its own
		// gameplay systems on top of the engine, not from a pre-baked component set).

		// Runtime-side modules: the engine core, the transports (sockets, HTTP, JSON), and every
		// runtime system a command family reads or authors.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core", "CoreUObject", "Engine",
				"Networking", "Sockets", "HTTP",          // the MCP HTTP server
				"Json", "JsonUtilities",                   // every request and response
				"DeveloperSettings",                       // the plugin's project and editor settings
				"InputCore", "PhysicsCore",
				"UMG", "Slate", "SlateCore",               // widgets, and the editor UI built on Slate
				"NavigationSystem",  // navmesh queries, nav modifiers and links
				"AIModule",          // BehaviorTree / Blackboard / AAIController (BehaviorTree commands)
				"GameplayTasks",     // Required transitively by AIModule (AAIController IGameplayTaskOwnerInterface)
				"GameplayTags",      // FGameplayTag / FGameplayTagContainer and the tag manager
				"GameplayAbilities", // GameplayAbilities plugin for Ability System (public headers)
				"EnhancedInput",     // Enhanced Input assets (UInputAction / UInputMappingContext)
				"ImageWrapper",      // PNG encoding for viewport capture
				"ImageCore",         // REQUIRED by render_widget_preview (FImage::CopyTo is IMAGECORE_API) --
				                     // FWidgetRenderer::CreateTargetFor asks for the format Slate recommends
				                     // (WidgetRenderer.cpp), which is NOT always BGRA8, and FImage::AsBGRA8()
				                     // has a check() that crashes the editor instead of failing.
				                     // Normalizing the format before touching pixels is the only safe way.
				"RenderCore",        // viewport pixel readback support
				"RHI",               // FReadSurfaceDataFlags (readback surface flags)
				"MovieSceneTracks",  // Sequencer authoring: UMovieScene3DTransformTrack/CameraCutTrack/Sections
				"CinematicCamera",   // Sequencer authoring: ACineCameraActor
				"GameplayCameras",   // Gameplay Camera System (Experimental): UCameraRigAsset/UCameraAsset/USingleCameraDirector/UGameplayCameraComponent
				"Niagara",           // Niagara VFX: UNiagaraSystem/Component/FunctionLibrary
				"NiagaraCore",       // UNiagaraMergeable::GetMergeId (simulation stages; NIAGARACORE_API)
				"Landscape",         // Landscape: ALandscape::Import / ULandscapeInfo
				"Foliage",           // Foliage: AInstancedFoliageActor / UFoliageType_InstancedStaticMesh
				"MediaAssets",       // Media: UMediaPlayer/UMediaTexture/UFileMediaSource
				"PCG",               // PCG: UPCGGraph/UPCGComponent/APCGVolume
				"GeometryFramework", // ADynamicMeshActor/UDynamicMeshComponent (Runtime module)
				"GeometryScriptingCore", // UGeometryScriptLibrary_* (GeometryScripting plugin module)
				"GeometryCore",      // FDynamicMesh3 via UDynamicMesh::ProcessMesh (FGeometryScriptMeshSelection::ConvertToMeshIndexArray). Public dep of GeometryFramework already; declared explicitly per IWYU.
				"ControlRig",        // URigHierarchyController / UControlRig (ControlRig plugin runtime)
				"AnimationCore",     // FEulerTransform::Identity used by FRigControlValue
				"IKRig",             // UIKRigDefinition / UIKRetargeter (IKRig plugin runtime)
				"PBIK",              // EPBIKRootBehavior / EPBIKLimitType StaticEnum (FBIK solver settings; FullBodyIK plugin)
				"GeometryCollectionEngine", // UGeometryCollection (Chaos Destruction)
				"Chaos",             // FGeometryCollection core (Chaos Destruction)
				"DataRegistry",      // UDataRegistry/Source/Subsystem (DataRegistry plugin runtime)
				"FieldSystemEngine", // UFieldSystem / AFieldSystemActor / field nodes (Chaos Fields)
				"AudioModulation",   // USoundControlBus/USoundControlBusMix/USoundModulationGeneratorLFO
				"AudioExtensions",   // USoundModulatorBase / IAudioModulation
				"SmartObjectsModule",// USmartObjectDefinition/Component/Subsystem
				"StateTreeModule",   // UStateTree/UStateTreeSchema/FStateTreeStateLink
				"GameplayStateTreeModule", // UStateTreeComponent + FStateTreeReference (UStateTreeComponent::SetPropertyOverridden for set_state_tree_component_parameter_override)
				"MassEntity",        // UMassEntityConfigAsset/FMassEntityTemplate
				"MassCommon",        // UMassEntityTraitBase + common fragments
				"MassSpawner",       // AMassSpawner + FMassEntitySpawnRequest
				"MassSignals",       // UMassSignalSubsystem (SignalEntity/SignalEntities/NamedSignals)
				"GameplayBehaviorsModule", // UGameplayBehaviorConfig/Subsystem
				"Water",              // AWaterZone/AWaterBody/UWaterBodyComponent
				"ProceduralMeshComponent", // UProceduralMeshComponent/UKismetProceduralMeshLibrary
				"EngineSettings",     // UGameMapsSettings/UGeneralEngineSettings
				// Interchange framework
				// (NOT "Interchange" -- that's a .uplugin plugin name, not a module name; the Interchange.uplugin is enabled in UEAgent.uplugin's Plugins[])
				"InterchangeCore",                   // UInterchangeManager/UInterchangeAssetImportData/UInterchangeSourceData/UInterchangeBaseNodeContainer (InterchangeManager.h/804/855)
				"InterchangeEngine",                 // Engine module; auto-via Engine, but explicit for LNK
				"InterchangeImport",                 // UInterchangeTexture/StaticMesh/MaterialFactoryNode + translators (FBX/glTF/PSD/DDS/MaterialX)
				"InterchangePipelines",              // UInterchangeGenericAssetsPipeline/Texture/Mesh/Material/Animation/Audio/Groom/SparseVolumeTexturePipeline + MaterialXPipeline (concrete MinimalAPI classes)
				"InterchangeNodes",                  // UInterchangeBaseNode/SourceNode/FactoryBaseNode (graph primitives)
				"InterchangeFactoryNodes",           // UInterchangeTextureFactoryNode/StaticMeshFactoryNode/MaterialFactoryNode (per-asset node types)
				"InterchangeCommon",                 // Shared types
				"InterchangeCommonParser",           // Parser base
				"InterchangeMessages",               // Telemetry
				"InterchangeExport",                 // Writers
				"InterchangeFbxParser",              // FBX-specific parser (Editor-only at runtime; minimal runtime API)
				"GLTFCore",                          // Third-party Khronos glTF SDK
				// (NOT "InterchangeAssets" -- that's a .uplugin plugin name, not a module name; enabled via UEAgent.uplugin's Plugins[])
				// USD pipeline (stage, prims, attributes, layers, variants, stage actor)
				"UnrealUSDWrapper",                  // UE::FUsdStage/FUsdPrim/FSdfLayer/FSdfPath/FUsdAttribute/FUsdVariantSets (UsdWrappers/*.h; UNREALUSDWRAPPER_API; headless 100%)
				"USDUtilities",                      // USDErrorUtils.h (StartMonitoringErrors/GetErrorsAndStopMonitoring; USDUTILITIES_API) + USDAttributeUtils.h
				"USDStage",                           // AUsdStageActor/IUsdStageModule/EUsdStageState (USDSTAGE_API; honest-gated editor world)
				// PoseSearch / Chooser / MotionWarping / Subtitles plugin modules.
				"PoseSearch",                         // UPoseSearchDatabase/Schema/FeatureChannel/FSearchContext/Result (POSESEARCH_API)
				"Chooser",                            // UChooserTable/UChooserFunctionLibrary/EvaluateChooser + FInstancedStruct column types (CHOOSER_API)
				"MotionWarping",                      // UMotionWarpingComponent/FMotionWarpingTarget/UAnimNotifyState_MotionWarping (MOTIONWARPING_API)
				"SubtitlesAndClosedCaptions",         // USubtitleAssetUserData (re-exported from Engine via this plugin's public header) (SUBTITLESANDCLOSEDCAPTIONS_API)
				// UMG MVVM / CommonUI authoring, gameplay debugger, Game Features, Chaos vehicles.
				"ModelViewViewModel",                 // UMVVMViewModelBase / UMVVMBlueprintLibrary / FMVVMViewModelContext (MODELVIEWVIEWMODEL_API)
				"CommonUI",                           // UCommonUserWidget / UCommonActivatableWidget / UCommonActivatableWidgetStack (COMMONUI_API)
				"CommonInput",                        // UCommonInputSubsystem (COMMONINPUT_API, sub-plugin of CommonUI)
				"GameplayDebugger",                   // IGameplayDebugger/FGameplayDebuggerCategory (Engine Runtime module, gated by WITH_GAMEPLAY_DEBUGGER at use-site -- not a Plugins/ .uplugin, no uplugin change needed)
				"ChaosVehicles",                      // UChaosWheeledVehicleMovementComponent/FChaosWheelSetup (CHAOSVEHICLES_API; Runtime module of the ChaosVehiclesPlugin, enabled in UEAgent.uplugin)
			}
		);

		// Modules that exist only from UE 5.8 on. On 5.7 the Mass core types still live in MassEntity, and Interchange
		// has no separate analytics module.
		if (Target.Version.MajorVersion > 5 || Target.Version.MinorVersion >= 8)
		{
			PublicDependencyModuleNames.AddRange(new string[]
			{
				"MassCore",          // FMassEntityHandle/FMassEntityIndex/FMassEntitySubsystem core types (transitive of MassEntity)
				"InterchangeAnalytics",
			});
		}
		
		// The editor itself: this is an editor-only plugin (the bridge is a UEditorSubsystem).
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",            // asset lookups without loading
				"BlueprintGraph",           // K2 nodes
				"EditorScriptingUtilities", // UEditorAssetLibrary and friends
				"EditorSubsystem",          // UEditorSubsystem (the bridge)
				"Kismet",                   // FBlueprintEditorUtils, FKismetEditorUtilities
				"KismetCompiler",           // Blueprint compilation
				"Projects",                 // IPluginManager (plugin paths, enabled plugins)
				"UnrealEd",                 // GEditor and the editor framework
				"UEAgentUsdSdk"             // the USD values the engine's USD wrapper cannot convert (integer list ops, path expressions)
			}
		);

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"PropertyEditor",      // FPropertyEditorModule (details-panel property access)
					"ToolMenus",           // the "MCP" status-bar indicator and its menu (Mcp/McpEditorUI.cpp)
					"MainFrame",           // IMainFrameModule::RequestCloseEditor: "Restart now" to install an update (Mcp/McpUpdate.cpp)
					"FileUtilities",       // FZipArchiveReader: unpacking a downloaded update (Mcp/McpUpdate.cpp)
					"Settings",            // ISettingsModule::ShowViewer -- "Settings..." in the status-bar menu
					"BlueprintEditorLibrary", // UBlueprintEditorLibrary
					"LevelSequence",       // ULevelSequence and its runtime player
					"MovieScene",          // UMovieScene tracks, sections, bindings
					"LevelSequenceEditor", // sequence authoring helpers
					"MaterialEditor",      // UMaterialEditingLibrary
					"LevelEditor",         // ULevelEditorSubsystem (PIE start/stop, current level)
					"ApplicationCore",     // IPlatformInputDeviceMapper (inject_key_input / inject_analog_input / inject_mouse_button)
					"Localization",        // ULocalizationSettings/ULocalizationTarget (list/gather/compile_text_for_target)
					"AssetTools",          // For IAssetTools (import_texture_file, fixup_redirectors)
					"GameplayTagsEditor",  // For IGameplayTagsEditorModule (create_gameplay_tag -> AddNewGameplayTagToINI)
					"UMGEditor",           // UWidgetBlueprint / UWidgetBlueprintFactory
					"DataValidation",      // UEditorValidatorSubsystem
					"MessageLog",          // FMessageLogModule for run_map_check
					"NiagaraEditor",       // UNiagaraSystemFactoryNew::InitializeSystem
					"PythonScriptPlugin",  // IPythonScriptPlugin::ExecPythonCommandEx
					"Blutility",           // UEditorUtilityWidgetBlueprint / UEditorUtilitySubsystem
					"MetasoundEngine",     // UMetaSoundBuilderSubsystem / UMetaSoundSource
					"MetasoundFrontend",   // FMetasoundFrontendLiteral / document builder
					"MetasoundEditor",     // MetaSound editor asset wrapper
					"SourceControl",       // ISourceControlModule status
					"AnimGraph",           // UAnimBlueprint authoring / AnimGraph nodes
					"AnimGraphRuntime",    // AnimGraph runtime nodes
					"InputBlueprintNodes", // UK2Node_EnhancedInputAction (add_enhanced_input_action_node); EnhancedInput plugin UncookedOnly module
					"InputEditor",         // Public dep of InputBlueprintNodes (UInputAction editor types) -- keep the link island complete
					"PhysicsUtilities",    // FPhysicsAssetUtils::CreateFromSkeletalMesh (Developer module)
					"MeshUtilitiesEngine", // FMeshUtilitiesEngine::CalcBoneVertInfos -- the vertices add_physics_asset_body fits a body to (Developer module)
					"DataLayerEditor",     // UDataLayerEditorSubsystem assignment
					"CollectionManager",   // ICollectionManager collections (Developer module)
					"ControlRigDeveloper", // UControlRigBlueprint
					"ControlRigEditor",    // UControlRigBlueprintFactory::CreateNewControlRigAsset
					"MovieRenderPipelineCore",        // UMoviePipelineQueueSubsystem/Queue/Job/Config
					"MovieRenderPipelineEditor",      // queue subsystem editor
					"MovieRenderPipelineRenderPasses", // image-sequence output settings
					"RigVMDeveloper",      // URigVMController/Graph/Node/Pin/Link (RigVM solver graph; RIGVMDEVELOPER_API)
					"IKRigEditor",          // UIKRigController / UIKRetargeterController (IKRIGEDITOR_API)
					"StaticMeshEditor",     // UStaticMeshEditorSubsystem (STATICMESHEDITOR_API)
					"SkeletalMeshEditor",   // USkeletalMeshEditorSubsystem (SKELETALMESHEDITOR_API)
					"FractureEngine",       // FFractureEngineFracturing/Clustering (Chaos Destruction)
					"AnimationBlueprintLibrary", // UAnimationBlueprintLibrary (Animation Blendspace/Notify)
					"DataflowCore",            // FDataflowSelection (Chaos Destruction)
					"StateTreeEditorModule",   // UStateTreeEditorData / UStateTreeState (WITH_EDITOR)
					"PropertyBindingUtils",    // FPropertyBindingBindableStructDescriptor (FStateTreeCompilerLog dtor needs its export)
					"LandscapeEditor", // FLandscapeImportHelper + LandscapeEditorUtils (sculpt + heightmap I/O)
					"VirtualTexturingEditor", // RuntimeVirtualTexture::SetBounds -- the "Set Bounds" op for create_runtime_virtual_texture_volume/set_runtime_virtual_texture_volume_bounds (VIRTUALTEXTURINGEDITOR_API; Editor-only)
					// Interchange Editor (reset + asset save pipelines)
					"InterchangeEditor",                // UInterchangeEditorScriptLibrary (ResetLevelAsset/ResetActors/CanResetActor; INTERCHANGEEDITOR_API UFUNCTION static)
					"InterchangeEditorUtilities",       // UInterchangeEditorUtilities::SaveAsset/IsRuntimeOrPIE/ClearEditorSelection (INTERCHANGEEDITORUTILITIES_API)
					"InterchangeEditorPipelines",        // UInterchangePipelineConfigurationGeneric/CardsPipeline/GraphInspectorPipeline (UI; WITH_EDITOR)
					// USD exporter (USDStage/Exporter write-side; WITH_EDITOR)
					"USDExporter",                       // USDExporter.uplugin (USDConversionBlueprintLibrary::InsertSubLayer/AddReference; USDEXPORTER_API)
					// Editor-only factories for PoseSearch / Chooser / Subtitles.
					"PoseSearchEditor",                  // UPoseSearchDatabaseFactory / UPoseSearchSchemaFactory (POSESEARCHEDITOR_API)
					"ChooserEditor",                     // UChooserTableFactory / UAssetDefinition_ChooserTable (CHOOSEREDITOR_API)
					"SubtitlesAndClosedCaptionsEditor",  // USubtitleFactory (SUBTITLESANDCLOSEDCAPTIONSEDITOR_API)
// MVVM / CommonUI editor-only modules (UncookedOnly + Editor; auto-via ModelViewViewModel.uplugin / CommonUI.uplugin).
				"ModelViewViewModelBlueprint",        // UMVVMBlueprintView::AddViewModel / AddDefaultBinding via UWidgetBlueprintExtension (MODELVIEWVIEWMODELBLUEPRINT_API; UncookedOnly)
				"ModelViewViewModelEditor",           // UMVVMWidgetBlueprintExtension_View editor-side (MODELVIEWVIEWMODELEDITOR_API; Editor)
				"ModelViewViewModelDebugger",         // MVVMDebugging headers (UncookedOnly)
				"ModelViewViewModelDebuggerEditor",   // MVVMDebugging editor-side (Editor)
				"CommonUIEditor",                     // CommonUI editor extensions (COMMONUIEDITOR_API; Editor)
				// Functional Test authoring (PIE-based gameplay tests).
				"FunctionalTesting",                  // AFunctionalTest / UAutomationBlueprintFunctionLibrary (FUNCTIONALTESTING_API; Developer module, Editor + Non-Shipping)
				"FunctionalTestingEditor",            // FunctionalTest BP factory (FUNCTIONALTESTINGEDITOR_API; Editor)
					// (GCachedScalabilityCVars comes from Engine, already a public dependency.)
				}
			);
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Windows CNG: the SHA-256 of a downloaded update (Mcp/McpUpdate.cpp). The engine's
			// FPlatformMisc::GetSHA256Signature has no Windows implementation and asserts.
			PublicSystemLibraries.Add("bcrypt.lib");
		}
	}
}
