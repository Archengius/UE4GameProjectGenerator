#include "ProjectGeneratorCommandlet.h"
#include "PluginManifest.h"
#include "ProjectDescriptor.h"
#include "Misc/OutputDeviceFile.h"
#include "UObject/MetaData.h"

DEFINE_LOG_CATEGORY(LogProjectGeneratorCommandlet);

#pragma optimize("", off)

UProjectGeneratorCommandlet::UProjectGeneratorCommandlet() {
	this->HelpDescription = TEXT("Generates a project source structure using the project file, plugin manifest and generated headers");
	this->HelpUsage = TEXT("ProjectGenerator -HeaderRoot=<HeaderRoot> -ProjectFile=<ProjectFile> -PluginManifest=<PluginManifest> -OutputDir=<OutputProjectDir>");
}

int32 UProjectGeneratorCommandlet::Main(const FString& Params) {
	UE_LOG(LogProjectGeneratorCommandlet, Display, TEXT("Parsing commandlet arguments"));
	
	FCommandletRunParams ResultParams{};
	ResultParams.Params = Params;

	{
		FString PluginManifestFile;
		if (!FParse::Value(*Params, TEXT("PluginManifest="), PluginManifestFile)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Missing plugin manifest. Usage: %s"), *HelpUsage);
			return 1;
		}
	
		FText ManifestLoadErrorText;
		if (!ResultParams.PluginManifest.Load(PluginManifestFile, ManifestLoadErrorText)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Cannot parse plugin manifest: %s"), *ManifestLoadErrorText.ToString());
			return 1;
		}
	}
	
	{
		FString ProjectFilePath;
		if (!FParse::Value(*Params, TEXT("ProjectFile="), ProjectFilePath)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Missing project file. Usage: %s"), *HelpUsage);
			return 1;
		}
		ResultParams.ProjectName = FPaths::GetBaseFilename(ProjectFilePath);
		
		FText ProjectFileErrorText;
		if (!ResultParams.ProjectFile.Load(ProjectFilePath, ProjectFileErrorText)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Cannot parse project file: %s"), *ProjectFileErrorText.ToString());
			return 1;
		}
	}

	{
		if (!FParse::Value(*Params, TEXT("HeaderRoot="), ResultParams.GeneratedHeaderDir)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Missing header root directory. Usage: %s"), *HelpUsage);
			return 1;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*ResultParams.GeneratedHeaderDir)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Provided header directory does not exist"));
			return 1;
		}
	}
	
	{
		if (!FParse::Value(*Params, TEXT("OutputDir="), ResultParams.OutputDirectory)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Missing output project dir. Usage: %s"), *HelpUsage);
			return 1;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*ResultParams.OutputDirectory)) {
			UE_LOG(LogProjectGeneratorCommandlet, Error, TEXT("Provided output directory does not exist"));
			return 1;
		}
	}
	
	return MainInternal(ResultParams);
}

