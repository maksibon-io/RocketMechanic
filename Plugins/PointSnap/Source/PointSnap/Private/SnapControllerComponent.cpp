#include "SnapControllerComponent.h"
#include "SnapAssemblyComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"

USnapControllerComponent::USnapControllerComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bAutoActivate = true;

	static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> OpacityMatFinder(TEXT("/PointSnap/M_Opacity"));
	if (OpacityMatFinder.Succeeded())
	{
		GhostMaterial = OpacityMatFinder.Get();
	}
	else
	{
		static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> GhostMatFinder(TEXT("/PointSnap/M_SnapGhost"));
		if (GhostMatFinder.Succeeded())
		{
			GhostMaterial = GhostMatFinder.Get();
		}
	}
}

void USnapControllerComponent::SetActorPhysicsCollisionState(AActor* InActor, bool bEnablePhysicsSimulationAndCollision)
{
	if (!IsValid(InActor))
	{
		return;
	}

	TArray<UPrimitiveComponent*> PrimitiveComponents;
	InActor->GetComponents(PrimitiveComponents);

	if (!bEnablePhysicsSimulationAndCollision)
	{
		// Disable physics simulation and switch Physics/QueryAndPhysics collision to QueryOnly
		// so held, interpolating, or attached child parts never impart Chaos depenetration impulses
		// to physics-simulating parts lying on the table, while still allowing VR grab overlap queries.
		for (UPrimitiveComponent* Prim : PrimitiveComponents)
		{
			if (!IsValid(Prim))
			{
				continue;
			}
			if (Prim->IsSimulatingPhysics())
			{
				Prim->SetSimulatePhysics(false);
			}
			const ECollisionEnabled::Type CurCollision = Prim->GetCollisionEnabled();
			if (CurCollision == ECollisionEnabled::QueryAndPhysics || CurCollision == ECollisionEnabled::PhysicsOnly)
			{
				Prim->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			}
		}
	}
	else
	{
		if (const USnapControllerComponent* Ctrl = InActor->FindComponentByClass<USnapControllerComponent>())
		{
			if (Ctrl->SnapRole == ESnapRole::AlwaysBase)
			{
				return;
			}
		}

		UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(InActor->GetRootComponent());
		if (!RootPrim)
		{
			RootPrim = InActor->FindComponentByClass<UPrimitiveComponent>();
		}
		if (IsValid(RootPrim))
		{
			if (RootPrim->GetCollisionEnabled() == ECollisionEnabled::QueryOnly)
			{
				RootPrim->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			}
			RootPrim->SetSimulatePhysics(true);
		}
	}
}

void USnapControllerComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwner())
	{
		// If the Blueprint has a non-primitive DefaultSceneRoot with a child UPrimitiveComponent (e.g. Cube/Cylinder/Cone),
		// promote the UPrimitiveComponent to RootComponent so physics simulation and BP_GrabComponent (AttachParentToMotionController)
		// never detach the mesh and sockets from the Actor's RootComponent!
		USceneComponent* CurrentRoot = GetOwner()->GetRootComponent();
		if (CurrentRoot && !CurrentRoot->IsA<UPrimitiveComponent>())
		{
			if (UPrimitiveComponent* MainPrim = GetOwner()->FindComponentByClass<UPrimitiveComponent>())
			{
				const FTransform PrimWorldTransform = MainPrim->GetComponentTransform();
				const bool bWasSimulating = MainPrim->IsSimulatingPhysics();
				if (bWasSimulating)
				{
					MainPrim->SetSimulatePhysics(false);
				}

				TArray<USceneComponent*> RootChildren = CurrentRoot->GetAttachChildren();
				MainPrim->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
				GetOwner()->SetRootComponent(MainPrim);
				MainPrim->SetWorldTransform(PrimWorldTransform);

				for (USceneComponent* Child : RootChildren)
				{
					if (IsValid(Child) && Child != MainPrim && Child != CurrentRoot)
					{
						Child->AttachToComponent(MainPrim, FAttachmentTransformRules::KeepWorldTransform);
					}
				}

				if (bWasSimulating)
				{
					MainPrim->SetSimulatePhysics(true);
				}
			}
		}

		OriginalScale = GetOwner()->GetActorScale3D();
	}

	if (bAutoDetectGrab && GetOwner())
	{
		CachedGrabComponents.Empty();
		TArray<UActorComponent*> Components;
		GetOwner()->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (Comp && (Comp->GetName().Contains(TEXT("Grab")) || Comp->GetClass()->FindPropertyByName(TEXT("bIsHeld")) != nullptr))
			{
				CachedGrabComponents.Add(Comp);
				if (!CachedGrabComponent.IsValid())
				{
					CachedGrabComponent = Comp;
					CachedIsHeldProp = CastField<FBoolProperty>(Comp->GetClass()->FindPropertyByName(TEXT("bIsHeld")));
					CachedMotionControllerProp = CastField<FObjectProperty>(Comp->GetClass()->FindPropertyByName(TEXT("motionControllerRef")));
				}
			}
		}
	}

	SetupGhostPreview();
}

void USnapControllerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Ensure held assembly parts are safely released and physics restored if actor is destroyed while held
	if (bIsHoldingWholeAssemblyByThisPart)
	{
		ReleaseAssemblyPickedUpByThisPart();
	}
	HeldAssemblyParts.Empty();
	SavedPhysicsStates.Empty();

	HideAllGhosts();

	// Clear GhostActor alias first to prevent double-destruction of GhostActorsPool[0]
	GhostActor = nullptr;

	for (TObjectPtr<AStaticMeshActor>& GhostPtr : GhostActorsPool)
	{
		if (IsValid(GhostPtr.Get()))
		{
			GhostPtr->Destroy();
		}
		GhostPtr = nullptr;
	}
	GhostActorsPool.Empty();

	// Clean up dynamic material instances created for ghost visualization
	DynamicAvailableMaterial = nullptr;
	DynamicReadyMaterial = nullptr;

	// Reset cached socket pointers
	CandidateHeldSocket = nullptr;
	CandidateTargetSocket = nullptr;
	LastTargetSocket.Reset();

	Super::EndPlay(EndPlayReason);
}

AStaticMeshActor* USnapControllerComponent::GetOrCreateGhostActor(int32 Index)
{
	if (Index < 0 || !GetWorld() || !GetOwner())
	{
		return nullptr;
	}

	UStaticMeshComponent* RootMesh = Cast<UStaticMeshComponent>(GetOwner()->GetRootComponent());
	if (!RootMesh)
	{
		RootMesh = GetOwner()->FindComponentByClass<UStaticMeshComponent>();
	}

	auto SpawnSingleGhost = [this, RootMesh]() -> AStaticMeshActor*
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = GetOwner();
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;

		AStaticMeshActor* NewGhost = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), SpawnParams);
		if (NewGhost)
		{
			NewGhost->SetActorEnableCollision(false);
			NewGhost->SetMobility(EComponentMobility::Movable);
			if (UStaticMeshComponent* GhostMeshComp = NewGhost->GetStaticMeshComponent())
			{
				GhostMeshComp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
				GhostMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				GhostMeshComp->SetGenerateOverlapEvents(false);
				GhostMeshComp->SetCastShadow(false);
				if (RootMesh && RootMesh->GetStaticMesh())
				{
					GhostMeshComp->SetStaticMesh(RootMesh->GetStaticMesh());
				}
			}
			NewGhost->SetActorHiddenInGame(true);
		}
		return NewGhost;
	};

	while (GhostActorsPool.Num() <= Index)
	{
		AStaticMeshActor* NewGhost = SpawnSingleGhost();
		if (NewGhost)
		{
			GhostActorsPool.Add(NewGhost);
		}
		else
		{
			break;
		}
	}

	if (GhostActorsPool.IsValidIndex(Index))
	{
		// If the pooled actor was destroyed externally or marked PendingKill, respawn it
		if (!IsValid(GhostActorsPool[Index].Get()))
		{
			GhostActorsPool[Index] = SpawnSingleGhost();
		}

		AStaticMeshActor* Ghost = GhostActorsPool[Index];
		if (Index == 0)
		{
			GhostActor = Ghost;
		}
		return Ghost;
	}
	return nullptr;
}

void USnapControllerComponent::HideAllGhosts()
{
	for (TObjectPtr<AStaticMeshActor>& Ghost : GhostActorsPool)
	{
		if (IsValid(Ghost.Get()))
		{
			Ghost->SetActorHiddenInGame(true);
		}
	}
	if (IsValid(GhostActor.Get()))
	{
		GhostActor->SetActorHiddenInGame(true);
	}
}

void USnapControllerComponent::SetupGhostPreview()
{
	if (!bShowGhostPreview || !GetWorld() || !GetOwner())
	{
		return;
	}

	// Pre-warm primary ghost actor and dynamic materials
	GhostActor = GetOrCreateGhostActor(0);
	if (GhostActor)
	{
		UpdateGhostMeshAndMaterials(GhostActor, GetGhostMaterialAvailable());
		GhostActor->SetActorHiddenInGame(true);
	}
}

