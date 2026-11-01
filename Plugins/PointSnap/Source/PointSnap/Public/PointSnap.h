#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

POINTSNAP_API DECLARE_LOG_CATEGORY_EXTERN(LogPointSnap, Log, All);

class FPointSnapModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
