#pragma once
#include "CoreMinimal.h"
#include "PluginManifest.h"
#include "ProjectDescriptor.h"
#include "Commandlets/Commandlet.h"
#include "ProjectGeneratorCommandlet.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogProjectGeneratorCommandlet, All, All);

struct FCommandletRunParams {
	FString Params;
	FString ProjectName;
	FPluginManifest PluginManifest;
	FProjectDescriptor ProjectFile;
	FString GeneratedHeaderDir;
	FString OutputDirectory;
};

UCLASS()
class PROJECTGENERATOR_API UProjectGeneratorCommandlet : public UCommandlet {
	GENERATED_BODY()
public:
	UProjectGeneratorCommandlet();

	virtual int32 Main(const FString& Params) override;
private:
	int32 MainInternal(FCommandletRunParams& Params);

	static void GenerateEditorFile(FCommandletRunParams& Params, const FString& TargetFileName, const TSet<FString>& GameModuleNames, const FString&
	                               EditorType);
	static void DiscoverPlugins(const FString& PluginDirectory, TMap<FString, TSet<FString>>& OutPluginsFound);
	static void DiscoverModules(const FString& SourceDirectory, TSet<FString>& OutModulesFound);
	static FString GetIncludePathForObject(UObject* Object);
	static bool GetSpecialObjectIncludePath(UObject* Object, FString& OutIncludePath);
};