void USnapControllerComponent::SetGhostVisibility(bool bVisible)
{
	if (!bVisible)
	{
		HideAllGhosts();
	}
	else if (GhostActor)
	{
		GhostActor->SetActorHiddenInGame(false);
	}
}

void USnapControllerComponent::SetGhostActorTransform(AStaticMeshActor* InGhostActor, const FTransform& NewTransform, AActor* SourceActor)
{
	AActor* EffectiveSource = SourceActor ? SourceActor : GetOwner();
	if (!InGhostActor || !EffectiveSource)
	{
		return;
	}

	UStaticMeshComponent* RootMesh = Cast<UStaticMeshComponent>(EffectiveSource->GetRootComponent());
	if (!RootMesh)
	{
		RootMesh = EffectiveSource->FindComponentByClass<UStaticMeshComponent>();
	}

	if (RootMesh && RootMesh != EffectiveSource->GetRootComponent())
	{
		const FQuat ActorQuat = EffectiveSource->GetActorQuat();
		const FVector ActorLoc = EffectiveSource->GetActorLocation();
		const FVector ActorScale = EffectiveSource->GetActorScale3D();

		const FQuat MeshRelQuat = (ActorQuat.Inverse() * RootMesh->GetComponentQuat()).GetNormalized();
		const FVector MeshLocalScaledOffset = ActorQuat.UnrotateVector(RootMesh->GetComponentLocation() - ActorLoc);
		const FVector ScaleRatio(
			FMath::IsNearlyZero(ActorScale.X) ? 1.0f : (NewTransform.GetScale3D().X / ActorScale.X),
			FMath::IsNearlyZero(ActorScale.Y) ? 1.0f : (NewTransform.GetScale3D().Y / ActorScale.Y),
			FMath::IsNearlyZero(ActorScale.Z) ? 1.0f : (NewTransform.GetScale3D().Z / ActorScale.Z)
		);

		const FQuat GhostQuat = (NewTransform.GetRotation() * MeshRelQuat).GetNormalized();
		const FVector GhostLoc = NewTransform.GetLocation() + NewTransform.GetRotation().RotateVector(MeshLocalScaledOffset * ScaleRatio);
		const FVector GhostScale = RootMesh->GetComponentScale() * ScaleRatio;

		InGhostActor->SetActorTransform(FTransform(GhostQuat, GhostLoc, GhostScale));
	}
	else
	{
		InGhostActor->SetActorTransform(NewTransform);
	}
}

void USnapControllerComponent::UpdateGhostMeshAndMaterials(AStaticMeshActor* InGhostActor, UMaterialInterface* MaterialToUse, AActor* SourceActor)
{
	AActor* EffectiveSource = SourceActor ? SourceActor : GetOwner();
	if (!InGhostActor || !EffectiveSource)
	{
		return;
	}

	InGhostActor->SetActorEnableCollision(false);

	UStaticMeshComponent* RootMesh = Cast<UStaticMeshComponent>(EffectiveSource->GetRootComponent());
	if (!RootMesh)
	{
		RootMesh = EffectiveSource->FindComponentByClass<UStaticMeshComponent>();
	}
	if (!RootMesh || !RootMesh->GetStaticMesh())
	{
		return;
	}

	UStaticMeshComponent* GhostMeshComp = InGhostActor->GetStaticMeshComponent();
	if (!GhostMeshComp)
	{
		return;
	}

	if (GhostMeshComp->GetStaticMesh() != RootMesh->GetStaticMesh())
	{
		GhostMeshComp->SetStaticMesh(RootMesh->GetStaticMesh());
	}
	GhostMeshComp->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	GhostMeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostMeshComp->SetCastShadow(false);

	if (MaterialToUse)
	{
		const int32 NumMats = FMath::Max(GhostMeshComp->GetNumMaterials(), 1);
		for (int32 i = 0; i < NumMats; ++i)
		{
			if (GhostMeshComp->GetMaterial(i) != MaterialToUse)
			{
				GhostMeshComp->SetMaterial(i, MaterialToUse);
			}
		}
	}
}

UMaterialInterface* USnapControllerComponent::GetGhostMaterialAvailable()
{
	if (GhostMaterialAvailable)
	{
		return GhostMaterialAvailable;
	}
	if (DynamicAvailableMaterial)
	{
		return DynamicAvailableMaterial;
	}

	// 1. Prioritize plugin's translucent material with Color parameter
	UMaterialInterface* BaseMat = GhostMaterial;
	if (!BaseMat)
	{
		BaseMat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/PointSnap/M_Opacity")));
	}
	if (!BaseMat)
	{
		BaseMat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/PointSnap/M_SnapGhost")));
	}
	if (!BaseMat)
	{
		BaseMat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Engine/EngineDebugMaterials/M_SimpleTranslucent")));
	}

	if (BaseMat)
	{
		DynamicAvailableMaterial = UMaterialInstanceDynamic::Create(BaseMat, this);
		if (DynamicAvailableMaterial)
		{
			// Cool cyan / blue tint for available docking sockets
			const FLinearColor AvailableColor(0.0f, 0.75f, 1.0f, 1.0f);
			DynamicAvailableMaterial->SetVectorParameterValue(TEXT("Color"), AvailableColor);
			DynamicAvailableMaterial->SetVectorParameterValue(TEXT("BaseColor"), AvailableColor);
			DynamicAvailableMaterial->SetVectorParameterValue(TEXT("EmissiveColor"), AvailableColor);
			DynamicAvailableMaterial->SetVectorParameterValue(TEXT("bright"), FLinearColor(0.2f, 0.85f, 1.0f, 1.0f));
			DynamicAvailableMaterial->SetVectorParameterValue(TEXT("dark"), FLinearColor(0.0f, 0.25f, 0.6f, 1.0f));
			DynamicAvailableMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.45f);
			return DynamicAvailableMaterial;
		}
		return BaseMat;
	}
	return nullptr;
}

UMaterialInterface* USnapControllerComponent::GetGhostMaterialReady()
{
	if (GhostMaterialReady)
	{
		return GhostMaterialReady;
	}
	if (DynamicReadyMaterial)
	{
		return DynamicReadyMaterial;
	}

	// 1. Prioritize plugin's translucent material with Color parameter
	UMaterialInterface* BaseMat = GhostMaterial;
	if (!BaseMat)
	{
		BaseMat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/PointSnap/M_Opacity")));
	}
	if (!BaseMat)
	{
		BaseMat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/PointSnap/M_SnapGhost")));
	}
	if (!BaseMat)
	{
		BaseMat = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Engine/EngineDebugMaterials/M_SimpleTranslucent")));
	}

	if (BaseMat)
	{
		DynamicReadyMaterial = UMaterialInstanceDynamic::Create(BaseMat, this);
		if (DynamicReadyMaterial)
		{
			// Vibrant green tint for ready-to-snap socket
			const FLinearColor ReadyColor(0.05f, 1.0f, 0.2f, 1.0f);
			DynamicReadyMaterial->SetVectorParameterValue(TEXT("Color"), ReadyColor);
			DynamicReadyMaterial->SetVectorParameterValue(TEXT("BaseColor"), ReadyColor);
			DynamicReadyMaterial->SetVectorParameterValue(TEXT("EmissiveColor"), ReadyColor);
			DynamicReadyMaterial->SetVectorParameterValue(TEXT("bright"), FLinearColor(0.3f, 1.0f, 0.4f, 1.0f));
			DynamicReadyMaterial->SetVectorParameterValue(TEXT("dark"), FLinearColor(0.05f, 0.5f, 0.1f, 1.0f));
			DynamicReadyMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.75f);
			return DynamicReadyMaterial;
		}
		return BaseMat;
	}
	return nullptr;
}

void USnapControllerComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bAutoDetectGrab)
	{
		const bool bCurrentHeld = CheckOwnerIsHeld();
		if (bCurrentHeld != bWasHeld)
		{
			bWasHeld = bCurrentHeld;
			if (bCurrentHeld)
			{
				NotifyGrabbed();
			}
			else
			{
				NotifyDropped();
			}
		}
	}

	if (bIsHeldState)
	{
		// Ensure held actor keeps QueryOnly collision so it never kicks physics parts lying on the table
		SetActorPhysicsCollisionState(GetOwner(), false);

		if (bIsTwoHandedTensionActive)
		{
			UpdateTwoHandedTension(DeltaTime);
		}
		else
		{
			UpdateCandidateTracking();
		}
	}
	else if (bAllowWorldSnapWithoutGrab && !bIsSnapped && !bIsInterpolating)
	{
		UpdateCandidateTracking();
	}
	else if (bIsInterpolating)
	{
		UpdateInterpolation(DeltaTime);
	}
}