int32 UProjectGeneratorCommandlet::MainInternal(FCommandletRunParams& Params) {
	UE_LOG(LogProjectGeneratorCommandlet, Display, TEXT("Collecting plugin module list"));

	//Collect registered plugins inside of the packaged game
	TMap<FString, FString> ModuleNameToOwnerPluginName;
	TMap<FString, FString> GameImpliedPluginFileLocations;
	TSet<FString> GameEnginePlugins;

	const FString ProjectSourceDir = Params.OutputDirectory / TEXT("Source");
	const FString GameProjectPluginDir = FString::Printf(TEXT("../../../%s/Plugins/"), *Params.ProjectName);
	const FString GameEnginePluginDir = TEXT("../../../Engine/Plugins/");

	for (const FPluginManifestEntry& ManifestEntry : Params.PluginManifest.Contents) {
		const FString PluginName = FPaths::GetBaseFilename(ManifestEntry.File);
	
		for (const FModuleDescriptor& ModuleInfo : ManifestEntry.Descriptor.Modules) {
			ModuleNameToOwnerPluginName.Add(ModuleInfo.Name.ToString(), PluginName);
		}

		const FString PluginFilename = ManifestEntry.File;

		if (PluginFilename.StartsWith(GameProjectPluginDir)) {
			const FString RelativeFileLocation = PluginFilename.Mid(GameProjectPluginDir.Len());
			const FString FullFilePath = Params.OutputDirectory / TEXT("Plugins") / RelativeFileLocation;
			
			GameImpliedPluginFileLocations.Add(PluginName, FullFilePath);

		} else if (PluginFilename.StartsWith(GameEnginePluginDir)) {
			const FString RelativeFileLocation = PluginFilename.Mid(GameEnginePluginDir.Len());
			const FString FullFilePath = Params.OutputDirectory / TEXT("Plugins/EnginePlugins") / RelativeFileLocation;
			
			GameImpliedPluginFileLocations.Add(PluginName, FullFilePath);
			GameEnginePlugins.Add(PluginName);
		} else {
			UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Found game plugin not located inside of the engine or project directories: %s"), *PluginFilename);
		}
	}

	//All game modules referenced through the project descriptor file
	TSet<FString> ProjectModuleNames;

	for (const FModuleDescriptor& ModuleDescriptor : Params.ProjectFile.Modules) {
		const FString ModuleName = ModuleDescriptor.Name.ToString();

		//Need to check whenever the module belongs to any of plugins first, DBD lists some of the plugin-contained modules explicitly inside of the project modules
		if (!ModuleNameToOwnerPluginName.Contains(ModuleName)) {
			ProjectModuleNames.Add(ModuleName);
		}
	}
	
	//Collect a list of all engine modules and plugins, so we can compare against what we already have
	TMap<FString, TSet<FString>> EnginePlugins;
	TSet<FString> EngineModules;
	
	DiscoverPlugins(FPaths::EnginePluginsDir(), EnginePlugins);
	DiscoverModules(FPaths::EngineSourceDir(), EngineModules);

	UE_LOG(LogProjectGeneratorCommandlet, Display, TEXT("Found %d engine plugins and %d engine modules"), EnginePlugins.Num(), EngineModules.Num());

	const FString CrossModuleIncludePrefix = TEXT("//CROSS-MODULE INCLUDE V2: ");
	TSet<FString> ModulesThatHaveTriedBeingLoaded;

	auto HandleModuleHeaderFile = [&](const FString& HeaderFileName, TArray<FString>& HeaderLines) {
		for (FString& HeaderString : HeaderLines) {
			if (HeaderString.StartsWith(CrossModuleIncludePrefix)) {

				const FString IncludeData = HeaderString.Mid(CrossModuleIncludePrefix.Len());

				FString IncludeModuleName;
				FString IncludeObjectName;
				FParse::Value(*IncludeData, TEXT("ModuleName="), IncludeModuleName);
				FParse::Value(*IncludeData, TEXT("ObjectName="), IncludeObjectName);

				if (IncludeModuleName.Len() == 0 || IncludeObjectName.Len() == 0) {
					UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Malformed cross module include string encoutered processing %s: %s"), *HeaderFileName, *HeaderString);
					continue;
				}

				FString FallbackHeaderName = IncludeObjectName;
				FParse::Value(*IncludeData, TEXT("FallbackName="), FallbackHeaderName);
				
				FModuleManager& ModuleManager = FModuleManager::Get();

				//Try loading the module if it has not been loaded already and we know for a fact that it exists inside of the engine
				//TODO this is surely a hack, we need a better solution involving using uproject file data
				if (EngineModules.Contains(IncludeModuleName) && !ModuleManager.IsModuleLoaded(*IncludeModuleName)) {
					if (!ModulesThatHaveTriedBeingLoaded.Contains(IncludeModuleName)) {
						ModulesThatHaveTriedBeingLoaded.Add(IncludeModuleName);

						IModuleInterface* LoadedModule = ModuleManager.LoadModule(*IncludeModuleName);
						if (LoadedModule != NULL) {
							ProcessNewlyLoadedUObjects();
							UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Force loaded engine module %s"), *IncludeModuleName);
						} else {
							UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Failed to load engine module %s required by the header file %s"), *IncludeModuleName, *HeaderFileName);
						}
					}
				}
				
				const FString ModulePackageName = FString::Printf(TEXT("/Script/%s"), *IncludeModuleName);
				UPackage* ModulePackage = FindPackage(NULL, *ModulePackageName);

				//If module package is not found, we assume it's one of the game modules, and generate a normal include
				if (ModulePackage == NULL) {
					HeaderString = FString::Printf(TEXT("#include \"%s.h\""), *FallbackHeaderName);
					continue;
				}

				//Module has been found, attempt to resolve the object reference now
				UObject* FoundModuleObject = FindObjectFast<UObject>(ModulePackage, *IncludeObjectName);

				//Print a warning if we couldn't find an object but module is there
				if (FoundModuleObject == NULL) {
					UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Couldn't find native object %s inside of the module %s package"), *IncludeObjectName, *IncludeModuleName);
					continue;
				}

				FString ObjectIncludePath;
				if (!GetSpecialObjectIncludePath(FoundModuleObject, ObjectIncludePath)) {
					ObjectIncludePath = GetIncludePathForObject(FoundModuleObject);
				}
				if (!ObjectIncludePath.IsEmpty()) {
					HeaderString = FString::Printf(TEXT("#include \"%s\""), *ObjectIncludePath);
				}
			}
		}
	};
	
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	auto MoveModuleFilesRecursive = [&](const FString& SourceDirectory, const FString& ResultDirectory, const TCHAR* Filename, bool bIsDirectory) {
		//Only interested in the loose files, we will make directories for them on the go
		if (bIsDirectory) {
			return true;
		}

		//Compute the new absolute path for the file in question
		const FString SourceDirectoryWithSlash = SourceDirectory / TEXT("");
		FString RelativePathToFile = Filename;
		FPaths::MakePathRelativeTo(RelativePathToFile, *SourceDirectoryWithSlash);
		
		const FString NewAbsoluteFilename = ResultDirectory / RelativePathToFile;
		const FString FileExtension = FPaths::GetExtension(NewAbsoluteFilename);

		//Make sure the directory containing the file exists
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(NewAbsoluteFilename));
		
		//If the file is a header or CPP file, we need to process it explicitly
		if (FileExtension == TEXT("h") || FileExtension == TEXT("cpp")) {
			TArray<FString> FileContentLines;
			FFileHelper::LoadFileToStringArray(FileContentLines, Filename);
			HandleModuleHeaderFile(Filename, FileContentLines);
			
			FFileHelper::SaveStringArrayToFile(FileContentLines, *NewAbsoluteFilename);
		} else {
			//Otherwise, copy the file normally
			PlatformFile.CopyFile(*NewAbsoluteFilename, Filename);
		}
		return true;
	};

	//Generate the modules for each folder inside of the headers root
	TSet<FString> AllGameModulesProcessed;
	TSet<FString> LooseGameModuleNames;
	TSet<FString> EngineModulesForcedToBeGameModules;

	int32 PluginModulesCopied = 0;
	int32 GameModulesCopied = 0;
	
	TFunction<bool(const TCHAR*, bool)> ModuleDirIterator = [&](const TCHAR* Filename, bool bIsDirectory) {
		//Only interested in actual module directories and not loose files
		if (!bIsDirectory) {
			return true;
		}
		
		const FString ModuleName = FPaths::GetCleanFilename(Filename);

		//This module is owned by one of the plugins
		if (const FString* OwnerPluginName = ModuleNameToOwnerPluginName.Find(ModuleName)) {

			//Check if it's one of the existing engine plugins, then we return early and discard the module
			if (const TSet<FString>* OwnerPluginModules = EnginePlugins.Find(*OwnerPluginName)) {

				//Print a warning when the module belongs to the plugin, but engine plugin does not have that module
				if (!OwnerPluginModules->Contains(ModuleName)) {
					UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Engine plugin %s does not have a module %s present in the game"), OwnerPluginName->operator*(), *ModuleName);
				}
				return true;
			}

			//Module does not belong to the any of the existing engine plugins
			if (const FString* ResultPluginFile = GameImpliedPluginFileLocations.Find(*OwnerPluginName)) {

				const FString PluginDir = FPaths::GetPath(*ResultPluginFile);
				const FString TargetModuleDirectory = PluginDir / TEXT("Source") / ModuleName;

				TFunction<bool(const TCHAR*, bool)> CopyModuleFiles = [&](const TCHAR* FilenameInner, bool bIsDirectoryInner) {
					return MoveModuleFilesRecursive(Filename, TargetModuleDirectory, FilenameInner, bIsDirectoryInner);
				};
				
				PlatformFile.IterateDirectoryRecursively(Filename, CopyModuleFiles);
				AllGameModulesProcessed.Add(ModuleName);
				PluginModulesCopied++;
			} else {
				//No registered game module associated with this plugin, print a warning
				UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Discarding game module %s because associated plugin %s does not exist"), *ModuleName, OwnerPluginName->operator*());
			}
			return true;
		}
		
		//If this is the normal engine module, we skip it altogether
		if (EngineModules.Contains(ModuleName)) {
			return true;
		}

		//Otherwise assume it is a normal game module. If it was not declared inside of the project file, output a warning
		if (!ProjectModuleNames.Contains(ModuleName)) {
			EngineModulesForcedToBeGameModules.Add(ModuleName);
			UE_LOG(LogProjectGeneratorCommandlet, Warning, TEXT("Module %s does not belong to the engine or any plugins, neither it is listed in the project modules. Assuming it is a game module"), *ModuleName);
		}
		const FString TargetModuleDirectory = ProjectSourceDir / ModuleName;

		TFunction<bool(const TCHAR*, bool)> CopyModuleFiles = [&](const TCHAR* FilenameInner, bool bIsDirectoryInner) {
			return MoveModuleFilesRecursive(Filename, TargetModuleDirectory, FilenameInner, bIsDirectoryInner);
		};

		//Copy the game module to the normal directory
		PlatformFile.IterateDirectoryRecursively(Filename, CopyModuleFiles);

		AllGameModulesProcessed.Add(ModuleName);
		LooseGameModuleNames.Add(ModuleName);
		GameModulesCopied++;
		
		return true;
	};

	//Now run the handler for each module we found in the header dump
	PlatformFile.IterateDirectory(*Params.GeneratedHeaderDir, ModuleDirIterator);
	UE_LOG(LogProjectGeneratorCommandlet, Display, TEXT("Handled %d plugin modules and %d game modules"), PluginModulesCopied, GameModulesCopied);

	TSet<FString> AllGamePluginsProcessed;
	
	//Carry over uplugin files and sanitize them to only include modules that we have carried over
	for (FPluginManifestEntry& ManifestEntry : Params.PluginManifest.Contents) {
		const FString PluginName = FPaths::GetBaseFilename(ManifestEntry.File);

		//Skip engine plugins or plugins for which we do not have the implied paths
		if (EnginePlugins.Contains(PluginName) || !GameImpliedPluginFileLocations.Contains(PluginName)) {
			continue;
		}

		const FString NewPluginFileLocation = GameImpliedPluginFileLocations.FindChecked(PluginName);
		FPluginDescriptor NewPluginDescriptor = ManifestEntry.Descriptor;

		//Cleanup any modules that we have not copied
		NewPluginDescriptor.Modules.RemoveAll([&](const FModuleDescriptor& ModuleDescriptor) {
			const FString ModuleName = ModuleDescriptor.Name.ToString();
			
			//Keep engine module references, even if we have not copied them
			return !EngineModules.Contains(ModuleName) &&
				!AllGameModulesProcessed.Contains(ModuleName);
		});

		//Cleanup any plugin dependencies that physically do not exist in the project
		NewPluginDescriptor.Plugins.RemoveAll([&](const FPluginReferenceDescriptor& PluginDescriptor) {
			const FString ReferencedPluginName = PluginDescriptor.Name;
			
			return !GameImpliedPluginFileLocations.Contains(ReferencedPluginName) &&
				!EnginePlugins.Contains(ReferencedPluginName);
		});

		//Make sure the underlying directory exists
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(NewPluginFileLocation));
		FText PluginFileSaveReason;
		check(NewPluginDescriptor.Save(NewPluginFileLocation, PluginFileSaveReason));

		AllGamePluginsProcessed.Add(PluginName);
	}

	//Sanitize the project file
	FProjectDescriptor NewProjectDescriptor = Params.ProjectFile;

	//Remove all of the modules that we have not copied
	NewProjectDescriptor.Modules.RemoveAll([&](const FModuleDescriptor& ModuleDescriptor) {
		const FString ModuleName = ModuleDescriptor.Name.ToString();
			
		//Keep engine module references, even if we have not copied them
		return !EngineModules.Contains(ModuleName) &&
			!AllGameModulesProcessed.Contains(ModuleName);
	});

	//Remove references to the plugins that we have not actually copied
	NewProjectDescriptor.Plugins.RemoveAll([&](FPluginReferenceDescriptor& PluginReference) {
		const FString PluginName = PluginReference.Name;

		//Strip out whitelisted platforms that we do not know about, Stadia in particular
		//TODO seems to be engine patch to support stadia target? Is it a backport from UE4.26?
		PluginReference.WhitelistPlatforms.Remove(TEXT("Stadia"));
		
		//Keep engine plugins references
		return !EnginePlugins.Contains(PluginName) &&
			!AllGamePluginsProcessed.Contains(PluginName);
	});

	//Force references to the engine modules that do not exist in the engine now
	for (const FString& ForcedEngineModule : EngineModulesForcedToBeGameModules) {
		FModuleDescriptor NewModuleDescriptor{};
		
		NewModuleDescriptor.Name = *ForcedEngineModule;
		NewModuleDescriptor.Type = EHostType::Runtime;
		NewModuleDescriptor.LoadingPhase = ELoadingPhase::Default;
		
		NewProjectDescriptor.Modules.Add(NewModuleDescriptor);
	}

	//Save the new project file at the destination path
	const FString ResultFilePath = Params.OutputDirectory / Params.ProjectName += TEXT(".uproject");
	FText ProjectFileSaveReason;
	check(NewProjectDescriptor.Save(ResultFilePath, ProjectFileSaveReason));

	//Generate the editor target file
	const FString TargetFileName = ProjectSourceDir / FString::Printf(TEXT("%sEditor.Target.cs"), *Params.ProjectName);
	GenerateEditorFile(Params, TargetFileName, LooseGameModuleNames, TEXT("Editor"));
	const FString GameFileName = ProjectSourceDir / FString::Printf(TEXT("%sGame.Target.cs"), *Params.ProjectName);
	GenerateEditorFile(Params, GameFileName, LooseGameModuleNames, TEXT("Game"));

	UE_LOG(LogProjectGeneratorCommandlet, Display, TEXT("Wrote project data to %s"), *Params.OutputDirectory);
	return 0;
}

