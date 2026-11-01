#include "PointSnapFunctionLibrary.h"
#include "SnapControllerComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"

FTransform UPointSnapFunctionLibrary::CalculateDockingTransform(const USnapSocketComponent* HeldSocket, const USnapSocketComponent* TargetSocket)
{
	if (!HeldSocket)
	{
		return FTransform::Identity;
	}

	return HeldSocket->CalculateDockingActorTransform(TargetSocket);
}

bool UPointSnapFunctionLibrary::AreSocketsCompatibleAndFacing(const USnapSocketComponent* SocketA, const USnapSocketComponent* SocketB)
{
	if (!SocketA || !SocketB)
	{
		return false;
	}

	return SocketA->IsCompatibleWith(SocketB) && SocketA->IsFacing(SocketB);
}

TArray<AActor*> UPointSnapFunctionLibrary::GetAllConnectedAssemblyParts(AActor* AnyPartInAssembly)
{
	TArray<AActor*> Result;
	if (!AnyPartInAssembly)
	{
		return Result;
	}

	AActor* Current = AnyPartInAssembly;
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

	if (!RootActor)
	{
		return Result;
	}

	Result.Add(RootActor);

	TArray<AActor*> Attached;
	RootActor->GetAttachedActors(Attached, false, true);

	for (AActor* Act : Attached)
	{
		if (Act && (Act->FindComponentByClass<USnapSocketComponent>() || Act->FindComponentByClass<USnapControllerComponent>()))
		{
			Result.AddUnique(Act);
		}
	}

	return Result;
}

USnapAssemblyComponent* UPointSnapFunctionLibrary::GetAssemblyComponent(AActor* AnyPartInAssembly)
{
	if (!AnyPartInAssembly)
	{
		return nullptr;
	}

	AActor* Current = AnyPartInAssembly;
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

	if (!RootActor)
	{
		return nullptr;
	}

	// First check root actor
	if (USnapAssemblyComponent* RootComp = RootActor->FindComponentByClass<USnapAssemblyComponent>())
	{
		return RootComp;
	}

	// Then check attached actors
	TArray<AActor*> Attached;
	RootActor->GetAttachedActors(Attached, false, true);

	for (AActor* Act : Attached)
	{
		if (Act)
		{
			if (USnapAssemblyComponent* Comp = Act->FindComponentByClass<USnapAssemblyComponent>())
			{
				return Comp;
			}
		}
	}

	return nullptr;
}