void USnapControllerComponent::NotifyGrabbed()
{
	bIsHeldState = true;
	GrabTimestamp = FPlatformTime::Seconds();
	HoldingHandSource = DetectHoldingHand();
	LastTargetSocket = nullptr;

	if (bIsInterpolating)
	{
		bIsInterpolating = false;
	}

	AActor* OwnerActor = GetOwner();
	SetActorPhysicsCollisionState(OwnerActor, false);

	const bool bHasConnected = HasOccupiedSockets();

	if (bIsSnapped || bHasConnected)
	{
		// Check if another part of this assembly is held in the other hand OR anchored as AlwaysBase (stationary stand/wall mount)
		if (IsAnotherAssemblyPartHeld() || HasAnchoredBasePartInAssembly())
		{
			// Enter tension mode so pulling away detaches from the held or anchored base without pulling an AlwaysBase off the table/wall!
			if (bEnableTensionDisassembly)
			{
				bIsTwoHandedTensionActive = true;
				SetGhostVisibility(false);

				// Symmetrically activate tension mode on all other held parts in the assembly
				const TArray<AActor*> ConnectedParts = GetAllConnectedAssemblyParts();
				for (AActor* Part : ConnectedParts)
				{
					if (Part && Part != OwnerActor)
					{
						if (USnapControllerComponent* OtherCtrl = Part->FindComponentByClass<USnapControllerComponent>())
						{
							if (OtherCtrl->bIsHeldState)
							{
								OtherCtrl->bIsTwoHandedTensionActive = true;
								OtherCtrl->SetGhostVisibility(false);
							}
						}
					}
				}
			}
			else
			{
				DetachFromSocket();
			}
		}
		else
		{
			// Single hand grab: pick up the ENTIRE assembly together
			bIsTwoHandedTensionActive = false;
			if (bGrabAttachedPartPicksUpAssembly)
			{
				PickUpAssemblyByThisPart();
			}
		}
	}
}

void USnapControllerComponent::UpdateTwoHandedTension(float DeltaTime)
{
	if (!bIsHeldState || (!IsAnotherAssemblyPartHeld() && !HasAnchoredBasePartInAssembly()))
	{
		// Second hand released! Exit two-handed state.
		bIsTwoHandedTensionActive = false;
		OnSnapTensionChanged.Broadcast(0.0f);

		// If we are still held, ensure we take over and carry the assembly (seamless handoff)!
		if (bIsHeldState && bGrabAttachedPartPicksUpAssembly && !HasAnchoredBasePartInAssembly())
		{
			PickUpAssemblyByThisPart();
		}
		return;
	}

	// Measure maximum tension across our occupied sockets and find the specifically strained socket
	float MaxTensionDist = 0.0f;
	USnapSocketComponent* StrainedSocket = nullptr;
	TArray<USnapSocketComponent*> MySockets;

	if (AActor* OwnerActor = GetOwner())
	{
		OwnerActor->GetComponents(MySockets);
		for (USnapSocketComponent* Socket : MySockets)
		{
			if (Socket && Socket->bIsOccupied && Socket->ConnectedComponent.IsValid())
			{
				const float Dist = FVector::Dist(Socket->GetComponentLocation(), Socket->ConnectedComponent->GetComponentLocation());
				if (Dist > MaxTensionDist)
				{
					MaxTensionDist = Dist;
					StrainedSocket = Socket;
				}
			}
		}
	}

	const float TensionAlpha = FMath::Clamp(MaxTensionDist / FMath::Max(DetachTensionDistance, 1.0f), 0.0f, 1.0f);
	OnSnapTensionChanged.Broadcast(TensionAlpha);

	// If pulled beyond tension distance: SNAP DETACH only the specifically strained joint!
	if (MaxTensionDist >= DetachTensionDistance && StrainedSocket)
	{
		bIsTwoHandedTensionActive = false;
		OnSnapTensionChanged.Broadcast(0.0f);
		DetachFromSocket(StrainedSocket);
	}
}

void USnapControllerComponent::SyncConnectedSubAssemblyHierarchy(bool bRealignTransforms)
{
	AActor* OwnerActor = GetOwner();
	if (!IsValid(OwnerActor))
	{
		return;
	}

	TSet<AActor*> Visited;
	TArray<AActor*> Queue;
	Visited.Add(OwnerActor);
	Queue.Add(OwnerActor);

	// If this actor is not held and is attached to a parent actor (e.g. BP_OilBase attached to BP_Engine),
	// do not traverse upward into the attach parent (prevents cyclic AttachTo warnings).
	if (!bIsHeldState && IsValid(OwnerActor->GetAttachParentActor()))
	{
		Visited.Add(OwnerActor->GetAttachParentActor());
	}

	for (int32 QueueIdx = 0; QueueIdx < Queue.Num(); ++QueueIdx)
	{
		AActor* ParentPart = Queue[QueueIdx];
		if (!IsValid(ParentPart))
		{
			continue;
		}

		TArray<USnapSocketComponent*> ParentSockets;
		ParentPart->GetComponents(ParentSockets);

		for (USnapSocketComponent* ParentSocket : ParentSockets)
		{
			if (!IsValid(ParentSocket) || !ParentSocket->bIsOccupied || !ParentSocket->ConnectedComponent.IsValid())
			{
				continue;
			}

			USnapSocketComponent* ChildSocket = ParentSocket->ConnectedComponent.Get();
			AActor* ChildPart = ChildSocket ? ChildSocket->GetOwner() : nullptr;
			if (!IsValid(ChildPart) || Visited.Contains(ChildPart))
			{
				continue;
			}

			// Never reparent or move a stationary AlwaysBase (or controller-less mount) actor as a child of another part!
			if (ParentPart != ChildPart)
			{
				const USnapControllerComponent* ChildCtrl = ChildPart->FindComponentByClass<USnapControllerComponent>();
				if (!ChildCtrl || ChildCtrl->SnapRole == ESnapRole::AlwaysBase)
				{
					Visited.Add(ChildPart);
					continue;
				}
			}

			Visited.Add(ChildPart);
			Queue.Add(ChildPart);

			// If ParentPart is currently attached to ChildPart (e.g. picking up whole assembly by a child part),
			// detach ParentPart from ChildPart first so attaching ChildPart to ParentPart never forms a cycle.
			if (ParentPart->GetAttachParentActor() == ChildPart)
			{
				ParentPart->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			}

			// Disable physics and switch attached child collision to QueryOnly so it never depenetrates against parent physics
			SetActorPhysicsCollisionState(ChildPart, false);

			if (bRealignTransforms)
			{
				const FTransform ExactChildTransform = ChildSocket->CalculateDockingActorTransformForActor(ParentSocket, ChildPart, true);
				ChildPart->SetActorTransform(ExactChildTransform, false, nullptr, ETeleportType::TeleportPhysics);
				if (ChildPart->GetAttachParentActor() != ParentPart)
				{
					ChildPart->AttachToActor(ParentPart, FAttachmentTransformRules::KeepWorldTransform);
				}
				if (USceneComponent* ChildRoot = ChildPart->GetRootComponent())
				{
					ChildRoot->SetWorldScale3D(ExactChildTransform.GetScale3D());
				}
			}
			else
			{
				if (ChildPart->GetAttachParentActor() != ParentPart)
				{
					ChildPart->AttachToActor(ParentPart, FAttachmentTransformRules::KeepWorldTransform);
				}
			}
		}
	}
}

void USnapControllerComponent::PickUpAssemblyByThisPart()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	bIsHoldingWholeAssemblyByThisPart = true;
	HeldAssemblyParts.Empty();
	SavedPhysicsStates.Empty();

	SetActorPhysicsCollisionState(OwnerActor, false);

	const TArray<AActor*> AllParts = GetAllConnectedAssemblyParts();
	for (AActor* OtherPart : AllParts)
	{
		if (OtherPart && OtherPart != OwnerActor)
		{
			HeldAssemblyParts.Add(OtherPart);

			// Query ALL primitive components on this part to support multi-primitive actors
			TArray<UPrimitiveComponent*> PrimitiveComponents;
			OtherPart->GetComponents(PrimitiveComponents);
			for (UPrimitiveComponent* Prim : PrimitiveComponents)
			{
				if (Prim)
				{
					SavedPhysicsStates.Add(Prim, Prim->IsSimulatingPhysics());
				}
			}
			SetActorPhysicsCollisionState(OtherPart, false);
		}
	}

	// Build parent-child attachment hierarchy from OwnerActor along occupied sockets and lock transforms
	SyncConnectedSubAssemblyHierarchy(true);
}

void USnapControllerComponent::ReleaseAssemblyPickedUpByThisPart()
{
	bIsHoldingWholeAssemblyByThisPart = false;

	// Ensure connected sub-assembly children stay non-simulating (QueryOnly) and attached to OwnerActor
	SyncConnectedSubAssemblyHierarchy(true);

	HeldAssemblyParts.Empty();
	SavedPhysicsStates.Empty();

	// Enable physics and QueryAndPhysics collision on the root part so the assembly falls onto the table normally
	SetActorPhysicsCollisionState(GetOwner(), true);
}