void UProjectGeneratorCommandlet::GenerateEditorFile(FCommandletRunParams& Params, const FString& FileName, const TSet<FString>& GameModuleNames, const FString& EditorType) {

	FOutputDeviceFile TargetFileOutputDevice(*FileName, true, false);
	TargetFileOutputDevice.SetAutoEmitLineTerminator(true);
	TargetFileOutputDevice.SetSuppressEventTag(true);

	TargetFileOutputDevice.Logf(TEXT("using UnrealBuildTool;"));
	TargetFileOutputDevice.Logf(TEXT(""));
	TargetFileOutputDevice.Logf(TEXT("public class %s%sTarget : TargetRules {"), *Params.ProjectName, *EditorType);
	TargetFileOutputDevice.Logf(TEXT("	public %s%sTarget(TargetInfo Target) : base(Target) {"), *Params.ProjectName, *EditorType);

	TargetFileOutputDevice.Logf(TEXT("		Type = TargetType.%s;"), *EditorType);
	TargetFileOutputDevice.Logf(TEXT("		DefaultBuildSettings = BuildSettingsVersion.V2;"));

	TargetFileOutputDevice.Logf(TEXT("		ExtraModuleNames.AddRange(new string[] {"));
	for (const FString& GameModuleName : GameModuleNames) {
		TargetFileOutputDevice.Logf(TEXT("			\"%s\","), *GameModuleName);
	}
	TargetFileOutputDevice.Logf(TEXT("		});"));
	
	TargetFileOutputDevice.Logf(TEXT("	}"));
	TargetFileOutputDevice.Logf(TEXT("}"));

	TargetFileOutputDevice.Flush();
	TargetFileOutputDevice.TearDown();
}

