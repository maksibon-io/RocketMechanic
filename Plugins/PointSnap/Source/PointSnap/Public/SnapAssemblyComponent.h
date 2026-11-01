#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SnapAssemblyComponent.generated.h"

class USnapSocketComponent;
class AActor;

UENUM(BlueprintType)
enum class EAssemblyValidationMode : uint8
{
	AllSocketsOccupied   UMETA(DisplayName="All Sockets Occupied"),
	RequiredPartTags     UMETA(DisplayName="Required Part Tags"),
	RequiredPartClasses  UMETA(DisplayName="Required Part Classes"),
	Combined             UMETA(DisplayName="Combined (All Specified)")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAssemblyCompletedSignature, AActor*, AssemblyRoot, const TArray<AActor*>&, AssembledParts);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnAssemblyBrokenSignature, AActor*, AssemblyRoot, AActor*, RemovedPart);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnAssemblyPartAddedSignature, AActor*, AssemblyRoot, AActor*, AddedPart, int32, CurrentPartCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnAssemblyPartRemovedSignature, AActor*, AssemblyRoot, AActor*, RemovedPart, int32, CurrentPartCount);

/**
 * USnapAssemblyComponent
 * Tracks, validates, and manages complex multi-part assemblies.
 * Can be attached to the root, chassis, or any component of a multi-part structure.
 * Automatically checks when parts are attached or removed, and broadcasts
 * OnAssemblyCompleted when all criteria are met, and OnAssemblyBroken if any part is detached.
 */
UCLASS(ClassGroup=(PointSnap), meta=(BlueprintSpawnableComponent))
class POINTSNAP_API USnapAssemblyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USnapAssemblyComponent();

	/** Validation criteria mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Assembly")
	EAssemblyValidationMode ValidationMode = EAssemblyValidationMode::Combined;

	/** If true, every snap socket across all connected parts must be occupied for assembly to be complete */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Assembly")
	bool bRequireAllSocketsOccupied = true;

	/** Minimum number of total parts (including root) required to qualify as complete (0 = disabled) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Assembly", meta=(ClampMin="0"))
	int32 MinTotalParts = 0;

	/** List of Actor Tags that must all be present in the connected assembly */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Assembly")
	TArray<FName> RequiredPartTags;

	/** List of Actor Classes that must all be present in the connected assembly */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Assembly")
	TArray<TSubclassOf<AActor>> RequiredPartClasses;

	/** Whether the assembly is currently complete */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|Assembly")
	bool bIsAssemblyComplete = false;

	/** Event triggered when all assembly requirements are satisfied */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Assembly|Events")
	FOnAssemblyCompletedSignature OnAssemblyCompleted;

	/** Event triggered when an assembled structure loses a piece and is no longer complete */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Assembly|Events")
	FOnAssemblyBrokenSignature OnAssemblyBroken;

	/** Event triggered whenever a new part is snapped into this assembly */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Assembly|Events")
	FOnAssemblyPartAddedSignature OnAssemblyPartAdded;

	/** Event triggered whenever a part is detached from this assembly */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Assembly|Events")
	FOnAssemblyPartRemovedSignature OnAssemblyPartRemoved;

public:
	virtual void BeginPlay() override;

	/** Re-evaluates assembly completeness. Broadcasts events if completion status changes. */
	UFUNCTION(BlueprintCallable, Category="PointSnap|Assembly")
	bool CheckAssembly();

	/** Returns the highest root actor of this connected hierarchy */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	AActor* GetAssemblyRoot() const;

	/** Returns all actors connected in this assembly hierarchy that have Snap components */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	TArray<AActor*> GetAllConnectedParts() const;

	/** Returns the count of connected parts */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	int32 GetConnectedPartCount() const;

	/** Returns tags from RequiredPartTags that are currently missing in the assembly */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	TArray<FName> GetMissingTags() const;

	/** Returns classes from RequiredPartClasses that are currently missing in the assembly */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	TArray<TSubclassOf<AActor>> GetMissingClasses() const;

	/** Returns all unoccupied sockets across the connected assembly */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	TArray<USnapSocketComponent*> GetUnoccupiedSockets() const;

	/** Internal notification called when a part docks or detaches */
	void NotifyAssemblyChanged(AActor* ChangedPart, bool bAttached);
};