void USnapControllerComponent::NotifyDropped()
{
	bIsHeldState = false;
	bIsTwoHandedTensionActive = false;
	OnSnapTensionChanged.Broadcast(0.0f);
	LastTargetSocket = nullptr;

	// Always hide ghost preview upon release
	SetGhostVisibility(false);

	// Case 1: Releasing a hand while another assembly part is STILL held in the other hand!
	// Fix silent drop / two-handed handoff bug: Do NOT enable physics. Pass assembly ownership to remaining held part.
	if (IsAnotherAssemblyPartHeld())
	{
		if (bIsHoldingWholeAssemblyByThisPart)
		{
			bIsHoldingWholeAssemblyByThisPart = false;
			HeldAssemblyParts.Empty();
			SavedPhysicsStates.Empty();
		}

		// Ensure physics remains disabled on this part so it stays attached to the assembly
		SetActorPhysicsCollisionState(GetOwner(), false);

		// Notify the remaining held controller(s) to exit tension and resume holding the entire assembly
		const TArray<AActor*> ConnectedParts = GetAllConnectedAssemblyParts();
		for (AActor* Part : ConnectedParts)
		{
			if (Part && Part != GetOwner())
			{
				if (USnapControllerComponent* OtherCtrl = Part->FindComponentByClass<USnapControllerComponent>())
				{
					if (OtherCtrl->bIsHeldState)
					{
						OtherCtrl->bIsTwoHandedTensionActive = false;
						OtherCtrl->OnSnapTensionChanged.Broadcast(0.0f);
						if (OtherCtrl->bGrabAttachedPartPicksUpAssembly)
						{
							OtherCtrl->PickUpAssemblyByThisPart();
						}
					}
				}
			}
		}

		bHasCandidate = false;
		bCanSnap = false;
		return;
	}

	// Case 1B: Releasing a part while still connected to an anchored AlwaysBase stand (pulled less than DetachTensionDistance):
	// Snap the part and its sub-assembly right back to its locked socket on the anchored base!
	if (AActor* AnchoredBase = GetAnchoredBasePartInAssembly())
	{
		SetActorPhysicsCollisionState(GetOwner(), false);
		if (USnapControllerComponent* BaseCtrl = AnchoredBase->FindComponentByClass<USnapControllerComponent>())
		{
			BaseCtrl->SyncConnectedSubAssemblyHierarchy(true);
		}
		else
		{
			// Fallback when the anchored base only has USnapSocketComponent without USnapControllerComponent
			TArray<USnapSocketComponent*> MySockets;
			if (GetOwner())
			{
				GetOwner()->GetComponents(MySockets);
				for (USnapSocketComponent* MySocket : MySockets)
				{
					if (MySocket && MySocket->bIsOccupied && MySocket->ConnectedComponent.IsValid())
					{
						USnapSocketComponent* TargetSocket = MySocket->ConnectedComponent.Get();
						if (TargetSocket->GetOwner() == AnchoredBase)
						{
							const FTransform LockedTransform = MySocket->CalculateDockingActorTransformForActor(TargetSocket, GetOwner(), true);
							GetOwner()->SetActorTransform(LockedTransform, false, nullptr, ETeleportType::TeleportPhysics);
							GetOwner()->AttachToActor(AnchoredBase, FAttachmentTransformRules::KeepWorldTransform);
							if (USceneComponent* RootComp = GetOwner()->GetRootComponent())
							{
								RootComp->SetWorldScale3D(LockedTransform.GetScale3D());
							}
							break;
						}
					}
				}
			}
			SyncConnectedSubAssemblyHierarchy(true);
		}
		bHasCandidate = false;
		bCanSnap = false;
		return;
	}

	// Case 2: Smooth magnetic snap interpolation (priority if in snap range, docks whole assembly!)
	if (bHasCandidate && bCanSnap && IsValid(CandidateHeldSocket.Get()) && IsValid(CandidateTargetSocket.Get()))
	{
		bIsInterpolating = true;
		InterpElapsed = 0.0f;
		InterpStartTransform = IsValid(GetOwner()) ? GetOwner()->GetActorTransform() : FTransform::Identity;

		// Disable physics simulation and keep QueryOnly collision during interpolation so it never kicks TargetActor
		SetActorPhysicsCollisionState(GetOwner(), false);

		// Ensure all connected sub-assembly parts remain attached and aligned during interpolation
		SyncConnectedSubAssemblyHierarchy(true);

		bHasCandidate = false;
		bCanSnap = false;
		OnSnapCandidateChanged.Broadcast(false, FTransform::Identity);
		return;
	}

	// Case 3: Releasing an assembly that this part was holding into free world (no snap candidate)
	if (bIsHoldingWholeAssemblyByThisPart)
	{
		ReleaseAssemblyPickedUpByThisPart();
		bHasCandidate = false;
		bCanSnap = false;
		OnSnapCandidateChanged.Broadcast(false, FTransform::Identity);
		return;
	}

	// Case 4: Dropped freely outside snap range with no other part held
	CandidateHeldSocket = nullptr;
	CandidateTargetSocket = nullptr;
	SetActorPhysicsCollisionState(GetOwner(), true);
	bHasCandidate = false;
	bCanSnap = false;
}

void USnapControllerComponent::DetachFromSocket(USnapSocketComponent* SocketToDetach)
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		return;
	}

	AActor* OldParent = OwnerActor->GetAttachParentActor();

	// 1. Collect partner actors across the socket(s) being detached before unsnapping
	TArray<AActor*> DisconnectedPartnerActors;
	if (SocketToDetach)
	{
		if (SocketToDetach->bIsOccupied)
		{
			if (SocketToDetach->ConnectedComponent.IsValid() && SocketToDetach->ConnectedComponent->GetOwner())
			{
				DisconnectedPartnerActors.AddUnique(SocketToDetach->ConnectedComponent->GetOwner());
			}
			SocketToDetach->Unsnap();
		}
	}
	else
	{
		TArray<USnapSocketComponent*> MySockets;
		OwnerActor->GetComponents(MySockets);
		for (USnapSocketComponent* Socket : MySockets)
		{
			if (Socket && Socket->bIsOccupied)
			{
				if (Socket->ConnectedComponent.IsValid() && Socket->ConnectedComponent->GetOwner())
				{
					DisconnectedPartnerActors.AddUnique(Socket->ConnectedComponent->GetOwner());
				}
				Socket->Unsnap();
			}
		}
	}

	// 2. Determine which parts are STILL connected to OwnerActor after unsnapping
	const TArray<AActor*> RemainingConnectedParts = GetAllConnectedAssemblyParts();

	// 3. Handle parts that were separated from OwnerActor
	for (AActor* PartnerActor : DisconnectedPartnerActors)
	{
		if (!IsValid(PartnerActor) || RemainingConnectedParts.Contains(PartnerActor))
		{
			continue;
		}

		// If PartnerActor was attached to OwnerActor (or to a part in OwnerActor's remaining assembly), detach it
		if (RemainingConnectedParts.Contains(PartnerActor->GetAttachParentActor()))
		{
			PartnerActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}

		USnapControllerComponent* PartnerCtrl = PartnerActor->FindComponentByClass<USnapControllerComponent>();
		const TArray<AActor*> PartnerSubAssembly = PartnerCtrl ? PartnerCtrl->GetAllConnectedAssemblyParts() : TArray<AActor*>{ PartnerActor };

		// Check if any part in the separated sub-assembly is currently held by the other hand
		USnapControllerComponent* HeldPartnerCtrl = nullptr;
		for (AActor* SubPart : PartnerSubAssembly)
		{
			if (IsValid(SubPart))
			{
				if (USnapControllerComponent* SubCtrl = SubPart->FindComponentByClass<USnapControllerComponent>())
				{
					if (SubCtrl->bIsHeldState)
					{
						HeldPartnerCtrl = SubCtrl;
						break;
					}
				}
			}
		}

		if (HeldPartnerCtrl)
		{
			HeldPartnerCtrl->bIsTwoHandedTensionActive = false;
			if (HeldPartnerCtrl->bGrabAttachedPartPicksUpAssembly)
			{
				HeldPartnerCtrl->PickUpAssemblyByThisPart();
			}
		}
		else
		{
			// Separated sub-assembly is not held: rebuild its hierarchy from PartnerActor and enable physics on its root
			if (PartnerCtrl)
			{
				PartnerCtrl->SyncConnectedSubAssemblyHierarchy(true);
			}
			AActor* PartnerRoot = PartnerActor->GetRootComponent() ? PartnerActor->GetRootComponent()->GetAttachmentRootActor() : PartnerActor;
			if (IsValid(PartnerRoot))
			{
				USnapControllerComponent* PartnerRootCtrl = PartnerRoot->FindComponentByClass<USnapControllerComponent>();
				const bool bIsAlwaysBase = PartnerRootCtrl && (PartnerRootCtrl->SnapRole == ESnapRole::AlwaysBase);
				if (!bIsAlwaysBase)
				{
					SetActorPhysicsCollisionState(PartnerRoot, true);
				}
			}
		}
	}

	// Synchronize local bIsSnapped based on remaining occupied sockets
	bIsSnapped = HasOccupiedSockets();
	bIsHoldingWholeAssemblyByThisPart = false;
	bIsTwoHandedTensionActive = false;
	HeldAssemblyParts.Empty();
	SavedPhysicsStates.Empty();
	CandidateHeldSocket = nullptr;
	CandidateTargetSocket = nullptr;
	LastTargetSocket = nullptr;

	// 4. Resolve OwnerActor's attachment and remaining sub-assembly
	const bool bStillConnectedToOldParent = OldParent && RemainingConnectedParts.Contains(OldParent);
	if (!bStillConnectedToOldParent)
	{
		USceneComponent* MCComponent = nullptr;
		if (UActorComponent* ActiveGrabComp = GetActiveGrabComponent())
		{
			if (const FObjectProperty* MotionControllerProp = CastField<FObjectProperty>(ActiveGrabComp->GetClass()->FindPropertyByName(TEXT("motionControllerRef"))))
			{
				MCComponent = Cast<USceneComponent>(MotionControllerProp->GetObjectPropertyValue_InContainer(ActiveGrabComp));
			}
		}

		if (MCComponent && bIsHeldState)
		{
			OwnerActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			OwnerActor->GetRootComponent()->AttachToComponent(MCComponent, FAttachmentTransformRules::KeepWorldTransform);
			OwnerActor->GetRootComponent()->SetWorldScale3D(OriginalScale);
			SetActorPhysicsCollisionState(OwnerActor, false);
		}
		else
		{
			OwnerActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			if (OwnerActor->GetRootComponent())
			{
				OwnerActor->GetRootComponent()->SetWorldScale3D(OriginalScale);
			}
			if (!bIsHeldState)
			{
				SetActorPhysicsCollisionState(OwnerActor, true);
			}
			else
			{
				SetActorPhysicsCollisionState(OwnerActor, false);
			}
		}
	}

	// If OwnerActor still has attached sub-assembly parts (e.g. BP_Stab on BP_OilBase), keep them attached!
	if (RemainingConnectedParts.Num() > 1)
	{
		if (bIsHeldState && bGrabAttachedPartPicksUpAssembly)
		{
			PickUpAssemblyByThisPart();
		}
		else
		{
			SyncConnectedSubAssemblyHierarchy(true);
		}
	}

	if (SnapDetachedSound && GetWorld())
	{
		UGameplayStatics::PlaySoundAtLocation(GetWorld(), SnapDetachedSound, OwnerActor->GetActorLocation());
	}

	OnSnapDetached.Broadcast();

	// Notify BOTH sides of the split hierarchy so both USnapAssemblyComponents re-evaluate
	if (OldParent)
	{
		NotifyHierarchyAssemblyChanged(OldParent, OwnerActor, false);
	}
	for (AActor* PartnerActor : DisconnectedPartnerActors)
	{
		if (IsValid(PartnerActor))
		{
			NotifyHierarchyAssemblyChanged(PartnerActor, OwnerActor, false);
		}
	}
	NotifyHierarchyAssemblyChanged(OwnerActor, OwnerActor, false);
}