void UProjectGeneratorCommandlet::DiscoverPlugins(const FString& PluginDirectory, TMap<FString, TSet<FString>>& OutPluginsFound) {
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	TFunction<bool(const TCHAR*, bool)> DirectoryIterator = [&](const TCHAR* Filename, bool bIsDirectory) {
		//Only interested in actual directories
		if (bIsDirectory) {
			//Check whenever the plugin file is present in the directory
			const FString DirectoryName = FPaths::GetBaseFilename(Filename);
			const FString PluginFilePath = FString(Filename) / DirectoryName += TEXT(".uplugin");

			//Plugin file has been found at that directory, record it and continue iteration
			if (PlatformFile.FileExists(*PluginFilePath)) {
				//Record modules that belong to the plugin
				TSet<FString> PluginModules;
				const FString PluginSourceDir = FString(Filename) / TEXT("Source");

				if (PlatformFile.DirectoryExists(*PluginSourceDir)) {
					DiscoverModules(PluginSourceDir, PluginModules);
				}
			
				OutPluginsFound.Add(DirectoryName, PluginModules);
				return true;
			}
			//Otherwise recursively iterate the directory, unless it's Saved
			if (DirectoryName != TEXT("Saved")) {
				PlatformFile.IterateDirectory(Filename, DirectoryIterator);
			}
		}
		return true;
	};
	PlatformFile.IterateDirectory(*PluginDirectory, DirectoryIterator);
}

