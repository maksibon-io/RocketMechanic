#include "SnapSocketComponent.h"
#include "SnapControllerComponent.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Components/ArrowComponent.h"

namespace
{
	static AActor* GetNonPawnAttachmentRootActor(const USceneComponent* InComponent)
	{
		if (!InComponent)
		{
			return nullptr;
		}
		AActor* CurrentActor = InComponent->GetOwner();
		const USceneComponent* CurrentComp = InComponent;
		while (CurrentComp && CurrentComp->GetAttachParent())
		{
			const USceneComponent* ParentComp = CurrentComp->GetAttachParent();
			AActor* ParentActor = ParentComp->GetOwner();
			if (!ParentActor || ParentActor->IsA<APawn>() || ParentActor->IsA<AController>())
			{
				break;
			}
			CurrentActor = ParentActor;
			CurrentComp = ParentComp;
		}
		return CurrentActor;
	}
}

TArray<TWeakObjectPtr<USnapSocketComponent>> USnapSocketComponent::GlobalSnapSockets;

USnapSocketComponent::USnapSocketComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	bAutoActivate = true;
}

void USnapSocketComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (bDrawDebugVisual)
	{
		SetComponentTickEnabled(true);
	}
#endif

	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		GlobalSnapSockets.AddUnique(this);
	}

#if WITH_EDITOR
	if (!IsRunningCommandlet() && GetWorld() && !GetWorld()->IsGameWorld() && GetOwner())
	{
		if (!EditorArrow)
		{
			EditorArrow = NewObject<UArrowComponent>(GetOwner(), NAME_None, RF_Transient | RF_TextExportTransient);
			if (EditorArrow)
			{
				EditorArrow->ArrowColor = FColor(0, 200, 255);
				EditorArrow->ArrowSize = 0.35f;
				EditorArrow->bTreatAsASprite = true;
				EditorArrow->bIsScreenSizeScaled = true;
				EditorArrow->SetHiddenInGame(true);
				EditorArrow->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
				EditorArrow->RegisterComponent();
			}
		}
	}
#endif
}

void USnapSocketComponent::OnUnregister()
{
	if (bIsOccupied)
	{
		Unsnap();
	}

	GlobalSnapSockets.Remove(this);

#if WITH_EDITOR
	if (EditorArrow)
	{
		EditorArrow->DestroyComponent();
		EditorArrow = nullptr;
	}
#endif

	Super::OnUnregister();
}

void USnapSocketComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

#if WITH_EDITOR || !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (bDrawDebugVisual && GetWorld())
	{
		const FVector Loc = GetComponentLocation();
		const FVector Fwd = GetForwardVector();
		const FColor CurrentColor = bIsOccupied ? FColor::Red : DebugColor;

		// Draw small docking sphere and direction arrow
		DrawDebugSphere(GetWorld(), Loc, 2.5f, 8, CurrentColor, false, -1.0f, 0, 0.4f);
		DrawDebugDirectionalArrow(GetWorld(), Loc, Loc + (Fwd * 12.0f), 3.0f, CurrentColor, false, -1.0f, 0, 0.6f);
	}
#endif
}

ESocketRole USnapSocketComponent::GetEffectiveRoleFor(const USnapSocketComponent* OtherSocket) const
{
	if (OtherSocket && !OtherSocket->SnapType.IsNone())
	{
		for (const FSnapCompatibleType& Rule : CompatibleTypeRules)
		{
			if (Rule.SnapType == OtherSocket->SnapType)
			{
				return Rule.SocketRole;
			}
		}
	}
	return SocketRole;
}