bool USnapControllerComponent::HasOccupiedSockets() const
{
	if (!GetOwner())
	{
		return false;
	}

	TArray<USnapSocketComponent*> MySockets;
	GetOwner()->GetComponents(MySockets);
	for (USnapSocketComponent* Socket : MySockets)
	{
		if (Socket && Socket->bIsOccupied)
		{
			return true;
		}
	}
	return false;
}

TArray<AActor*> USnapControllerComponent::GetAllConnectedAssemblyParts() const
{
	TArray<AActor*> Result;
	AActor* MyOwner = GetOwner();
	if (!MyOwner)
	{
		return Result;
	}

	TArray<AActor*> Frontier;
	Frontier.Add(MyOwner);
	Result.Add(MyOwner);

	while (Frontier.Num() > 0)
	{
		AActor* Current = Frontier.Pop();
		if (!Current) continue;

		TArray<USnapSocketComponent*> Sockets;
		Current->GetComponents(Sockets);
		for (USnapSocketComponent* Socket : Sockets)
		{
			if (Socket && Socket->bIsOccupied && Socket->ConnectedComponent.IsValid())
			{
				if (AActor* Neighbor = Socket->ConnectedComponent->GetOwner())
				{
					if (!Result.Contains(Neighbor))
					{
						Result.Add(Neighbor);
						Frontier.Add(Neighbor);
					}
				}
			}
		}
	}

	return Result;
}

AActor* USnapControllerComponent::GetAssemblyRoot() const
{
	AActor* Current = GetOwner();
	while (Current && Current->GetAttachParentActor())
	{
		AActor* Parent = Current->GetAttachParentActor();
		if (Parent->IsA<APawn>() || Parent->IsA<AController>())
		{
			break;
		}
		Current = Parent;
	}
	return Current;
}

bool USnapControllerComponent::IsAnotherAssemblyPartHeld() const
{
	const TArray<AActor*> AllParts = GetAllConnectedAssemblyParts();
	for (AActor* Part : AllParts)
	{
		if (Part && Part != GetOwner())
		{
			if (USnapControllerComponent* Ctrl = Part->FindComponentByClass<USnapControllerComponent>())
			{
				if (Ctrl->bIsHeldState)
				{
					return true;
				}
			}
		}
	}
	return false;
}

AActor* USnapControllerComponent::GetAnchoredBasePartInAssembly() const
{
	const TArray<AActor*> AllParts = GetAllConnectedAssemblyParts();
	for (AActor* Part : AllParts)
	{
		if (IsValid(Part) && Part != GetOwner())
		{
			const USnapControllerComponent* Ctrl = Part->FindComponentByClass<USnapControllerComponent>();
			if (!Ctrl || Ctrl->SnapRole == ESnapRole::AlwaysBase)
			{
				return Part;
			}
		}
	}
	return nullptr;
}

bool USnapControllerComponent::HasAnchoredBasePartInAssembly() const
{
	return GetAnchoredBasePartInAssembly() != nullptr;
}

FName USnapControllerComponent::DetectHoldingHand() const
{
	UActorComponent* ActiveGrabComp = GetActiveGrabComponent();
	if (!ActiveGrabComp)
	{
		ActiveGrabComp = CachedGrabComponent.Get();
	}
	if (!IsValid(ActiveGrabComp))
	{
		return NAME_None;
	}

	// 1. Try BP_GrabComponent's function "GetHeldByHand"
	UFunction* GetHeldByHandFunc = ActiveGrabComp->FindFunction(TEXT("GetHeldByHand"));
	if (GetHeldByHandFunc)
	{
		struct FGetHeldByHandParams
		{
			uint8 ReturnValue;
		};
		FGetHeldByHandParams Params{255};
		ActiveGrabComp->ProcessEvent(GetHeldByHandFunc, &Params);

		if (Params.ReturnValue == 0)
		{
			return FName(TEXT("Left"));
		}
		else if (Params.ReturnValue == 1)
		{
			return FName(TEXT("Right"));
		}
	}

	// 2. Check motionControllerRef
	if (const FObjectProperty* MotionControllerProp = CastField<FObjectProperty>(ActiveGrabComp->GetClass()->FindPropertyByName(TEXT("motionControllerRef"))))
	{
		if (UObject* MCObj = MotionControllerProp->GetObjectPropertyValue_InContainer(ActiveGrabComp))
		{
			if (const FNameProperty* SourceProp = CastField<FNameProperty>(MCObj->GetClass()->FindPropertyByName(TEXT("MotionSource"))))
			{
				const FName SourceVal = SourceProp->GetPropertyValue_InContainer(MCObj);
				if (!SourceVal.IsNone())
				{
					return SourceVal;
				}
			}

			const FString ObjName = MCObj->GetName();
			if (ObjName.Contains(TEXT("Left"), ESearchCase::IgnoreCase))
			{
				return FName(TEXT("Left"));
			}
			if (ObjName.Contains(TEXT("Right"), ESearchCase::IgnoreCase))
			{
				return FName(TEXT("Right"));
			}
		}
	}

	return NAME_None;
}

bool USnapControllerComponent::HasAssemblyComponentInHierarchy() const
{
	const TArray<AActor*> AllParts = GetAllConnectedAssemblyParts();
	for (AActor* Part : AllParts)
	{
		if (Part && Part->FindComponentByClass<USnapAssemblyComponent>())
		{
			return true;
		}
	}
	return false;
}