void UProjectGeneratorCommandlet::DiscoverModules(const FString& SourceDirectory, TSet<FString>& OutModulesFound) {
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	TFunction<bool(const TCHAR*, bool)> DirectoryIterator = [&](const TCHAR* Filename, bool bIsDirectory) {
		//Only interested in actual directories
		if (bIsDirectory) {
			//Check whenever the module build file is present in the directory
			const FString DirectoryName = FPaths::GetBaseFilename(Filename);

			//Quit directory immediately if it's Saved or Intermediate
			if (DirectoryName == TEXT("Saved") || DirectoryName == TEXT("Intermediate")) {
				return true;
			}
			
			int32 ModulesFoundInDirectory = 0;
			TArray<FString> SubDirectoryPaths;

			PlatformFile.IterateDirectory(Filename, [&](const TCHAR* InnerFilename, bool bIsDirectoryInner) {
				const FString InnerFilenameString = FString(InnerFilename);
				if (bIsDirectoryInner) {
					SubDirectoryPaths.Add(InnerFilenameString);
					
				} else if (InnerFilenameString.EndsWith(TEXT(".Build.cs"))) {
					const FString BaseFilename = FPaths::GetBaseFilename(InnerFilenameString);
					const FString ModuleName = BaseFilename.Mid(0, BaseFilename.Len() - 6);
					
					OutModulesFound.Add(ModuleName);
					ModulesFoundInDirectory++;
				}
				return true;
			});

			//Module build file has been found at that directory, we do not consider any sub-directories
			if (ModulesFoundInDirectory != 0) {
				return true;
			}
			
			//Otherwise recursively iterate the subdirectories
			for (const FString& SubDirectoryName : SubDirectoryPaths) {
				DirectoryIterator(*SubDirectoryName, true);
			}
		}
		return true;
	};
	DirectoryIterator(*SourceDirectory, true);
}

