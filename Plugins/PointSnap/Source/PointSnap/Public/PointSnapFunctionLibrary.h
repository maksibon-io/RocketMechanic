#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SnapSocketComponent.h"
#include "SnapAssemblyComponent.h"
#include "PointSnapFunctionLibrary.generated.h"

UCLASS()
class POINTSNAP_API UPointSnapFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Calculates the docking actor transform given two snap sockets */
	UFUNCTION(BlueprintPure, Category="PointSnap|Math")
	static FTransform CalculateDockingTransform(const USnapSocketComponent* HeldSocket, const USnapSocketComponent* TargetSocket);

	/** Checks if two snap sockets are compatible and face each other */
	UFUNCTION(BlueprintPure, Category="PointSnap|Math")
	static bool AreSocketsCompatibleAndFacing(const USnapSocketComponent* SocketA, const USnapSocketComponent* SocketB);

	/** Returns all actors connected in this snapped assembly */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	static TArray<AActor*> GetAllConnectedAssemblyParts(AActor* AnyPartInAssembly);

	/** Finds the USnapAssemblyComponent in the assembly tree of the given actor */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	static USnapAssemblyComponent* GetAssemblyComponent(AActor* AnyPartInAssembly);
};
