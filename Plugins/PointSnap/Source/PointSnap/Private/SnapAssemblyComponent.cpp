#include "SnapAssemblyComponent.h"
#include "SnapSocketComponent.h"
#include "SnapControllerComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"

USnapAssemblyComponent::USnapAssemblyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USnapAssemblyComponent::BeginPlay()
{
	Super::BeginPlay();

	// Check assembly state on start if parts are pre-assembled in the level
	CheckAssembly();
}

AActor* USnapAssemblyComponent::GetAssemblyRoot() const
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

TArray<AActor*> USnapAssemblyComponent::GetAllConnectedParts() const
{
	TArray<AActor*> Result;
	AActor* RootActor = GetAssemblyRoot();
	if (!RootActor)
	{
		return Result;
	}

	Result.Add(RootActor);

	TArray<AActor*> AttachedActors;
	RootActor->GetAttachedActors(AttachedActors, false, true);

	for (AActor* Act : AttachedActors)
	{
		if (Act)
		{
			// Filter only actors that participate in snapping
			if (Act->FindComponentByClass<USnapSocketComponent>() || Act->FindComponentByClass<USnapControllerComponent>())
			{
				Result.AddUnique(Act);
			}
		}
	}

	// Also traverse occupied snap socket connections via BFS so socket-connected parts are always discovered
	TArray<AActor*> Frontier = Result;
	while (Frontier.Num() > 0)
	{
		AActor* Current = Frontier.Pop();
		if (!Current)
		{
			continue;
		}

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

int32 USnapAssemblyComponent::GetConnectedPartCount() const
{
	return GetAllConnectedParts().Num();
}

TArray<FName> USnapAssemblyComponent::GetMissingTags() const
{
	TArray<FName> Missing;
	const TArray<AActor*> Parts = GetAllConnectedParts();

	TSet<FName> AssembledTags;
	for (const AActor* Act : Parts)
	{
		if (Act)
		{
			for (const FName& Tag : Act->Tags)
			{
				AssembledTags.Add(Tag);
			}
		}
	}

	for (const FName& ReqTag : RequiredPartTags)
	{
		if (!AssembledTags.Contains(ReqTag))
		{
			Missing.Add(ReqTag);
		}
	}

	return Missing;
}

TArray<TSubclassOf<AActor>> USnapAssemblyComponent::GetMissingClasses() const
{
	TArray<TSubclassOf<AActor>> Missing;
	const TArray<AActor*> Parts = GetAllConnectedParts();

	for (TSubclassOf<AActor> ReqClass : RequiredPartClasses)
	{
		if (!ReqClass)
		{
			continue;
		}

		bool bFound = false;
		for (const AActor* Act : Parts)
		{
			if (Act && Act->IsA(ReqClass))
			{
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			Missing.Add(ReqClass);
		}
	}

	return Missing;
}

TArray<USnapSocketComponent*> USnapAssemblyComponent::GetUnoccupiedSockets() const
{
	TArray<USnapSocketComponent*> Unoccupied;
	const TArray<AActor*> Parts = GetAllConnectedParts();

	for (const AActor* Act : Parts)
	{
		if (Act)
		{
			TArray<USnapSocketComponent*> Sockets;
			Act->GetComponents(Sockets);
			for (USnapSocketComponent* Socket : Sockets)
			{
				if (Socket && !Socket->bIsOccupied)
				{
					Unoccupied.Add(Socket);
				}
			}
		}
	}

	return Unoccupied;
}

bool USnapAssemblyComponent::CheckAssembly()
{
	const TArray<AActor*> Parts = GetAllConnectedParts();
	AActor* RootActor = GetAssemblyRoot();

	if (Parts.Num() == 0 || !RootActor)
	{
		if (bIsAssemblyComplete)
		{
			bIsAssemblyComplete = false;
			OnAssemblyBroken.Broadcast(RootActor, nullptr);
		}
		return false;
	}

	if (MinTotalParts > 0 && Parts.Num() < MinTotalParts)
	{
		if (bIsAssemblyComplete)
		{
			bIsAssemblyComplete = false;
			OnAssemblyBroken.Broadcast(RootActor, nullptr);
		}
		return false;
	}

	bool bValid = true;

	const bool bCheckSockets = (ValidationMode == EAssemblyValidationMode::AllSocketsOccupied) ||
	                           (ValidationMode == EAssemblyValidationMode::Combined && bRequireAllSocketsOccupied);

	const bool bCheckTags = (ValidationMode == EAssemblyValidationMode::RequiredPartTags) ||
	                        (ValidationMode == EAssemblyValidationMode::Combined && RequiredPartTags.Num() > 0);

	const bool bCheckClasses = (ValidationMode == EAssemblyValidationMode::RequiredPartClasses) ||
	                          (ValidationMode == EAssemblyValidationMode::Combined && RequiredPartClasses.Num() > 0);

	if (bCheckSockets)
	{
		const TArray<USnapSocketComponent*> OpenSockets = GetUnoccupiedSockets();
		if (OpenSockets.Num() > 0)
		{
			bValid = false;
		}
		else
		{
			// Also ensure there is at least one socket in the assembly
			int32 TotalSockets = 0;
			for (const AActor* Act : Parts)
			{
				TArray<USnapSocketComponent*> Sockets;
				Act->GetComponents(Sockets);
				TotalSockets += Sockets.Num();
			}
			if (TotalSockets == 0)
			{
				bValid = false;
			}
		}
	}

	if (bValid && bCheckTags)
	{
		if (GetMissingTags().Num() > 0)
		{
			bValid = false;
		}
	}

	if (bValid && bCheckClasses)
	{
		if (GetMissingClasses().Num() > 0)
		{
			bValid = false;
		}
	}

	// State transitions
	if (!bIsAssemblyComplete && bValid)
	{
		bIsAssemblyComplete = true;
		OnAssemblyCompleted.Broadcast(RootActor, Parts);
	}
	else if (bIsAssemblyComplete && !bValid)
	{
		bIsAssemblyComplete = false;
		OnAssemblyBroken.Broadcast(RootActor, nullptr);
	}

	return bIsAssemblyComplete;
}

void USnapAssemblyComponent::NotifyAssemblyChanged(AActor* ChangedPart, bool bAttached)
{
	AActor* RootActor = GetAssemblyRoot();

	if (bAttached)
	{
		OnAssemblyPartAdded.Broadcast(RootActor, ChangedPart, GetConnectedPartCount());
	}
	else
	{
		OnAssemblyPartRemoved.Broadcast(RootActor, ChangedPart, GetConnectedPartCount());
		if (bIsAssemblyComplete)
		{
			bIsAssemblyComplete = false;
			OnAssemblyBroken.Broadcast(RootActor, ChangedPart);
		}
	}

	CheckAssembly();
}