bool USnapControllerComponent::IsBaseRelativeTo(const AActor* OtherActor) const
{
	if (!OtherActor || OtherActor == GetOwner())
	{
		return false;
	}

	const USnapControllerComponent* OtherCtrl = OtherActor->FindComponentByClass<USnapControllerComponent>();

	// 1. Explicit Role overrides
	if (SnapRole == ESnapRole::AlwaysBase)
	{
		return true; // I am explicitly Base!
	}
	if (OtherCtrl && OtherCtrl->SnapRole == ESnapRole::AlwaysBase)
	{
		return false; // Other is explicitly Base!
	}
	if (SnapRole == ESnapRole::AlwaysDockingPart)
	{
		return false; // I am explicitly Docking Part (e.g. wing)
	}
	if (OtherCtrl && OtherCtrl->SnapRole == ESnapRole::AlwaysDockingPart)
	{
		return true; // Other is explicitly Docking Part -> I am Base!
	}

	// 2. Held vs Stationary priority: A stationary world actor is Base relative to an actor held in hand
	if (OtherCtrl)
	{
		if (!bIsHeldState && OtherCtrl->bIsHeldState)
		{
			return true; // I am stationary in the world, other is held in hand -> I am Base!
		}
		if (bIsHeldState && !OtherCtrl->bIsHeldState)
		{
			return false; // I am held in hand, other is stationary in the world -> Other is Base!
		}
	}

	// 3. USnapAssemblyComponent presence: the object with USnapAssemblyComponent is ALWAYS the Base (e.g. Rocket Body)!
	const bool bMyHasAssemblyComp = HasAssemblyComponentInHierarchy();
	const bool bOtherHasAssemblyComp = OtherCtrl ? OtherCtrl->HasAssemblyComponentInHierarchy() : (OtherActor->FindComponentByClass<USnapAssemblyComponent>() != nullptr);

	if (bMyHasAssemblyComp && !bOtherHasAssemblyComp)
	{
		return true; // I am (or part of) the main Assembly! I yield as Base!
	}
	if (bOtherHasAssemblyComp && !bMyHasAssemblyComp)
	{
		return false; // Other object is the main Assembly! It is Base, I am docking part!
	}

	// 4. Compare connected assembly part count: the bigger structure is the Base!
	const int32 MyCount = GetAllConnectedAssemblyParts().Num();
	const int32 OtherCount = OtherCtrl ? OtherCtrl->GetAllConnectedAssemblyParts().Num() : 1;

	if (MyCount > OtherCount)
	{
		return true; // I have more assembled parts, I am Base!
	}
	if (OtherCount > MyCount)
	{
		return false; // Other has more assembled parts, it is Base!
	}

	// 5. Structural Socket Count: An actor with more sockets is the Host/Base!
	// (e.g. Rocket Body has 6 sockets, Wing has 1 socket -> Rocket Body is definitively Base!)
	TArray<USnapSocketComponent*> MySockets;
	if (GetOwner())
	{
		GetOwner()->GetComponents(MySockets);
	}
	TArray<USnapSocketComponent*> OtherSockets;
	OtherActor->GetComponents(OtherSockets);

	if (MySockets.Num() > OtherSockets.Num())
	{
		return true;
	}
	if (OtherSockets.Num() > MySockets.Num())
	{
		return false;
	}

	// 6. Two-handed VR resolution when identical parts are held in hands
	if (OtherCtrl && OtherCtrl->bIsHeldState && bIsHeldState)
	{
		const bool bMyLeft = HoldingHandSource.ToString().Contains(TEXT("Left"), ESearchCase::IgnoreCase);
		const bool bOtherLeft = OtherCtrl->HoldingHandSource.ToString().Contains(TEXT("Left"), ESearchCase::IgnoreCase);
		const bool bMyRight = HoldingHandSource.ToString().Contains(TEXT("Right"), ESearchCase::IgnoreCase);
		const bool bOtherRight = OtherCtrl->HoldingHandSource.ToString().Contains(TEXT("Right"), ESearchCase::IgnoreCase);

		switch (DualHandPriority)
		{
		case EDualHandPriorityMode::RightHandBase:
			if (bMyRight && !bOtherRight) return true;
			if (bOtherRight && !bMyRight) return false;
			break;
		case EDualHandPriorityMode::AssemblyComponentPriority:
		case EDualHandPriorityMode::LeftHandBase:
		default:
			if (bMyLeft && !bOtherLeft) return true;
			if (bOtherLeft && !bMyLeft) return false;
			break;
		}

		return GrabTimestamp < OtherCtrl->GrabTimestamp;
	}

	return false;
}

void USnapControllerComponent::GatherCandidateSlots(TArray<FSnapCandidateSlot>& OutCandidates)
{
	OutCandidates.Reset();

	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !GetWorld())
	{
		return;
	}

	// Explicit AlwaysBase actors never dock into other hosts and never project ghosts
	if (SnapRole == ESnapRole::AlwaysBase)
	{
		return;
	}

	// If I am not held and not allowed world snapping, do not scan
	if (!bIsHeldState && !bAllowWorldSnapWithoutGrab)
	{
		return;
	}

	// Connected assembly parts to ignore as external targets
	const TArray<AActor*> MyAssemblyParts = GetAllConnectedAssemblyParts();

	// 1. Gather unoccupied snap sockets on this held actor (and across attached assembly parts when holding the sub-assembly).
	// Because we strictly filter MyRole != Receiver below, ONLY unoccupied Plug (or Bidirectional) sockets on our held assembly
	// will ever project ghosts onto compatible Receiver sockets, and each ghost uses HeldSocket->GetOwner()'s mesh!
	TArray<USnapSocketComponent*> MySockets;
	if (bIsHeldState && bGrabAttachedPartPicksUpAssembly && MyAssemblyParts.Num() > 1)
	{
		for (AActor* AssemblyPart : MyAssemblyParts)
		{
			if (IsValid(AssemblyPart))
			{
				TArray<USnapSocketComponent*> PartSockets;
				AssemblyPart->GetComponents(PartSockets);
				for (USnapSocketComponent* S : PartSockets)
				{
					if (IsValid(S) && !S->bIsOccupied)
					{
						MySockets.AddUnique(S);
					}
				}
			}
		}
	}
	else
	{
		OwnerActor->GetComponents(MySockets);
	}

	if (MySockets.Num() == 0)
	{
		return;
	}

	const float MaxDistSquared = FMath::Square(GhostPreviewMaxDistance);
	UWorld* CurrentWorld = GetWorld();

	// 2. Scan active snap sockets from the global registry
	const TArray<TWeakObjectPtr<USnapSocketComponent>>& AllSockets = USnapSocketComponent::GetAllSnapSockets();
	for (const TWeakObjectPtr<USnapSocketComponent>& SocketPtr : AllSockets)
	{
		USnapSocketComponent* OtherSocket = SocketPtr.Get();
		if (!IsValid(OtherSocket) || OtherSocket->bIsOccupied)
		{
			continue;
		}

		// Multi-world filtering (prevents cross-world / PIE leaks)
		if (OtherSocket->GetWorld() != CurrentWorld)
		{
			continue;
		}

		AActor* OtherActor = OtherSocket->GetOwner();
		if (!IsValid(OtherActor) || OtherActor == OwnerActor)
		{
			continue;
		}

		// Skip actors that are already part of our connected assembly
		if (MyAssemblyParts.Contains(OtherActor))
		{
			continue;
		}

		const USnapControllerComponent* OtherCtrl = OtherActor->FindComponentByClass<USnapControllerComponent>();

		// Ghosts and candidates MUST ONLY appear on Receiver target sockets (OtherRole != Plug)
		// matching the Plug socket type(s) on the part/assembly held in our hands (MyRole != Receiver).
		// HIGH-PERFORMANCE EARLY DISTANCE REJECT:
		const FVector OtherLoc = OtherSocket->GetComponentLocation();
		bool bAnyInRangeAndCompatible = false;
		for (const USnapSocketComponent* MySocket : MySockets)
		{
			if (IsValid(MySocket) && !MySocket->bIsOccupied)
			{
				const ESocketRole MyRole = MySocket->GetEffectiveRoleFor(OtherSocket);
				const ESocketRole OtherRole = OtherSocket->GetEffectiveRoleFor(MySocket);
				if (MyRole != ESocketRole::Receiver && OtherRole != ESocketRole::Plug)
				{
					if (FVector::DistSquared(MySocket->GetComponentLocation(), OtherLoc) <= MaxDistSquared)
					{
						if (MySocket->IsCompatibleWith(OtherSocket))
						{
							bAnyInRangeAndCompatible = true;
							break;
						}
					}
				}
			}
		}

		if (!bAnyInRangeAndCompatible)
		{
			continue;
		}

		// Find the best matching MySocket for this OtherSocket
		float BestMySocketScore = MAX_flt;
		USnapSocketComponent* BestMySocket = nullptr;
		float BestMyDist = 0.0f;
		bool bBestCanSnap = false;

		for (USnapSocketComponent* MySocket : MySockets)
		{
			if (!IsValid(MySocket) || MySocket->bIsOccupied)
			{
				continue;
			}

			const ESocketRole MyRole = MySocket->GetEffectiveRoleFor(OtherSocket);
			const ESocketRole OtherRole = OtherSocket->GetEffectiveRoleFor(MySocket);

			// Strictly require held socket to be Plug (or Bidirectional) and target socket to be Receiver (or Bidirectional)
			if (MyRole == ESocketRole::Receiver || OtherRole == ESocketRole::Plug)
			{
				continue;
			}

			if (!MySocket->IsCompatibleWith(OtherSocket))
			{
				continue;
			}

			// Polarity resolution when both sockets are Bidirectional:
			if (MyRole == ESocketRole::Bidirectional && OtherRole == ESocketRole::Bidirectional)
			{
				if (OtherCtrl && OtherCtrl->SnapRole == ESnapRole::AlwaysDockingPart)
				{
					continue;
				}
				if (IsBaseRelativeTo(OtherActor))
				{
					continue; // This actor is Base for this bidirectional connection; yield to OtherActor
				}
			}

			const float Dist = FVector::Dist(MySocket->GetComponentLocation(), OtherSocket->GetComponentLocation());
			if (Dist > GhostPreviewMaxDistance)
			{
				continue;
			}

			// Forward Vector alignment:
			const float Dot = FVector::DotProduct(MySocket->GetForwardVector(), OtherSocket->GetForwardVector());

			// Prefer sockets on OwnerActor slightly if tied, closer sockets, and sockets facing more directly opposing (-1.0)
			const float FacingFactor = (Dot + 1.0f);
			const float OwnerBonus = (MySocket->GetOwner() == OwnerActor) ? 0.0f : 1.0f;
			float Score = Dist + FacingFactor * 12.0f + OwnerBonus;

			const float EffectiveSnapRadius = FMath::Max(MySocket->SnapRadius, OtherSocket->SnapRadius);

			// Jitter-suppression hysteresis (only when in close range of the previous target):
			if (OtherSocket == LastTargetSocket.Get() && Dist <= (EffectiveSnapRadius * 1.5f))
			{
				Score -= 3.5f;
			}

			if (Score < BestMySocketScore)
			{
				BestMySocketScore = Score;
				BestMySocket = MySocket;
				BestMyDist = Dist;
				bBestCanSnap = (Dist <= EffectiveSnapRadius);
			}
		}

		if (BestMySocket)
		{
			AActor* SocketOwnerActor = BestMySocket->GetOwner() ? BestMySocket->GetOwner() : OwnerActor;
			FSnapCandidateSlot Slot;
			Slot.HeldSocket = BestMySocket;
			Slot.TargetSocket = OtherSocket;
			Slot.TargetTransform = BestMySocket->CalculateDockingActorTransformForActor(OtherSocket, OwnerActor, true);
			Slot.GhostTransform = (SocketOwnerActor == OwnerActor)
				? Slot.TargetTransform
				: BestMySocket->CalculateDockingActorTransformForActor(OtherSocket, SocketOwnerActor, true);
			Slot.Distance = BestMyDist;
			Slot.Score = BestMySocketScore;
			Slot.bCanSnap = bBestCanSnap;
			OutCandidates.Add(Slot);
		}
	}

	// Sort candidates: absolute priority to sockets that are ready to snap (bCanSnap == true), then tie-break by score
	OutCandidates.Sort([](const FSnapCandidateSlot& A, const FSnapCandidateSlot& B)
	{
		if (A.bCanSnap != B.bCanSnap)
		{
			return A.bCanSnap;
		}
		return A.Score < B.Score;
	});
}

