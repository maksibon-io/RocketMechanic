#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SnapSocketComponent.generated.h"

class UArrowComponent;

UENUM(BlueprintType)
enum class ESocketRole : uint8
{
	Bidirectional UMETA(DisplayName="Bidirectional (Universal - Can plug or receive)"),
	Receiver      UMETA(DisplayName="Receiver / Slot (Female - Receives parts, never projects ghost)"),
	Plug          UMETA(DisplayName="Plug / Pin (Male - Plugs into receiver, projects ghost)")
};

/**
 * Controls how a docking part can be rotated (rolled) around the socket's Forward (X) axis.
 */
UENUM(BlueprintType)
enum class ESocketRollMode : uint8
{
	Fixed   UMETA(DisplayName="Fixed (1 Position - Asymmetric parts like wings or sights)"),
	Stepped UMETA(DisplayName="Stepped (N Positions - e.g. 4 steps = 90 deg for square blocks)"),
	Free360 UMETA(DisplayName="Free 360 (Any Roll Angle - Cylindrical parts like cones or tubes)")
};

/**
 * Per-type compatibility rule allowing a socket to specify both a compatible SnapType
 * and the specific ESocketRole (Receiver or Plug) to use when matching that type.
 */
USTRUCT(BlueprintType)
struct POINTSNAP_API FSnapCompatibleType
{
	GENERATED_BODY()

	/** Compatible snap socket type name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching")
	FName SnapType = NAME_None;

	/** Role this socket assumes when matching with SnapType (Receiver or Plug) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching")
	ESocketRole SocketRole = ESocketRole::Receiver;

	FSnapCompatibleType() = default;
	FSnapCompatibleType(FName InSnapType, ESocketRole InRole = ESocketRole::Receiver)
		: SnapType(InSnapType), SocketRole(InRole)
	{
	}
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSnapSocketConnectedSignature, USnapSocketComponent*, ConnectedSocket);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSnapSocketDisconnectedSignature);

/**
 * USnapSocketComponent
 * A modular docking socket / connection port component.
 * When placed on an actor, its position and Forward vector (X axis) define where and in
 * what direction other snap sockets can attach to it.
 */
UCLASS(ClassGroup=(PointSnap), meta=(BlueprintSpawnableComponent))
class POINTSNAP_API USnapSocketComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USnapSocketComponent();

	/** Socket role: Receiver (Female / slot) or Plug (Male / pin). Recommended: Receiver on base, Plug on docking part. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching")
	ESocketRole SocketRole = ESocketRole::Receiver;

	/** Snap socket type name (e.g. "Joint", "Standard", "SocketA", "WingMount") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching")
	FName SnapType = FName(TEXT("Joint"));

	/** Additional compatible snap types that this socket can dock with */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching")
	TArray<FName> CompatibleTypes;

	/** Structured compatible snap types with per-type Receiver/Plug role settings */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching")
	TArray<FSnapCompatibleType> CompatibleTypeRules;

	/** Maximum distance (in centimeters) for snapping to activate */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching", meta=(ClampMin="1.0"))
	float SnapRadius = 25.0f;

	/** Maximum angle deviation (in degrees) from opposing forward vectors */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching", meta=(ClampMin="5.0", ClampMax="180.0"))
	float AngleToleranceDegrees = 90.0f;

	/** Roll rotation mode around the socket's Forward (X) axis when docking */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching")
	ESocketRollMode RollMode = ESocketRollMode::Fixed;

	/** Number of discrete roll snap positions around the socket axis when RollMode is Stepped (e.g. 4 = every 90 deg, 6 = every 60 deg) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Matching", meta=(ClampMin="2", ClampMax="36", EditCondition="RollMode == ESocketRollMode::Stepped"))
	int32 RollSnapSteps = 4;

	/** Whether this socket currently has a piece connected to it */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	bool bIsOccupied = false;

	/** The snap socket currently connected to this socket, if any */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="PointSnap|State")
	TWeakObjectPtr<USnapSocketComponent> ConnectedComponent;

	/** Whether to draw debug line/sphere visuals during gameplay in editor/PIE (Default: false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Visual")
	bool bDrawDebugVisual = false;

	/** Color for the debug visual in editor and PIE */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="PointSnap|Visual")
	FColor DebugColor = FColor(0, 200, 255);

	/** Event triggered when a snap connection is locked */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Events")
	FOnSnapSocketConnectedSignature OnSnapSocketConnected;

	/** Event triggered when the snap connection is broken / detached */
	UPROPERTY(BlueprintAssignable, Category="PointSnap|Events")
	FOnSnapSocketDisconnectedSignature OnSnapSocketDisconnected;

#if WITH_EDITORONLY_DATA
	/** Visual arrow in the editor viewport showing docking forward direction */
	UPROPERTY(Transient)
	TObjectPtr<UArrowComponent> EditorArrow;
#endif

public:
	/** Global registry of all active snap sockets in the world (for high-speed candidate search without world iteration) */
	static TArray<TWeakObjectPtr<USnapSocketComponent>> GlobalSnapSockets;
	static const TArray<TWeakObjectPtr<USnapSocketComponent>>& GetAllSnapSockets() { return GlobalSnapSockets; }

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Returns the effective ESocketRole (Receiver/Plug) of this socket when interacting with OtherSocket */
	UFUNCTION(BlueprintPure, Category="PointSnap|Socket")
	ESocketRole GetEffectiveRoleFor(const USnapSocketComponent* OtherSocket) const;

	/** Checks if this snap socket can dock with OtherSocket based on occupancy, ownership, and type compatibility */
	UFUNCTION(BlueprintPure, Category="PointSnap|Socket")
	bool IsCompatibleWith(const USnapSocketComponent* OtherSocket) const;

	/** Checks if the forward vectors of this socket and OtherSocket are facing each other within AngleToleranceDegrees */
	UFUNCTION(BlueprintPure, Category="PointSnap|Socket")
	bool IsFacing(const USnapSocketComponent* OtherSocket) const;

	/** Calculates the World Transform that the owning Actor must have so this socket connects flush to TargetSocket */
	UFUNCTION(BlueprintPure, Category="PointSnap|Socket")
	FTransform CalculateDockingActorTransform(const USnapSocketComponent* TargetSocket) const;

	/** Calculates the World Transform that a specific MovingActor (e.g. assembly root) must have so this socket connects flush to TargetSocket */
	FTransform CalculateDockingActorTransformForActor(const USnapSocketComponent* TargetSocket, const AActor* MovingActor, bool bUseRollMode = true) const;

	/** Connects to TargetSocket, marking both as occupied */
	UFUNCTION(BlueprintCallable, Category="PointSnap|Socket")
	void SnapTo(USnapSocketComponent* TargetSocket);

	/** Disconnects from the current connected socket, freeing both */
	UFUNCTION(BlueprintCallable, Category="PointSnap|Socket")
	void Unsnap();
};
