#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SnapSocketComponent.h"
#include "SnapControllerComponent.generated.h"

class UPrimitiveComponent;
class UStaticMeshComponent;
class AStaticMeshActor;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class USoundBase;

struct FSnapCandidateSlot
{
	USnapSocketComponent* HeldSocket = nullptr;
	USnapSocketComponent* TargetSocket = nullptr;
	FTransform TargetTransform = FTransform::Identity;
	FTransform GhostTransform = FTransform::Identity;
	float Distance = 0.0f;
	float Score = 0.0f;
	bool bCanSnap = false;
};

UENUM(BlueprintType)
enum class ESnapRole : uint8
{
	AutoDetect         UMETA(DisplayName="Auto-Detect (Smart Structural Hierarchy)"),
	AlwaysBase         UMETA(DisplayName="Always Base (Host - Never Projects Ghost)"),
	AlwaysDockingPart  UMETA(DisplayName="Always Docking Part (Guest - Projects Ghost)")
};

UENUM(BlueprintType)
enum class EDualHandPriorityMode : uint8
{
	AssemblyComponentPriority UMETA(DisplayName="Assembly Component is Base (Recommended)"),
	LeftHandBase              UMETA(DisplayName="Always Left Hand is Base"),
	RightHandBase             UMETA(DisplayName="Always Right Hand is Base"),
	AssemblyRootPriority      UMETA(DisplayName="Most Assembled Parts is Base"),
	AdaptiveOrder             UMETA(DisplayName="Adaptive (First Grabbed is Base)")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSnapCandidateChangedSignature, bool, bHasCandidate, const FTransform&, CandidateTransform);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSnapCompletedSignature, USnapSocketComponent*, HeldSocket, USnapSocketComponent*, TargetSocket);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSnapDetachedSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSnapTensionChangedSignature, float, TensionAlpha);

/**
 * USnapControllerComponent
 * Adds modular snapping behavior to any Actor.
 * Automatically handles immediate ghost preview positioning, proximity scaling,
 * adaptive two-handed VR handling, grabbing assembly by attached parts,
 * smooth release interpolation, and notifies USnapAssemblyComponent.
 */