void USnapControllerComponent::UpdateCandidateTracking()
{
	const bool bHadCandidateBefore = bHasCandidate;

	TArray<FSnapCandidateSlot> Candidates;
	GatherCandidateSlots(Candidates);

	if (Candidates.Num() > 0)
	{
		const FSnapCandidateSlot& Best = Candidates[0];

		CandidateHeldSocket = Best.HeldSocket;
		CandidateTargetSocket = Best.TargetSocket;
		LastTargetSocket = Best.TargetSocket;
		DesiredSnapTransform = Best.TargetTransform;
		bCanSnap = Best.bCanSnap;
		bHasCandidate = true;

		if (!bHadCandidateBefore && CandidateFoundSound && GetWorld() && Best.TargetSocket)
		{
			UGameplayStatics::PlaySoundAtLocation(GetWorld(), CandidateFoundSound, Best.TargetSocket->GetComponentLocation());
		}

		if (bShowGhostPreview)
		{
			const int32 NumToShow = bShowAllAvailableGhosts
				? FMath::Min(Candidates.Num(), MaxVisibleGhosts)
				: 1;

			for (int32 i = 0; i < NumToShow; ++i)
			{
				const FSnapCandidateSlot& Slot = Candidates[i];
				AStaticMeshActor* Ghost = GetOrCreateGhostActor(i);
				if (!Ghost)
				{
					continue;
				}

				// The best/active candidate (index 0) turns GREEN when aligned within SnapRadius and tolerance!
				// All other available sockets (or active when approaching) remain BLUE / CYAN.
				UMaterialInterface* MatToUse = (i == 0 && Slot.bCanSnap)
					? GetGhostMaterialReady()
					: GetGhostMaterialAvailable();

				AActor* GhostSourceActor = (Slot.HeldSocket && Slot.HeldSocket->GetOwner()) ? Slot.HeldSocket->GetOwner() : GetOwner();
				UpdateGhostMeshAndMaterials(Ghost, MatToUse, GhostSourceActor);

				FTransform GhostTransform = Slot.GhostTransform;
				if (i == 0 && bEnableGhostProximityScale && Slot.bCanSnap && IsValid(Slot.TargetSocket))
				{
					const float ApproachDist = FMath::Max(GhostProximityApproachDistance, 20.0f);
					const float Alpha = FMath::Clamp(Slot.Distance / ApproachDist, 0.0f, 1.0f);
					const float SmoothAlpha = FMath::InterpEaseInOut(0.0f, 1.0f, Alpha, 2.0f);
					const float ScaleMultiplier = FMath::Lerp(GhostMinProximityScale, 1.0f, SmoothAlpha);

					// Anchor proximity scaling around the target snap socket location so the ghost socket never drifts away from the target socket
					const FVector TargetSocketLoc = Slot.TargetSocket->GetComponentLocation();
					GhostTransform.SetLocation(TargetSocketLoc + (Slot.GhostTransform.GetLocation() - TargetSocketLoc) * ScaleMultiplier);
					GhostTransform.SetScale3D(Slot.GhostTransform.GetScale3D() * ScaleMultiplier);
				}

				SetGhostActorTransform(Ghost, GhostTransform, GhostSourceActor);
				Ghost->SetActorHiddenInGame(false);
			}

			// Hide any excess ghost meshes in the pool
			for (int32 j = NumToShow; j < GhostActorsPool.Num(); ++j)
			{
				if (GhostActorsPool[j])
				{
					GhostActorsPool[j]->SetActorHiddenInGame(true);
				}
			}
		}
		else
		{
			HideAllGhosts();
		}

		OnSnapCandidateChanged.Broadcast(true, DesiredSnapTransform);
	}
	else
	{
		HideAllGhosts();

		if (bHasCandidate)
		{
			bHasCandidate = false;
			bCanSnap = false;
			CandidateHeldSocket = nullptr;
			CandidateTargetSocket = nullptr;
			LastTargetSocket = nullptr;
			OnSnapCandidateChanged.Broadcast(false, FTransform::Identity);
		}
	}
}

bool USnapControllerComponent::ScanForBestCandidate(USnapSocketComponent*& OutHeldSocket, USnapSocketComponent*& OutTargetSocket, FTransform& OutTargetTransform)
{
	TArray<FSnapCandidateSlot> Candidates;
	GatherCandidateSlots(Candidates);

	if (Candidates.Num() > 0)
	{
		OutHeldSocket = Candidates[0].HeldSocket;
		OutTargetSocket = Candidates[0].TargetSocket;
		OutTargetTransform = Candidates[0].TargetTransform;
		LastTargetSocket = Candidates[0].TargetSocket;
		return true;
	}

	LastTargetSocket = nullptr;
	return false;
}