bool USnapSocketComponent::IsCompatibleWith(const USnapSocketComponent* OtherSocket) const
{
	if (!OtherSocket || bIsOccupied || OtherSocket->bIsOccupied)
	{
		return false;
	}

	AActor* MyOwner = GetOwner();
	AActor* OtherOwner = OtherSocket->GetOwner();
	if (!MyOwner || !OtherOwner)
	{
		return false;
	}

	// Cannot snap to a socket on the exact same actor or within the same attached snap assembly hierarchy
	// (Ignore APawn / MotionController attachment root so holding two separate parts in both hands of VRPawn never hides the ghost!)
	if (MyOwner == OtherOwner || GetNonPawnAttachmentRootActor(this) == GetNonPawnAttachmentRootActor(OtherSocket))
	{
		return false;
	}

	// Resolve effective roles (supporting per-type role overrides in CompatibleTypeRules)
	const ESocketRole MyEffectiveRole = GetEffectiveRoleFor(OtherSocket);
	const ESocketRole OtherEffectiveRole = OtherSocket->GetEffectiveRoleFor(this);

	// Role compatibility: Two receivers cannot connect, two plugs cannot connect
	if (MyEffectiveRole == ESocketRole::Receiver && OtherEffectiveRole == ESocketRole::Receiver)
	{
		return false;
	}
	if (MyEffectiveRole == ESocketRole::Plug && OtherEffectiveRole == ESocketRole::Plug)
	{
		return false;
	}

	// Check type compatibility
	if (!SnapType.IsNone() && SnapType == OtherSocket->SnapType)
	{
		return true;
	}

	if (!OtherSocket->SnapType.IsNone() && CompatibleTypes.Contains(OtherSocket->SnapType))
	{
		return true;
	}

	if (!SnapType.IsNone() && OtherSocket->CompatibleTypes.Contains(SnapType))
	{
		return true;
	}

	if (!OtherSocket->SnapType.IsNone())
	{
		for (const FSnapCompatibleType& Rule : CompatibleTypeRules)
		{
			if (Rule.SnapType == OtherSocket->SnapType)
			{
				return true;
			}
		}
	}

	if (!SnapType.IsNone())
	{
		for (const FSnapCompatibleType& OtherRule : OtherSocket->CompatibleTypeRules)
		{
			if (OtherRule.SnapType == SnapType)
			{
				return true;
			}
		}
	}

	return false;
}

bool USnapSocketComponent::IsFacing(const USnapSocketComponent* OtherSocket) const
{
	if (!OtherSocket)
	{
		return false;
	}

	// Two sockets face each other when their forward vectors point towards each other (Dot < 0)
	const float EffectiveTolerance = FMath::Max(AngleToleranceDegrees, OtherSocket->AngleToleranceDegrees);
	const float Dot = FVector::DotProduct(GetForwardVector(), OtherSocket->GetForwardVector());
	const float MinDot = FMath::Cos(FMath::DegreesToRadians(180.0f - EffectiveTolerance));

	return Dot <= MinDot;
}

FTransform USnapSocketComponent::CalculateDockingActorTransform(const USnapSocketComponent* TargetSocket) const
{
	return CalculateDockingActorTransformForActor(TargetSocket, GetOwner(), true);
}

