#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Pawn.h"
#include "Components/StaticMeshComponent.h"
#include "SnapSocketComponent.h"
#include "SnapControllerComponent.h"
#include "SnapAssemblyComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PointSnapTestHelpers
{
	static UWorld* CreateTestWorld()
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, FName(TEXT("PointSnapTestWorld")));
		if (World && GEngine)
		{
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
		}
		return World;
	}

	static void DestroyTestWorld(UWorld* World)
	{
		if (World && GEngine)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	}

	static USnapSocketComponent* FindSocketByName(AActor* Actor, const FString& NamePart)
	{
		if (!Actor)
		{
			return nullptr;
		}
		TArray<USnapSocketComponent*> Sockets;
		Actor->GetComponents(Sockets);
		for (USnapSocketComponent* S : Sockets)
		{
			if (S && S->GetName().Contains(NamePart))
			{
				return S;
			}
		}
		return nullptr;
	}

	static AStaticMeshActor* SpawnTestPart(UWorld* World, const FVector& Location, const FVector& WorldScale, bool bSimulatePhysics = true)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FRotator::ZeroRotator, Location, FVector::OneVector));
		if (Actor && Actor->GetStaticMeshComponent())
		{
			Actor->SetMobility(EComponentMobility::Movable);
			UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (CubeMesh)
			{
				Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
			}
			Actor->GetStaticMeshComponent()->SetWorldScale3D(WorldScale);
			Actor->GetStaticMeshComponent()->SetSimulatePhysics(bSimulatePhysics);
		}
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPointSnapSocketTransformAndGhostTest,
	"PointSnap.Math.SocketTransformAndGhostPositioning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FPointSnapSocketTransformAndGhostTest::RunTest(const FString& Parameters)
{
	UWorld* World = PointSnapTestHelpers::CreateTestWorld();
	TestNotNull(TEXT("Test world created"), World);
	if (!World)
	{
		return false;
	}

	// 1. Self-contained programmatic test with non-uniform scale (0.15, 0.15, 0.40) and +-90 deg pitched sockets
	{
		AStaticMeshActor* BaseEngine = PointSnapTestHelpers::SpawnTestPart(World, FVector::ZeroVector, FVector(0.15f, 0.15f, 0.15f), true);
		USnapSocketComponent* BaseSocket = NewObject<USnapSocketComponent>(BaseEngine, TEXT("SnapSocket"));
		BaseSocket->SetupAttachment(BaseEngine->GetRootComponent());
		BaseSocket->SetRelativeLocation(FVector(0.0f, 0.0f, 66.666667f));
		BaseSocket->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
		BaseSocket->SetRelativeScale3D(FVector(40.0f, 40.0f, 40.0f));
		BaseSocket->SocketRole = ESocketRole::Receiver;
		BaseSocket->SnapType = FName(TEXT("Engine"));
		BaseSocket->RegisterComponent();

		AStaticMeshActor* OilBase = PointSnapTestHelpers::SpawnTestPart(World, FVector(0.0f, 0.0f, 35.0f), FVector(0.15f, 0.15f, 0.40f), true);
		USnapSocketComponent* OilEngineSocket = NewObject<USnapSocketComponent>(OilBase, TEXT("EngineSocket"));
		OilEngineSocket->SetupAttachment(OilBase->GetRootComponent());
		OilEngineSocket->SetRelativeLocation(FVector(2.314585f, 0.0f, -52.423965f));
		OilEngineSocket->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
		OilEngineSocket->SocketRole = ESocketRole::Plug;
		OilEngineSocket->SnapType = FName(TEXT("Engine"));
		OilEngineSocket->RegisterComponent();

		USnapControllerComponent* OilCtrl = NewObject<USnapControllerComponent>(OilBase, TEXT("SnapController"));
		OilCtrl->RegisterComponent();

		const FVector TargetSocketWorldLoc = BaseSocket->GetComponentLocation();
		const FTransform DockTransform = OilEngineSocket->CalculateDockingActorTransform(BaseSocket);

		// Base socket is at Z = 10.0, OilBase socket is 20.97 cm below OilBase center (-52.424 * 0.40 = -20.970).
		// Therefore, OilBase center MUST be at Z = 10.0 + 20.970 = 30.970 cm (NOT sunken at 10.0 or 17.86 cm!).
		TestTrue(
			FString::Printf(TEXT("Programmatic OilBase docked Z (%.3f) is ~30.97 cm, not sunken at Engine socket"), DockTransform.GetLocation().Z),
			FMath::IsNearlyEqual(DockTransform.GetLocation().Z, 30.97f, 0.1f)
		);

		OilCtrl->NotifyGrabbed();
		OilCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);

		TestTrue(TEXT("Programmatic OilBase found Engine candidate"), OilCtrl->bHasCandidate && OilCtrl->bCanSnap);
		TestTrue(
			TEXT("DesiredSnapTransform matches CalculateDockingActorTransform"),
			OilCtrl->DesiredSnapTransform.GetLocation().Equals(DockTransform.GetLocation(), 0.05f)
		);

		OilBase->SetActorTransform(DockTransform);
		const float SocketDistAfterDock = FVector::Dist(OilEngineSocket->GetComponentLocation(), TargetSocketWorldLoc);
		const float ForwardDot = FVector::DotProduct(OilEngineSocket->GetForwardVector(), BaseSocket->GetForwardVector());

		TestTrue(TEXT("Sockets meet flush after docking"), SocketDistAfterDock < 0.05f);
		TestTrue(TEXT("Sockets face directly opposite after docking"), ForwardDot <= -0.999f);

		// Verify RollMode::Stepped (4 steps = 90 deg increments) and RollMode::Free360
		BaseSocket->RollMode = ESocketRollMode::Stepped;
		BaseSocket->RollSnapSteps = 4;
		OilBase->SetActorRotation(FRotator(0.0f, 80.0f, 0.0f)); // ~80 deg yaw around vertical Z (socket X axis)
		const FTransform SteppedTransform = OilEngineSocket->CalculateDockingActorTransform(BaseSocket);
		const float SteppedYaw = FMath::UnwindDegrees(SteppedTransform.GetRotation().Rotator().Yaw);
		TestTrue(
			FString::Printf(TEXT("Stepped RollMode (4 steps) snaps 80 deg yaw to nearest 90 deg step (Actual=%.2f)"), FMath::Abs(SteppedYaw)),
			FMath::IsNearlyEqual(FMath::Abs(SteppedYaw), 90.0f, 1.0f)
		);

		BaseSocket->RollMode = ESocketRollMode::Free360;
		OilBase->SetActorRotation(FRotator(0.0f, 37.0f, 0.0f));
		const FTransform FreeRollTransform = OilEngineSocket->CalculateDockingActorTransform(BaseSocket);
		const FVector CurrentSocketUp = OilEngineSocket->GetComponentQuat().GetAxisZ();
		OilBase->SetActorTransform(FreeRollTransform);
		const FVector DockedSocketUp = OilEngineSocket->GetComponentQuat().GetAxisZ();
		TestTrue(
			TEXT("Free360 RollMode preserves arbitrary 37 deg roll orientation without twisting"),
			FVector::DotProduct(CurrentSocketUp, DockedSocketUp) >= 0.999f
		);

		// Verify holding the Plug rotated at any wrist angle in hand STILL shows the ghost (does NOT disappear when rotated!)
		OilBase->SetActorLocationAndRotation(FVector(0.0f, 0.0f, 35.0f), FRotator(180.0f, 0.0f, 0.0f));
		OilCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
		TestTrue(TEXT("Holding Plug rotated 180 deg in hand at 46cm socket distance still shows blue ghost"), OilCtrl->bHasCandidate);

		OilBase->SetActorLocationAndRotation(FVector(0.0f, 0.0f, 0.0f), FRotator(180.0f, 0.0f, 0.0f));
		OilCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
		TestTrue(TEXT("Holding Plug rotated 180 deg in hand within SnapRadius shows green ghost and can snap"), OilCtrl->bHasCandidate && OilCtrl->bCanSnap);

		// Verify two-handed VR hold: when BOTH BaseEngine and OilBase are attached to the same APawn (VRPawn left & right controllers),
		// the ghost does NOT disappear!
		APawn* DummyVRPawn = World->SpawnActor<APawn>(APawn::StaticClass(), FTransform::Identity);
		if (DummyVRPawn)
		{
			USceneComponent* PawnRoot = NewObject<USceneComponent>(DummyVRPawn, TEXT("VROrigin"));
			DummyVRPawn->SetRootComponent(PawnRoot);
			PawnRoot->RegisterComponent();

			BaseEngine->AttachToComponent(PawnRoot, FAttachmentTransformRules::KeepWorldTransform);
			OilBase->SetActorLocationAndRotation(FVector(0.0f, 0.0f, 35.0f), FRotator::ZeroRotator);
			OilBase->AttachToComponent(PawnRoot, FAttachmentTransformRules::KeepWorldTransform);

			OilCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
			TestTrue(TEXT("Ghost remains visible when both parts are held in hands of the same VRPawn"), OilCtrl->bHasCandidate && OilCtrl->bCanSnap);

			BaseEngine->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			OilBase->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			DummyVRPawn->Destroy();
		}

		// Verify holding the Receiver (BaseEngine) in hand near a Plug (OilBase) does NOT project an unwanted ghost onto the Plug!
		OilCtrl->NotifyDropped();
		OilBase->SetActorLocationAndRotation(FVector(0.0f, 0.0f, 35.0f), FRotator::ZeroRotator);
		USnapControllerComponent* BaseEngineCtrl = NewObject<USnapControllerComponent>(BaseEngine, TEXT("BaseEngineCtrl"));
		BaseEngineCtrl->RegisterComponent();
		BaseEngineCtrl->NotifyGrabbed();
		BaseEngineCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
		TestFalse(TEXT("Holding Receiver part near Plug part does NOT project unwanted ghost onto Plug"), BaseEngineCtrl->bHasCandidate);

		BaseEngine->Destroy();
		OilBase->Destroy();
	}

	// 2. If project Blueprints BP_Engine and BP_OilBase are present, verify them directly as well
	UClass* EngineBPClass = StaticLoadClass(AActor::StaticClass(), nullptr, TEXT("/Game/Blueprints/BP_Engine.BP_Engine_C"), nullptr, LOAD_NoWarn, nullptr);
	UClass* OilBaseBPClass = StaticLoadClass(AActor::StaticClass(), nullptr, TEXT("/Game/Blueprints/BP_OilBase.BP_OilBase_C"), nullptr, LOAD_NoWarn, nullptr);

	if (EngineBPClass && OilBaseBPClass)
	{
		AActor* EngineActor = World->SpawnActor<AActor>(EngineBPClass, FTransform(FRotator::ZeroRotator, FVector::ZeroVector));
		AActor* OilBaseActor = World->SpawnActor<AActor>(OilBaseBPClass, FTransform(FRotator::ZeroRotator, FVector(0.0f, 0.0f, 35.0f)));

		USnapSocketComponent* EngineSocketOnEngine = PointSnapTestHelpers::FindSocketByName(EngineActor, TEXT("SnapSocket"));
		USnapSocketComponent* EngineSocketOnOilBase = PointSnapTestHelpers::FindSocketByName(OilBaseActor, TEXT("EngineSocket"));

		if (EngineSocketOnEngine && EngineSocketOnOilBase)
		{
			const FTransform DockTransform = EngineSocketOnOilBase->CalculateDockingActorTransform(EngineSocketOnEngine);
			TestTrue(
				FString::Printf(TEXT("BP_OilBase docked Z (%.3f) is ~30.97 cm"), DockTransform.GetLocation().Z),
				FMath::IsNearlyEqual(DockTransform.GetLocation().Z, 30.97f, 0.1f)
			);

			if (USnapControllerComponent* OilBaseBPCtrl = OilBaseActor->FindComponentByClass<USnapControllerComponent>())
			{
				OilBaseBPCtrl->NotifyGrabbed();
				OilBaseBPCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
				TestTrue(TEXT("BP_OilBase Blueprint shows ghost on BP_Engine when grabbed in hand"), OilBaseBPCtrl->bHasCandidate && OilBaseBPCtrl->bCanSnap);
			}
		}
	}

	PointSnapTestHelpers::DestroyTestWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPointSnapSubAssemblyDockingTest,
	"PointSnap.Assembly.SubAssemblyDocking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FPointSnapSubAssemblyDockingTest::RunTest(const FString& Parameters)
{
	UWorld* World = PointSnapTestHelpers::CreateTestWorld();
	TestNotNull(TEXT("Test world created"), World);
	if (!World)
	{
		return false;
	}

	// Self-contained programmatic multi-part assembly test (Engine + OilBase + Stab + Nose with non-uniform scales)
	AStaticMeshActor* EngineActor = PointSnapTestHelpers::SpawnTestPart(World, FVector::ZeroVector, FVector(0.15f, 0.15f, 0.15f), true);
	USnapSocketComponent* EngineSocketOnEngine = NewObject<USnapSocketComponent>(EngineActor, TEXT("SnapSocket"));
	EngineSocketOnEngine->SetupAttachment(EngineActor->GetRootComponent());
	EngineSocketOnEngine->SetRelativeLocation(FVector(0.0f, 0.0f, 66.666667f));
	EngineSocketOnEngine->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
	EngineSocketOnEngine->SocketRole = ESocketRole::Receiver;
	EngineSocketOnEngine->SnapType = FName(TEXT("Engine"));
	EngineSocketOnEngine->RegisterComponent();

	USnapControllerComponent* EngineCtrl = NewObject<USnapControllerComponent>(EngineActor, TEXT("EngineCtrl"));
	EngineCtrl->RegisterComponent();
	USnapAssemblyComponent* EngineAssembly = NewObject<USnapAssemblyComponent>(EngineActor, TEXT("EngineAssembly"));
	EngineAssembly->RegisterComponent();

	AStaticMeshActor* OilBaseActor = PointSnapTestHelpers::SpawnTestPart(World, FVector(0.0f, 0.0f, 200.0f), FVector(0.15f, 0.15f, 0.40f), true);
	USnapSocketComponent* EngineSocketOnOilBase = NewObject<USnapSocketComponent>(OilBaseActor, TEXT("EngineSocket"));
	EngineSocketOnOilBase->SetupAttachment(OilBaseActor->GetRootComponent());
	EngineSocketOnOilBase->SetRelativeLocation(FVector(2.314585f, 0.0f, -52.423965f));
	EngineSocketOnOilBase->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
	EngineSocketOnOilBase->SocketRole = ESocketRole::Plug;
	EngineSocketOnOilBase->SnapType = FName(TEXT("Engine"));
	EngineSocketOnOilBase->RegisterComponent();

	USnapSocketComponent* OilBaseStabSocket = NewObject<USnapSocketComponent>(OilBaseActor, TEXT("Stab"));
	OilBaseStabSocket->SetupAttachment(OilBaseActor->GetRootComponent());
	OilBaseStabSocket->SetRelativeLocation(FVector(0.0f, 46.666667f, -25.0f));
	OilBaseStabSocket->SetRelativeRotation(FRotator(0.0f, 90.0f, 0.0f));
	OilBaseStabSocket->SocketRole = ESocketRole::Receiver;
	OilBaseStabSocket->SnapType = FName(TEXT("StabBase"));
	OilBaseStabSocket->CompatibleTypes.Add(FName(TEXT("Stab")));
	OilBaseStabSocket->RegisterComponent();

	USnapSocketComponent* OilBaseNoseSocket = NewObject<USnapSocketComponent>(OilBaseActor, TEXT("NoseSocket"));
	OilBaseNoseSocket->SetupAttachment(OilBaseActor->GetRootComponent());
	OilBaseNoseSocket->SetRelativeLocation(FVector(2.314585f, 0.0f, 52.576035f));
	OilBaseNoseSocket->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));
	OilBaseNoseSocket->SocketRole = ESocketRole::Receiver;
	OilBaseNoseSocket->SnapType = FName(TEXT("Nose"));
	OilBaseNoseSocket->RegisterComponent();

	USnapControllerComponent* OilBaseCtrl = NewObject<USnapControllerComponent>(OilBaseActor, TEXT("OilBaseCtrl"));
	OilBaseCtrl->RegisterComponent();

	AStaticMeshActor* StabActor = PointSnapTestHelpers::SpawnTestPart(World, FVector(0.0f, 20.0f, 190.0f), FVector(0.05f, 0.10f, 0.12f), true);
	USnapSocketComponent* StabSocket = NewObject<USnapSocketComponent>(StabActor, TEXT("SnapSocket"));
	StabSocket->SetupAttachment(StabActor->GetRootComponent());
	StabSocket->SetRelativeLocation(FVector(0.0f, 50.0f, 0.0f));
	StabSocket->SetRelativeRotation(FRotator(0.0f, 90.0f, 0.0f));
	StabSocket->SocketRole = ESocketRole::Plug;
	StabSocket->SnapType = FName(TEXT("Stab"));
	StabSocket->RegisterComponent();

	USnapControllerComponent* StabCtrl = NewObject<USnapControllerComponent>(StabActor, TEXT("StabCtrl"));
	StabCtrl->RegisterComponent();

	AStaticMeshActor* NoseActor = PointSnapTestHelpers::SpawnTestPart(World, FVector(0.0f, 0.0f, 230.0f), FVector(0.15f, 0.15f, 0.15f), true);
	USnapSocketComponent* NoseSocket = NewObject<USnapSocketComponent>(NoseActor, TEXT("SnapSocket"));
	NoseSocket->SetupAttachment(NoseActor->GetRootComponent());
	NoseSocket->SetRelativeLocation(FVector(0.0f, 0.0f, -46.666667f));
	NoseSocket->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
	NoseSocket->SocketRole = ESocketRole::Plug;
	NoseSocket->SnapType = FName(TEXT("Nose"));
	NoseSocket->RegisterComponent();

	USnapControllerComponent* NoseCtrl = NewObject<USnapControllerComponent>(NoseActor, TEXT("NoseCtrl"));
	NoseCtrl->RegisterComponent();

	// Step 1: Dock Stab onto OilBase
	StabActor->SetActorTransform(StabSocket->CalculateDockingActorTransform(OilBaseStabSocket));
	StabCtrl->NotifyGrabbed();
	StabCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Stab found candidate on OilBase"), StabCtrl->bHasCandidate && StabCtrl->bCanSnap);
	StabCtrl->NotifyDropped();
	StabCtrl->TickComponent(0.5f, LEVELTICK_All, nullptr);

	TestEqual(TEXT("Stab is attached to OilBase"), StabActor->GetAttachParentActor(), static_cast<AActor*>(OilBaseActor));
	TestTrue(TEXT("Stab world scale preserved after snapping to OilBase"), StabActor->GetActorScale3D().Equals(FVector(0.05f, 0.10f, 0.12f), 0.005f));

	// Step 2: Dock Nose onto OilBase with Stepped RollMode at 90 deg Yaw
	OilBaseNoseSocket->RollMode = ESocketRollMode::Stepped;
	OilBaseNoseSocket->RollSnapSteps = 4;
	NoseActor->SetActorRotation(FRotator(0.0f, 90.0f, 0.0f));
	NoseActor->SetActorTransform(NoseSocket->CalculateDockingActorTransform(OilBaseNoseSocket));
	NoseCtrl->NotifyGrabbed();
	NoseCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Nose found candidate on OilBase"), NoseCtrl->bHasCandidate && NoseCtrl->bCanSnap);
	NoseCtrl->NotifyDropped();
	NoseCtrl->TickComponent(0.5f, LEVELTICK_All, nullptr);

	TestEqual(TEXT("Nose is attached to OilBase"), NoseActor->GetAttachParentActor(), static_cast<AActor*>(OilBaseActor));
	TestTrue(TEXT("Nose world scale preserved after snapping to OilBase"), NoseActor->GetActorScale3D().Equals(FVector(0.15f, 0.15f, 0.15f), 0.005f));
	const float NoseYawBeforeDock = FMath::UnwindDegrees(NoseActor->GetActorRotation().Yaw);
	TestTrue(FString::Printf(TEXT("Nose docked at 90 deg Stepped Roll (Yaw=%.2f)"), NoseYawBeforeDock), FMath::IsNearlyEqual(FMath::Abs(NoseYawBeforeDock), 90.0f, 1.0f));

	const FVector StabLocAtZ200 = StabActor->GetActorLocation();
	const FVector NoseLocAtZ200 = NoseActor->GetActorLocation();

	// Step 3: Grab the sub-assembly BY THE CHILD PART (NoseActor!), move it so OilBase is near Engine at Z = 35, and drop Nose to snap OilBase onto Engine!
	NoseCtrl->NotifyGrabbed();
	// Move NoseActor down by 165 cm (so OilBase moves from Z=200 down to Z=35)
	NoseActor->SetActorLocation(NoseActor->GetActorLocation() - FVector(0.0f, 0.0f, 165.0f));
	NoseCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);

	TestTrue(TEXT("Holding sub-assembly by child part (Nose) found Engine candidate via OilBase's Plug socket"), NoseCtrl->bHasCandidate && NoseCtrl->bCanSnap);
	TestEqual(TEXT("CandidateHeldSocket is OilBase's EngineSocket"), NoseCtrl->CandidateHeldSocket.Get(), EngineSocketOnOilBase);

	NoseCtrl->NotifyDropped();
	TestTrue(TEXT("Sub-assembly held by Nose started magnetic snap interpolation"), NoseCtrl->bIsInterpolating);

	NoseCtrl->TickComponent(0.1f, LEVELTICK_All, nullptr);
	NoseCtrl->TickComponent(0.5f, LEVELTICK_All, nullptr);

	TestEqual(TEXT("OilBase is attached to Engine"), OilBaseActor->GetAttachParentActor(), static_cast<AActor*>(EngineActor));
	TestTrue(
		FString::Printf(TEXT("OilBase world scale preserved (%s) after attaching to Engine"), *OilBaseActor->GetActorScale3D().ToString()),
		OilBaseActor->GetActorScale3D().Equals(FVector(0.15f, 0.15f, 0.40f), 0.005f)
	);
	TestTrue(
		TEXT("OilBase EngineSocket is flush with Engine SnapSocket"),
		FVector::Dist(EngineSocketOnOilBase->GetComponentLocation(), EngineSocketOnEngine->GetComponentLocation()) < 0.05f
	);

	// Verify Nose preserved its 90 deg Stepped Roll angle during sub-assembly pickup and docking!
	const float NoseYawAfterDock = FMath::UnwindDegrees(NoseActor->GetActorRotation().Yaw);
	TestTrue(
		FString::Printf(TEXT("Nose preserved its 90 deg Stepped Roll after sub-assembly docking (Yaw=%.2f)"), NoseYawAfterDock),
		FMath::IsNearlyEqual(FMath::Abs(NoseYawAfterDock), 90.0f, 1.0f)
	);

	// Verify Stab and Nose moved down with OilBase and did NOT hang in mid-air at Z ~ 200!
	TestEqual(TEXT("Stab remains attached to OilBase after OilBase docks to Engine"), StabActor->GetAttachParentActor(), static_cast<AActor*>(OilBaseActor));
	TestEqual(TEXT("Nose remains attached to OilBase after OilBase docks to Engine"), NoseActor->GetAttachParentActor(), static_cast<AActor*>(OilBaseActor));

	const float StabSocketDist = FVector::Dist(StabSocket->GetComponentLocation(), OilBaseStabSocket->GetComponentLocation());
	const float NoseSocketDist = FVector::Dist(NoseSocket->GetComponentLocation(), OilBaseNoseSocket->GetComponentLocation());

	TestTrue(
		FString::Printf(TEXT("Stab did NOT hang in mid-air (Z=%.2f vs old Z=%.2f, SocketDist=%.4f)"), StabActor->GetActorLocation().Z, StabLocAtZ200.Z, StabSocketDist),
		StabSocketDist < 0.05f && FMath::Abs(StabActor->GetActorLocation().Z - StabLocAtZ200.Z) > 100.0f
	);
	TestTrue(
		FString::Printf(TEXT("Nose did NOT hang in mid-air (Z=%.2f vs old Z=%.2f, SocketDist=%.4f)"), NoseActor->GetActorLocation().Z, NoseLocAtZ200.Z, NoseSocketDist),
		NoseSocketDist < 0.05f && FMath::Abs(NoseActor->GetActorLocation().Z - NoseLocAtZ200.Z) > 100.0f
	);

	UPrimitiveComponent* StabPrim = Cast<UPrimitiveComponent>(StabActor->GetRootComponent());
	UPrimitiveComponent* NosePrim = Cast<UPrimitiveComponent>(NoseActor->GetRootComponent());
	UPrimitiveComponent* OilBasePrim = Cast<UPrimitiveComponent>(OilBaseActor->GetRootComponent());
	TestFalse(TEXT("Stab physics disabled while docked"), StabPrim && StabPrim->IsSimulatingPhysics());
	TestFalse(TEXT("Nose physics disabled while docked"), NosePrim && NosePrim->IsSimulatingPhysics());
	TestFalse(TEXT("OilBase physics disabled while docked"), OilBasePrim && OilBasePrim->IsSimulatingPhysics());
	TestEqual(TEXT("Docked child uses QueryOnly collision so it never launches table physics parts"), StabPrim ? StabPrim->GetCollisionEnabled() : ECollisionEnabled::NoCollision, ECollisionEnabled::QueryOnly);

	TestEqual(TEXT("Engine USnapAssemblyComponent sees all 4 connected parts"), EngineAssembly->GetConnectedPartCount(), 4);

	// Step 4: Verify detaching Stab via DetachFromSocket does NOT detach OilBase from Engine or Nose from OilBase, and re-enables physics + QueryAndPhysics on Stab
	StabCtrl->DetachFromSocket();
	TestNull(TEXT("Stab is no longer attached to OilBase after DetachFromSocket"), StabActor->GetAttachParentActor());
	TestTrue(TEXT("Stab physics re-enabled after detaching into world so it does not float"), StabPrim && StabPrim->IsSimulatingPhysics());
	TestEqual(TEXT("Detached free root restores QueryAndPhysics collision"), StabPrim ? StabPrim->GetCollisionEnabled() : ECollisionEnabled::NoCollision, ECollisionEnabled::QueryAndPhysics);
	TestEqual(TEXT("OilBase remains attached to Engine after Stab detaches"), OilBaseActor->GetAttachParentActor(), static_cast<AActor*>(EngineActor));
	TestEqual(TEXT("Nose remains attached to OilBase after Stab detaches"), NoseActor->GetAttachParentActor(), static_cast<AActor*>(OilBaseActor));

	// Step 5: Verify single-handed detachment when Engine is anchored as AlwaysBase (stationary stand/mount)
	EngineCtrl->SnapRole = ESnapRole::AlwaysBase;
	OilBaseCtrl->NotifyGrabbed();
	TestTrue(TEXT("Grabbing OilBase with one hand while Engine is AlwaysBase enters tension mode without pulling Engine"), OilBaseCtrl->bIsTwoHandedTensionActive);
	TestNull(TEXT("AlwaysBase Engine was NOT attached as a child of OilBase"), EngineActor->GetAttachParentActor());

	// Pull OilBase 25 cm up (beyond DetachTensionDistance = 18 cm) with one hand
	OilBaseActor->SetActorLocation(OilBaseActor->GetActorLocation() + FVector(0.0f, 0.0f, 25.0f));
	OilBaseCtrl->TickComponent(0.016f, LEVELTICK_All, nullptr);
	TestFalse(TEXT("EngineSocket detached from AlwaysBase Engine after single-handed tension pull"), EngineSocketOnOilBase->bIsOccupied);
	TestNull(TEXT("OilBase detached from AlwaysBase Engine"), OilBaseActor->GetAttachParentActor());
	TestEqual(TEXT("Nose remained attached to OilBase after pulling OilBase off AlwaysBase Engine"), NoseActor->GetAttachParentActor(), static_cast<AActor*>(OilBaseActor));
	TestTrue(TEXT("AlwaysBase Engine stayed stationary at origin"), EngineActor->GetActorLocation().IsNearlyZero(0.01f));

	PointSnapTestHelpers::DestroyTestWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPointSnapCompatibilityAndTypingTest,
	"PointSnap.Socket.CompatibilityAndTyping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FPointSnapCompatibilityAndTypingTest::RunTest(const FString& Parameters)
{
	UWorld* World = PointSnapTestHelpers::CreateTestWorld();
	TestNotNull(TEXT("Test world created"), World);
	if (!World)
	{
		return false;
	}

	AActor* ActorA = World->SpawnActor<AStaticMeshActor>();
	AActor* ActorB = World->SpawnActor<AStaticMeshActor>();
	AActor* ActorC = World->SpawnActor<AStaticMeshActor>();

	USnapSocketComponent* SocketA1 = NewObject<USnapSocketComponent>(ActorA, TEXT("SocketA1"));
	SocketA1->SetupAttachment(ActorA->GetRootComponent());
	SocketA1->RegisterComponent();

	USnapSocketComponent* SocketA2 = NewObject<USnapSocketComponent>(ActorA, TEXT("SocketA2"));
	SocketA2->SetupAttachment(ActorA->GetRootComponent());
	SocketA2->RegisterComponent();

	USnapSocketComponent* SocketB = NewObject<USnapSocketComponent>(ActorB, TEXT("SocketB"));
	SocketB->SetupAttachment(ActorB->GetRootComponent());
	SocketB->RegisterComponent();

	USnapSocketComponent* SocketC = NewObject<USnapSocketComponent>(ActorC, TEXT("SocketC"));
	SocketC->SetupAttachment(ActorC->GetRootComponent());
	SocketC->RegisterComponent();

	// 1. Default role is Receiver; two Receivers of same type must NOT be compatible
	SocketA1->SnapType = FName(TEXT("Joint"));
	SocketB->SnapType = FName(TEXT("Joint"));
	TestEqual(TEXT("Default SocketRole is Receiver"), SocketA1->SocketRole, ESocketRole::Receiver);
	TestFalse(TEXT("Receiver + Receiver is rejected"), SocketA1->IsCompatibleWith(SocketB));

	// 2. Plug + Plug must NOT be compatible
	SocketA1->SocketRole = ESocketRole::Plug;
	SocketB->SocketRole = ESocketRole::Plug;
	TestFalse(TEXT("Plug + Plug is rejected"), SocketA1->IsCompatibleWith(SocketB));

	// 3. Plug + Receiver with matching SnapType is compatible
	SocketB->SocketRole = ESocketRole::Receiver;
	TestTrue(TEXT("Plug + Receiver with matching SnapType is compatible"), SocketA1->IsCompatibleWith(SocketB));
	TestTrue(TEXT("Receiver + Plug symmetry holds"), SocketB->IsCompatibleWith(SocketA1));

	// 4. Same-actor sockets are always rejected even if Plug + Receiver and same SnapType
	SocketA2->SnapType = FName(TEXT("Joint"));
	SocketA2->SocketRole = ESocketRole::Receiver;
	TestFalse(TEXT("Sockets on the same actor are rejected"), SocketA1->IsCompatibleWith(SocketA2));

	// 5. Sockets on different actors within the same attached hierarchy are rejected
	ActorC->AttachToActor(ActorA, FAttachmentTransformRules::KeepWorldTransform);
	SocketC->SnapType = FName(TEXT("Joint"));
	SocketC->SocketRole = ESocketRole::Receiver;
	TestFalse(TEXT("Sockets on actors in the same attachment hierarchy are rejected"), SocketA1->IsCompatibleWith(SocketC));
	ActorC->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	TestTrue(TEXT("Sockets become compatible once detached from shared hierarchy"), SocketA1->IsCompatibleWith(SocketC));

	// 6. Per-type role rules (FSnapCompatibleType / CompatibleTypeRules)
	SocketA1->SnapType = FName(TEXT("OilBaseBottom"));
	SocketA1->SocketRole = ESocketRole::Receiver;
	SocketA1->CompatibleTypeRules.Empty();
	SocketA1->CompatibleTypeRules.Add(FSnapCompatibleType(FName(TEXT("Engine")), ESocketRole::Plug));
	SocketA1->CompatibleTypeRules.Add(FSnapCompatibleType(FName(TEXT("OilBaseTop")), ESocketRole::Plug));

	SocketB->SnapType = FName(TEXT("Engine"));
	SocketB->SocketRole = ESocketRole::Receiver;
	TestEqual(TEXT("Effective role of SocketA1 for Engine is Plug"), SocketA1->GetEffectiveRoleFor(SocketB), ESocketRole::Plug);
	TestTrue(TEXT("SocketA1 (per-type Plug) is compatible with Engine (Receiver)"), SocketA1->IsCompatibleWith(SocketB));

	// If Engine socket is changed to Plug, Plug + Plug must be rejected!
	SocketB->SocketRole = ESocketRole::Plug;
	TestFalse(TEXT("SocketA1 (per-type Plug) rejects Engine when Engine is also Plug"), SocketA1->IsCompatibleWith(SocketB));

	PointSnapTestHelpers::DestroyTestWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