FString UProjectGeneratorCommandlet::GetIncludePathForObject(UObject* Object) {
	//We cannot use "IncludePath" metadata attribute here because it's not added for UScriptStruct,
	//and object we have as argument can be either UClass or UScriptStruct, or maybe even UEnum if we decide to
	//support them at some point. However, "ModuleRelativePath" is present on all of these objects,
	//and even on function and property objects. According to UHT source, only difference
	//between these two is that include path has Public/Private/Classes prefixes stripped. We can
	//mimic that behavior and get uniform include paths for all defined objects

	UPackage* Package = Object->GetOutermost();
	UMetaData* MetaData = Package->GetMetaData();
	checkf(MetaData, TEXT("Metadata object is not found on the package %s"), *Package->GetName());

	FString IncludePath = MetaData->GetValue(Object, TEXT("ModuleRelativePath"));
	checkf(!IncludePath.IsEmpty(), TEXT("ModuleRelativePath metadata not found on object %s"), *Object->GetPathName());
	
	// Walk over the first potential slash
	if (IncludePath.StartsWith(TEXT("/"))) {
		IncludePath.RemoveAt(0);
	}
	// Does this module path start with a known include path location? If so, we can cut that part out of the include path
	static const TCHAR PublicFolderName[]  = TEXT("Public/");
	static const TCHAR PrivateFolderName[] = TEXT("Private/");
	static const TCHAR ClassesFolderName[] = TEXT("Classes/");
	if (IncludePath.StartsWith(PublicFolderName)) {
		IncludePath.RemoveAt(0, UE_ARRAY_COUNT(PublicFolderName) - 1);
	}
	if (IncludePath.StartsWith(PrivateFolderName)) {
		IncludePath.RemoveAt(0, UE_ARRAY_COUNT(PrivateFolderName) - 1);
	}
	if (IncludePath.StartsWith(ClassesFolderName)) {
		IncludePath.RemoveAt(0, UE_ARRAY_COUNT(ClassesFolderName) - 1);
	}
	return IncludePath;
}