FTransform USnapSocketComponent::CalculateDockingActorTransformForActor(const USnapSocketComponent* TargetSocket, const AActor* MovingActor, bool bUseRollMode) const
{
	const AActor* EffectiveActor = MovingActor ? MovingActor : GetOwner();
	if (!TargetSocket || !EffectiveActor)
	{
		return FTransform::Identity;
	}

	// 1. Target docking socket base world orientation & location:
	// A 180-degree rotation around the target socket's local Up vector (Z axis)
	// turns Forward (+X) into Opposing Forward (-X) while keeping Up (+Z) aligned.
	FQuat TargetDockSocketQuat = (TargetSocket->GetComponentQuat() * FQuat(FVector::UpVector, UE_PI)).GetNormalized();
	const FVector TargetDockSocketLoc = TargetSocket->GetComponentLocation();

	// 2. Apply Roll Mode around the socket's Forward (+X) axis if Stepped or Free360 is active on either socket
	if (bUseRollMode)
	{
		ESocketRollMode EffectiveRollMode = ESocketRollMode::Fixed;
		int32 EffectiveSteps = 4;

		if (RollMode == ESocketRollMode::Free360 || TargetSocket->RollMode == ESocketRollMode::Free360)
		{
			EffectiveRollMode = ESocketRollMode::Free360;
		}
		else if (RollMode == ESocketRollMode::Stepped)
		{
			EffectiveRollMode = ESocketRollMode::Stepped;
			EffectiveSteps = FMath::Max(RollSnapSteps, 2);
		}
		else if (TargetSocket->RollMode == ESocketRollMode::Stepped)
		{
			EffectiveRollMode = ESocketRollMode::Stepped;
			EffectiveSteps = FMath::Max(TargetSocket->RollSnapSteps, 2);
		}

		if (EffectiveRollMode != ESocketRollMode::Fixed)
		{
			const FVector BaseUp = TargetDockSocketQuat.GetAxisZ();
			const FVector RollOrthoUp = (TargetDockSocketQuat * FQuat(FVector::ForwardVector, UE_HALF_PI)).GetAxisZ();
			const FVector CurrentUp = GetComponentQuat().GetAxisZ();

			const float SinRoll = FVector::DotProduct(CurrentUp, RollOrthoUp);
			const float CosRoll = FVector::DotProduct(CurrentUp, BaseUp);

			if ((SinRoll * SinRoll + CosRoll * CosRoll) > 1e-4f)
			{
				float RollAngleRad = FMath::Atan2(SinRoll, CosRoll);
				if (EffectiveRollMode == ESocketRollMode::Stepped)
				{
					const float StepRad = (2.0f * UE_PI) / static_cast<float>(EffectiveSteps);
					RollAngleRad = FMath::RoundToFloat(RollAngleRad / StepRad) * StepRad;
				}
				TargetDockSocketQuat = (TargetDockSocketQuat * FQuat(FVector::ForwardVector, RollAngleRad)).GetNormalized();
			}
		}
	}

	// 3. Pure rigid-body (SE(3)) offset of this socket relative to EffectiveActor:
	// Using world-space scaled offset unrotated by ActorQuat avoids FTransform::Inverse()
	// non-uniform scale axis-swapping when sockets are pitched/yawed (e.g., Pitch +-90 deg on a (0.15, 0.15, 0.40) scaled mesh).
	const FQuat ActorQuat = EffectiveActor->GetActorQuat();
	const FVector ActorLoc = EffectiveActor->GetActorLocation();
	const FVector ActorScale = EffectiveActor->GetActorScale3D();

	const FQuat SocketRelQuat = (ActorQuat.Inverse() * GetComponentQuat()).GetNormalized();
	const FVector SocketLocalScaledOffset = ActorQuat.UnrotateVector(GetComponentLocation() - ActorLoc);

	// 4. Solve for new Actor world rotation and location so this socket lands flush at TargetDockSocketLoc/Quat:
	const FQuat NewActorQuat = (TargetDockSocketQuat * SocketRelQuat.Inverse()).GetNormalized();
	const FVector NewActorLoc = TargetDockSocketLoc - NewActorQuat.RotateVector(SocketLocalScaledOffset);

	return FTransform(NewActorQuat, NewActorLoc, ActorScale);
}

void USnapSocketComponent::SnapTo(USnapSocketComponent* TargetSocket)
{
	if (!TargetSocket)
	{
		return;
	}

	bIsOccupied = true;
	ConnectedComponent = TargetSocket;

	TargetSocket->bIsOccupied = true;
	TargetSocket->ConnectedComponent = this;

	if (AActor* MyOwner = GetOwner())
	{
		if (USnapControllerComponent* MyCtrl = MyOwner->FindComponentByClass<USnapControllerComponent>())
		{
			MyCtrl->bIsSnapped = true;
		}
	}
	if (AActor* OtherOwner = TargetSocket->GetOwner())
	{
		if (USnapControllerComponent* OtherCtrl = OtherOwner->FindComponentByClass<USnapControllerComponent>())
		{
			OtherCtrl->bIsSnapped = true;
		}
	}

	OnSnapSocketConnected.Broadcast(TargetSocket);
	TargetSocket->OnSnapSocketConnected.Broadcast(this);
}

void USnapSocketComponent::Unsnap()
{
	USnapSocketComponent* Other = ConnectedComponent.Get();

	bIsOccupied = false;
	ConnectedComponent = nullptr;

	OnSnapSocketDisconnected.Broadcast();

	// Update owning actor's USnapControllerComponent bIsSnapped
	if (AActor* MyOwner = GetOwner())
	{
		if (USnapControllerComponent* MyCtrl = MyOwner->FindComponentByClass<USnapControllerComponent>())
		{
			MyCtrl->bIsSnapped = MyCtrl->HasOccupiedSockets();
		}
	}

	if (IsValid(Other))
	{
		Other->bIsOccupied = false;
		Other->ConnectedComponent = nullptr;
		Other->OnSnapSocketDisconnected.Broadcast();

		// Symmetrically update partner actor's USnapControllerComponent bIsSnapped
		if (AActor* OtherOwner = Other->GetOwner())
		{
			if (USnapControllerComponent* OtherCtrl = OtherOwner->FindComponentByClass<USnapControllerComponent>())
			{
				OtherCtrl->bIsSnapped = OtherCtrl->HasOccupiedSockets();
			}
		}
	}
}