UCLASS(ClassGroup=(PointSnap), meta=(BlueprintSpawnableComponent))
class POINTSNAP_API USnapControllerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USnapControllerComponent();

	/** Time (in seconds) to smoothly glide into the docking socket upon release */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Animation", meta=(ClampMin="0.05", ClampMax="2.0"))
	float SnapDuration = 0.22f;

	/** Whether to automatically monitor BP_GrabComponent on the owner actor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Integration")
	bool bAutoDetectGrab = true;

	/** How to resolve Base vs Docking Part when holding two objects in hands simultaneously */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|TwoHanded")
	EDualHandPriorityMode DualHandPriority = EDualHandPriorityMode::AssemblyComponentPriority;

	/** Explicit role of this actor in snapping: AutoDetect uses socket count and assembly hierarchy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Assembly")
	ESnapRole SnapRole = ESnapRole::AutoDetect;

	/** If true, grabbing an attached part picks up the whole assembly (unless another part is held in the other hand to detach) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Assembly")
	bool bGrabAttachedPartPicksUpAssembly = true;

	/** If true, holding two connected parts with both hands allows two-handed hold and handoff; detaches only when pulled apart beyond DetachTensionDistance (HL: Alyx style) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|TwoHanded")
	bool bEnableTensionDisassembly = true;

	/** Tension distance (in centimeters) required to break / detach a snap connection when pulling with both hands (Default: 18 cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|TwoHanded", meta=(ClampMin="5.0", ClampMax="50.0"))
	float DetachTensionDistance = 18.0f;

	/** Timestamp when this actor was grabbed (used for adaptive priority) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|TwoHanded")
	double GrabTimestamp = 0.0;

	/** Name of the holding motion controller source (e.g. Left, Right) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|TwoHanded")
	FName HoldingHandSource = NAME_None;

	/** Whether to show a translucent ghost preview mesh at the target docking location */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost")
	bool bShowGhostPreview = true;

	/** Maximum distance (in centimeters) to show the ghost preview when held in hand (Default: 250 cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost", meta=(ClampMin="10.0"))
	float GhostPreviewMaxDistance = 250.0f;

	/** If true, ghost preview smoothly shrinks as it gets closer to the docking socket */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost")
	bool bEnableGhostProximityScale = false;

	/** Distance (in centimeters) across which the ghost smoothly shrinks upon approach (Default: 80 cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost", meta=(ClampMin="20.0", ClampMax="200.0"))
	float GhostProximityApproachDistance = 80.0f;

	/** Minimum scale multiplier of the ghost when held directly at the docking socket (e.g. 0.72 = 28% smaller) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost", meta=(ClampMin="0.4", ClampMax="1.2"))
	float GhostMinProximityScale = 0.72f;

	/** Base material to use for the ghost preview (translucent cyan/blue) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost")
	TObjectPtr<UMaterialInterface> GhostMaterial;

	/** Material for available docking slots when in range (Default: Blue / Cyan) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost")
	TObjectPtr<UMaterialInterface> GhostMaterialAvailable;

	/** Material for the closest docking slot when aligned and ready to snap upon release (Default: Green) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost")
	TObjectPtr<UMaterialInterface> GhostMaterialReady;

	/** Whether to display ghost previews at ALL available matching sockets in range (Default: true) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost")
	bool bShowAllAvailableGhosts = true;

	/** Maximum number of ghost previews to display simultaneously (Default: 8) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Ghost", meta=(ClampMin="1", ClampMax="16"))
	int32 MaxVisibleGhosts = 8;

	/** Optional sound played when a snap connection is locked */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Feedback")
	TObjectPtr<USoundBase> SnapCompletedSound = nullptr;

	/** Optional sound played when a snap connection is detached */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Feedback")
	TObjectPtr<USoundBase> SnapDetachedSound = nullptr;

	/** Optional sound played when a valid snap candidate first enters range */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Feedback")
	TObjectPtr<USoundBase> CandidateFoundSound = nullptr;

	/** Whether this actor can automatically snap into compatible sockets via physics proximity without being held */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|WorldSnapping")
	bool bAllowWorldSnapWithoutGrab = false;

	/** Whether a valid docking candidate socket is currently in range */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	bool bHasCandidate = false;

	/** Whether the candidate socket is close enough to dock upon release */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	bool bCanSnap = false;

	/** Whether the actor is currently smoothly interpolating towards the docking target */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	bool bIsInterpolating = false;

	/** Whether the actor is currently docked and attached to another actor */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	bool bIsSnapped = false;

	/** Whether this actor is currently held in two-handed tension mode with another part */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	bool bIsTwoHandedTensionActive = false;

	/** The snap socket on this actor that will connect */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	TObjectPtr<USnapSocketComponent> CandidateHeldSocket = nullptr;

	/** The target snap socket in the world that this actor will dock into */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	TObjectPtr<USnapSocketComponent> CandidateTargetSocket = nullptr;

	/** The computed world transform for the actor when docked */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	FTransform DesiredSnapTransform;

	/** Event triggered when a valid snap target enters or leaves range */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Events")
	FOnSnapCandidateChangedSignature OnSnapCandidateChanged;

	/** Event triggered when smooth snap finishes and actor is locked */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Events")
	FOnSnapCompletedSignature OnSnapCompleted;

	/** Event triggered when an attached actor is grabbed and detached */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Events")
	FOnSnapDetachedSignature OnSnapDetached;

	/** Event triggered each frame during two-handed tension pull (0.0 = relaxed, 1.0 = connection snaps open) */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Events")
	FOnSnapTensionChangedSignature OnSnapTensionChanged;

public:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Call when the actor is grabbed by VR controller or player */
	UFUNCTION(BlueprintCallable, Category="PointSnap|Controller")
	void NotifyGrabbed();

	/** Call when the actor is released / dropped */
	UFUNCTION(BlueprintCallable, Category="PointSnap|Controller")
	void NotifyDropped();

	/** Scans the world for the best compatible docking socket */
	UFUNCTION(BlueprintCallable, Category="PointSnap|Controller")
	bool ScanForBestCandidate(USnapSocketComponent*& OutHeldSocket, USnapSocketComponent*& OutTargetSocket, FTransform& OutTargetTransform);

	/** Manually triggers detachment from current docking socket (or a specific socket if specified) */
	UFUNCTION(BlueprintCallable, Category="PointSnap|Controller")
	void DetachFromSocket(USnapSocketComponent* SocketToDetach = nullptr);

	/** Returns whether this actor is currently held in hand */
	UFUNCTION(BlueprintPure, Category="PointSnap|Controller")
	bool IsHeld() const { return bIsHeldState; }

	/** Broadcasts assembly modified notification up the hierarchy to any USnapAssemblyComponent */
	static void NotifyHierarchyAssemblyChanged(AActor* InActor, AActor* ChangedPart, bool bAttached);

	/** Returns true if this actor acts as the stationary Base relative to OtherActor (Base never projects ghost onto guest) */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	bool IsBaseRelativeTo(const AActor* OtherActor) const;

	/** Detects which hand controller holds this actor */
	FName DetectHoldingHand() const;

	/** Returns all actors connected in this snap assembly graph */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	TArray<AActor*> GetAllConnectedAssemblyParts() const;

	/** Checks if this actor or any connected part has a USnapAssemblyComponent */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	bool HasAssemblyComponentInHierarchy() const;

	/** Checks if this actor currently has any occupied snap sockets */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	bool HasOccupiedSockets() const;

	/** Returns the root actor of the connected snap assembly */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	AActor* GetAssemblyRoot() const;

	/** Checks whether another part in this assembly is currently being held */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	bool IsAnotherAssemblyPartHeld() const;

	/** Returns the first stationary AlwaysBase (or controller-less) actor in this connected assembly, if any */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	AActor* GetAnchoredBasePartInAssembly() const;

	/** Checks whether another part in this assembly is anchored as AlwaysBase (stationary mount/workbench) */
	UFUNCTION(BlueprintPure, Category="PointSnap|Assembly")
	bool HasAnchoredBasePartInAssembly() const;

protected:
	void UpdateCandidateTracking();
	void UpdateInterpolation(float DeltaTime);
	void UpdateTwoHandedTension(float DeltaTime);
	void FinishSnap();

	void GatherCandidateSlots(TArray<FSnapCandidateSlot>& OutCandidates);
	void SetupGhostPreview();
	void SetGhostVisibility(bool bVisible);
	void SetGhostActorTransform(AStaticMeshActor* InGhostActor, const FTransform& NewTransform, AActor* SourceActor = nullptr);
	UMaterialInterface* GetGhostMaterialAvailable();
	UMaterialInterface* GetGhostMaterialReady();

	UPrimitiveComponent* GetOwnerPhysicsComponent() const;
	UActorComponent* GetActiveGrabComponent() const;
	bool CheckOwnerIsHeld() const;

	void PickUpAssemblyByThisPart();
	void ReleaseAssemblyPickedUpByThisPart();
	void SyncConnectedSubAssemblyHierarchy(bool bRealignTransforms = true);
	static void SetActorPhysicsCollisionState(AActor* InActor, bool bEnablePhysicsSimulationAndCollision);

private:
	bool bWasHeld = false;
	bool bIsHeldState = false;
	bool bIsHoldingWholeAssemblyByThisPart = false;

	float InterpElapsed = 0.0f;
	FTransform InterpStartTransform;
	FVector OriginalScale = FVector::OneVector;

	TWeakObjectPtr<UActorComponent> CachedGrabComponent;
	TArray<TWeakObjectPtr<UActorComponent>> CachedGrabComponents;

	// All other actors in the assembly that were temporarily attached when picked up by this part
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<AActor>> HeldAssemblyParts;

	// Prior physics simulation states of primitive components across held assembly parts
	TMap<TWeakObjectPtr<UPrimitiveComponent>, bool> SavedPhysicsStates;

	// Sticky target socket to prevent jitter/flickering between multiple sockets
	TWeakObjectPtr<USnapSocketComponent> LastTargetSocket;

	// Cached reflection properties to eliminate per-frame FindPropertyByName overhead
	mutable const FBoolProperty* CachedIsHeldProp = nullptr;
	mutable const FObjectProperty* CachedMotionControllerProp = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicAvailableMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicReadyMaterial;

	UPROPERTY(Transient)
	TObjectPtr<AStaticMeshActor> GhostActor;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AStaticMeshActor>> GhostActorsPool;

	AStaticMeshActor* GetOrCreateGhostActor(int32 Index);
	void HideAllGhosts();
	void UpdateGhostMeshAndMaterials(AStaticMeshActor* InGhostActor, UMaterialInterface* MaterialToUse, AActor* SourceActor = nullptr);
};