const TMap<UObject*, FString>& GetSpecialObjectIncludePaths() {
	static TMap<UObject*, FString> ResultMap;

	if (!ResultMap.Num()) {
		ResultMap.Add(UObject::StaticClass(), TEXT("UObject/Object.h"));
		
		ResultMap.Add(UClass::StaticClass(), TEXT("UObject/Class.h"));
		ResultMap.Add(UScriptStruct::StaticClass(), TEXT("UObject/Class.h"));
		ResultMap.Add(UEnum::StaticClass(), TEXT("UObject/Class.h"));
		ResultMap.Add(UInterface::StaticClass(), TEXT("UObject/Interface.h"));
	}
	return ResultMap;
}

//Handles some special paths inside of the CoreUObject specifically
bool UProjectGeneratorCommandlet::GetSpecialObjectIncludePath(UObject* Object, FString& OutIncludePath) {
	const TMap<UObject*, FString>& BaseObjectMap = GetSpecialObjectIncludePaths();

	//Check the base object map for basic CoreUObject type definitions
	if (BaseObjectMap.Contains(Object)) {
		OutIncludePath = BaseObjectMap.FindChecked(Object);
		return true;
	}

	//Check if the object is class has NoExport flag, and then include the NoExportTypes.h
	if (const UClass* Class = Cast<UClass>(Object)) {
		if (Class->HasAnyClassFlags(CLASS_NoExport)) {
			OutIncludePath = TEXT("UObject/NoExportTypes.h");
			return true;
		}
	}
	if (const UScriptStruct* Struct = Cast<UScriptStruct>(Object)) {
		if ((Struct->StructFlags & STRUCT_NoExport) != 0) {
			OutIncludePath = TEXT("UObject/NoExportTypes.h");
			return true;
		}
	}
	
	//Otherwise, assume it is a normal object that has the correct include path
	return false;
}

#pragma optimize("", on)