void USnapControllerComponent::UpdateInterpolation(float DeltaTime)
{
	AActor* OwnerActor = GetOwner();
	if (!IsValid(OwnerActor))
	{
		bIsInterpolating = false;
		return;
	}

	// Verify target sockets remain valid throughout interpolation
	if (!IsValid(CandidateHeldSocket.Get()) || !IsValid(CandidateTargetSocket.Get()))
	{
		bIsInterpolating = false;
		CandidateHeldSocket = nullptr;
		CandidateTargetSocket = nullptr;
		LastTargetSocket = nullptr;

		// Re-enable physics simulation if target vanished during interpolation
		SetActorPhysicsCollisionState(OwnerActor, true);
		return;
	}

	InterpElapsed += DeltaTime;
	const float Duration = FMath::Max(SnapDuration, 0.02f);
	const float Alpha = FMath::Clamp(InterpElapsed / Duration, 0.0f, 1.0f);

	// Dynamically update DesiredSnapTransform in case target socket is moving in player's other hand
	DesiredSnapTransform = CandidateHeldSocket->CalculateDockingActorTransformForActor(CandidateTargetSocket, OwnerActor, true);

	// Smooth cubic ease-out: brisk start, gentle cushion into lock
	const float SmoothAlpha = 1.0f - FMath::Pow(1.0f - Alpha, 3.0f);

	const FVector CurLoc = FMath::Lerp(InterpStartTransform.GetLocation(), DesiredSnapTransform.GetLocation(), SmoothAlpha);
	const FQuat CurRot = FQuat::Slerp(InterpStartTransform.GetRotation(), DesiredSnapTransform.GetRotation(), SmoothAlpha);
	const FVector CurScale = FMath::Lerp(InterpStartTransform.GetScale3D(), DesiredSnapTransform.GetScale3D(), SmoothAlpha);

	const FTransform CurTransform(CurRot, CurLoc, CurScale);
	SetActorPhysicsCollisionState(OwnerActor, false);
	OwnerActor->SetActorTransform(CurTransform, false, nullptr, ETeleportType::TeleportPhysics);

	// Keep any pre-attached sub-assembly child parts locked to OwnerActor's sockets throughout interpolation
	SyncConnectedSubAssemblyHierarchy(true);

	if (Alpha >= 1.0f)
	{
		FinishSnap();
	}
}

void USnapControllerComponent::FinishSnap()
{
	AActor* OwnerActor = GetOwner();
	if (!IsValid(OwnerActor) || !IsValid(CandidateHeldSocket.Get()) || !IsValid(CandidateTargetSocket.Get()))
	{
		bIsInterpolating = false;
		CandidateHeldSocket = nullptr;
		CandidateTargetSocket = nullptr;
		LastTargetSocket = nullptr;
		SetActorPhysicsCollisionState(OwnerActor, true);
		return;
	}

	// Recalculate exact docking transform right at the moment of snapping to ensure perfect flush alignment
	DesiredSnapTransform = CandidateHeldSocket->CalculateDockingActorTransformForActor(CandidateTargetSocket, OwnerActor, true);
	OwnerActor->SetActorTransform(DesiredSnapTransform, false, nullptr, ETeleportType::TeleportPhysics);

	// Realign all pre-existing sub-assembly children on OwnerActor before attaching to TargetActor
	SyncConnectedSubAssemblyHierarchy(true);

	AActor* HeldSocketOwner = CandidateHeldSocket->GetOwner() ? CandidateHeldSocket->GetOwner() : OwnerActor;
	AActor* TargetActor = CandidateTargetSocket->GetOwner();

	if (IsValid(TargetActor))
	{
		// Disable physics simulation and switch docked child to QueryOnly collision so it never kicks TargetActor's physics body
		SetActorPhysicsCollisionState(OwnerActor, false);
		SetActorPhysicsCollisionState(HeldSocketOwner, false);

		AActor* TargetRoot = TargetActor->GetRootComponent() ? TargetActor->GetRootComponent()->GetAttachmentRootActor() : TargetActor;
		if (TargetRoot == OwnerActor || TargetRoot == HeldSocketOwner)
		{
			TargetActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}

		if (HeldSocketOwner != OwnerActor && HeldSocketOwner->GetAttachParentActor() == OwnerActor)
		{
			HeldSocketOwner->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}

		const FVector SavedHeldOwnerScale = HeldSocketOwner->GetActorScale3D();
		HeldSocketOwner->AttachToActor(TargetActor, FAttachmentTransformRules::KeepWorldTransform);

		// Ensure world scale is strictly preserved after attachment (SetWorldScale3D, NOT SetActorScale3D which sets relative scale!)
		if (USceneComponent* RootComp = HeldSocketOwner->GetRootComponent())
		{
			RootComp->SetWorldScale3D(SavedHeldOwnerScale);
		}

		if (USnapControllerComponent* TargetCtrl = TargetActor->FindComponentByClass<USnapControllerComponent>())
		{
			TargetCtrl->bIsSnapped = true;
		}
	}

	CandidateHeldSocket->SnapTo(CandidateTargetSocket);

	// Rebuild outward sub-assembly hierarchy from HeldSocketOwner so all connected parts (including OwnerActor if HeldSocketOwner != OwnerActor)
	// are properly parented under HeldSocketOwner -> TargetActor with exact world scales and roll angles!
	if (USnapControllerComponent* HeldSocketOwnerCtrl = HeldSocketOwner->FindComponentByClass<USnapControllerComponent>())
	{
		HeldSocketOwnerCtrl->SyncConnectedSubAssemblyHierarchy(true);
	}
	else
	{
		SyncConnectedSubAssemblyHierarchy(true);
	}

	if (SnapCompletedSound && GetWorld())
	{
		UGameplayStatics::PlaySoundAtLocation(GetWorld(), SnapCompletedSound, CandidateTargetSocket->GetComponentLocation());
	}

	bIsSnapped = true;
	bIsInterpolating = false;
	LastTargetSocket = nullptr;
	bIsHoldingWholeAssemblyByThisPart = false;
	HeldAssemblyParts.Empty();
	SavedPhysicsStates.Empty();

	// Cache local pointers for event broadcast before zeroing candidate members
	USnapSocketComponent* SnappedHeld = CandidateHeldSocket;
	USnapSocketComponent* SnappedTarget = CandidateTargetSocket;

	// Reset candidate pointers so strong UPROPERTY references are cleared upon dock
	CandidateHeldSocket = nullptr;
	CandidateTargetSocket = nullptr;

	OnSnapCompleted.Broadcast(SnappedHeld, SnappedTarget);

	// Notify any USnapAssemblyComponent in the assembled hierarchy
	NotifyHierarchyAssemblyChanged(OwnerActor, OwnerActor, true);
}

void USnapControllerComponent::NotifyHierarchyAssemblyChanged(AActor* InActor, AActor* ChangedPart, bool bAttached)
{
	if (!InActor)
	{
		return;
	}

	AActor* Current = InActor;
	while (Current && Current->GetAttachParentActor())
	{
		AActor* Parent = Current->GetAttachParentActor();
		if (Parent->IsA<APawn>() || Parent->IsA<AController>())
		{
			break;
		}
		Current = Parent;
	}
	AActor* RootActor = Current;

	TArray<AActor*> AllActors;
	AllActors.Add(RootActor);
	RootActor->GetAttachedActors(AllActors, false, true);

	for (AActor* Act : AllActors)
	{
		if (Act)
		{
			TArray<USnapAssemblyComponent*> AssemblyComps;
			Act->GetComponents(AssemblyComps);
			for (USnapAssemblyComponent* Comp : AssemblyComps)
			{
				if (Comp)
				{
					Comp->NotifyAssemblyChanged(ChangedPart, bAttached);
				}
			}
		}
	}
}

UPrimitiveComponent* USnapControllerComponent::GetOwnerPhysicsComponent() const
{
	if (AActor* Owner = GetOwner())
	{
		if (UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Owner->GetRootComponent()))
		{
			return RootPrim;
		}
		// Fallback: If RootComponent is USceneComponent (e.g. DefaultSceneRoot), find the main primitive component!
		return Owner->FindComponentByClass<UPrimitiveComponent>();
	}
	return nullptr;
}

UActorComponent* USnapControllerComponent::GetActiveGrabComponent() const
{
	for (const TWeakObjectPtr<UActorComponent>& CompPtr : CachedGrabComponents)
	{
		UActorComponent* Comp = CompPtr.Get();
		if (!IsValid(Comp))
		{
			continue;
		}
		if (const FBoolProperty* IsHeldProp = CastField<FBoolProperty>(Comp->GetClass()->FindPropertyByName(TEXT("bIsHeld"))))
		{
			if (IsHeldProp->GetPropertyValue_InContainer(Comp))
			{
				return Comp;
			}
		}
	}

	if (CachedGrabComponent.IsValid())
	{
		UActorComponent* Comp = CachedGrabComponent.Get();
		if (const FBoolProperty* IsHeldProp = CastField<FBoolProperty>(Comp->GetClass()->FindPropertyByName(TEXT("bIsHeld"))))
		{
			if (IsHeldProp->GetPropertyValue_InContainer(Comp))
			{
				return Comp;
			}
		}
	}

	return nullptr;
}

bool USnapControllerComponent::CheckOwnerIsHeld() const
{
	return GetActiveGrabComponent() != nullptr;
}
