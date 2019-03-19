// Fill out your copyright notice in the Description page of Project Settings.
#include "UE4Singleton.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "UObjectGlobals.h"
#include "IConsoleManager.h"

// WITH_SERVER_CODE
#if !UE_SERVER
#include "UserWidget.h"
#endif

namespace MMPSingletonsManager
{
static TAutoConsoleVariable<int32> SingletonsCreateMethod(TEXT("r.SingletonsCreateMethod"),
														  0,
														  TEXT("0 for Instance, 1 for Transient"),
														  ECVF_Scalability);

template<typename Type>
bool TrueOnFirstCall(const Type&)
{
	static bool bValue = true;
	bool Result = bValue;
	bValue = false;
	return Result;
}
struct FWorldPair
{
	TWeakObjectPtr<UWorld> WeakWorld;
	TWeakObjectPtr<UMMPSingletons> Manager;
};
static TArray<FWorldPair> Managers;
static TWeakObjectPtr<UMMPSingletons>& FindOrAdd(UWorld* InWorld)
{
	for (int32 i = 0; i < Managers.Num(); ++i)
	{
		auto World = Managers[i].WeakWorld;
		if (!World.IsStale(true))
		{
			if (InWorld == World.Get())
				return Managers[i].Manager;
		}
		else
		{
			Managers.RemoveAt(i);
			--i;
		}
	}
	auto& Ref = Managers.AddDefaulted_GetRef();
	if (IsValid(InWorld))
		Ref.WeakWorld = InWorld;
	return Ref.Manager;
}
static void Remove(UWorld* InWorld)
{
	if (!InWorld)
		return;

	for (int32 i = 0; i < Managers.Num(); ++i)
	{
		auto World = Managers[i].WeakWorld;
		if (!World.IsStale(true))
		{
			if (InWorld == World.Get())
			{
				Managers.RemoveAt(i);
				return;
			}
		}
		else
		{
			Managers.RemoveAt(i);
			--i;
		}
	}
}
static UGameInstance* FindInstance()
{
	UGameInstance* Instance = nullptr;
#if WITH_EDITOR
	if (GIsEditor)
	{
		ensureAlwaysMsgf(!GIsInitialLoad && GEngine, TEXT("竟然在GEngine初始化时候就想获取单例？"));
		UWorld* World = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (auto CurWorld = Context.World())
			{
				if (CurWorld->IsGameWorld())
				{
					if (Context.WorldType == EWorldType::PIE /*&& Context.PIEInstance == 0*/)
					{
						World = CurWorld;
						break;
					}

					if (Context.WorldType == EWorldType::Game)
					{
						World = CurWorld;
						break;
					}

					if (CurWorld->GetNetMode() == ENetMode::NM_Standalone ||
						(CurWorld->GetNetMode() == ENetMode::NM_Client && Context.PIEInstance == 2))
					{
						World = CurWorld;
						break;
					}
				}
			}
		}
		Instance = World ? World->GetGameInstance() : nullptr;
	}
	else
#endif
	{
		UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
		if (GameEngine)
			Instance = GameEngine->GetGameInstance();
	}
	UE_LOG(LogTemp, Log, TEXT("UMMPSingletons::FindInstance %s(%p)"),
		   Instance ? *Instance->GetName() : TEXT("Instance"), Instance);
	return Instance;
}

}  // namespace MMPSingletonsManager

UObject* UMMPSingletons::RegisterAsSingletonImpl(UObject* Object, const UObject* WorldContextObject, bool bReplaceExist,
												 UClass* InNativeClass)
{
	check(IsValid(Object));
	if (!ensureAlwaysMsgf(!InNativeClass || InNativeClass->IsNative() || !Object->IsA(InNativeClass),
						  TEXT("Object %s is not child class of %s"), *GetNameSafe(Object),
						  *GetNameSafe(InNativeClass)))
	{
		return nullptr;
	}

	auto World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	// skip cook and CDOs without world
	if (IsRunningCommandlet() || (!World && Object->HasAnyFlags(RF_ClassDefaultObject)))
	{
		UE_LOG(LogTemp, Warning, TEXT("UMMPSingletons::RegisterAsSingleton Error"));
		return nullptr;
	}
	auto ObjectClass = Object->GetClass();
	if (!ensureAlwaysMsgf(ObjectClass->IsChildOf(AActor::StaticClass()) && (Object->GetWorld() != World),
						  TEXT("Actor Singleton must match it's own world")))
		return nullptr;

	auto Mgr = GetManager(World, true);
	UObject* LastPtr = nullptr;
	if (!bReplaceExist)
	{
		LastPtr = Mgr->Singletons.FindOrAdd(ObjectClass);
		if (!LastPtr && InNativeClass)
			LastPtr = Mgr->Singletons.FindOrAdd(InNativeClass);
		if (LastPtr)
		{
			UE_LOG(LogTemp, Log, TEXT("UMMPSingletons::RegisterAsSingleton Exist %s(%p) -> %s -> %s(%p)"),
				   *GetTypedNameSafe(World), World, *ObjectClass->GetName(), *GetTypedNameSafe(LastPtr), LastPtr);
			return LastPtr;
		}
	}

	for (auto CurClass = ObjectClass; CurClass; CurClass = CurClass->GetSuperClass())
	{
		UObject*& Ptr = Mgr->Singletons.FindOrAdd(CurClass);
		LastPtr = Ptr;

		ensureAlways(!bReplaceExist || !Ptr || (!InNativeClass && CurClass->IsNative()) ||
					 InNativeClass /* == CurClass*/);

		{
			Ptr = Object;
#if !SHIPPING_EXTERNAL
			UE_LOG(LogTemp, Log, TEXT("UMMPSingletons::RegisterAsSingleton %s(%p) -> %s -> %s(%p)"),
				   *GetTypedNameSafe(World), World, *CurClass->GetName(), *GetTypedNameSafe(Ptr), Ptr);
#endif
		}

		if ((InNativeClass && CurClass == InNativeClass) || (!InNativeClass && CurClass->IsNative()))
			break;
	}

	return LastPtr;
}

UObject* UMMPSingletons::GetSingletonImpl(UClass* Class, const UObject* WorldContextObject, bool bCreate,
										  UClass* RegClass)
{
	check(Class);
	if (!RegClass)
		RegClass = Class;

	auto World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	auto Mgr = GetManager(World, bCreate);
	UObject*& Ptr = Mgr->Singletons.FindOrAdd(RegClass);
#if 0
	UE_LOG(LogTemp, Log, TEXT("UMMPSingletons::GetSingleton %s(%p) -> %s -> %s(%p)"),
		*GetTypedNameSafe(World), World, *Class->GetName(), *GetTypedNameSafe(Ptr), Ptr);

#endif
	if (!IsValid(Ptr))
	{
		if (!IsValid(Ptr) && bCreate)
		{
			Ptr = CreateInstance(Class, World);
#if !SHIPPING_EXTERNAL
			UE_LOG(LogTemp, Log, TEXT("UMMPSingletons::NewSingleton %s(%p) -> %s -> %s(%p)"), *GetTypedNameSafe(World),
				   World, *Class->GetName(), *GetTypedNameSafe(Ptr), Ptr);

#endif
			if (ensureAlways(IsValid(Ptr)))
			{
				RegisterAsSingletonImpl(Ptr, World, true, RegClass);
			}
			else
			{
				Ptr = nullptr;
			}
		}
	}

	return Ptr;
}

UObject* UMMPSingletons::CreateInstance(UClass* Class, const UObject* WorldContextObject)
{
	UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	UObject* Ptr = nullptr;
	bool bIsActorClass = Class->IsChildOf(AActor::StaticClass());
	if (!World)
	{
		ensureAlways(!bIsActorClass);
		auto Instance = MMPSingletonsManager::FindInstance();
		if (ensure(Instance) && MMPSingletonsManager::SingletonsCreateMethod.GetValueOnGameThread() == 0)
		{
			Ptr = NewObject<UObject>(Instance, Class);
		}
		else
		{
			Ptr = NewObject<UObject>((UObject*)GetTransientPackage(), Class);
		}
	}
	else if (!bIsActorClass)
	{
#if !UE_SERVER
		if (Class->IsChildOf(UUserWidget::StaticClass()))
		{
			Ptr = CreateWidget(World, Class);
		}
		else
#endif
		{
			Ptr = NewObject<UObject>(World, Class);
		}
	}
	else
	{
		Ptr = World->SpawnActor<AActor>(Class);
	}
	ensureAlways(Ptr);
#if !SHIPPING_EXTERNAL
	UE_LOG(LogTemp, Log, TEXT("UMMPSingletons::CreateInstance %s(%p) -> %s -> %s(%p)"), *GetTypedNameSafe(World), World,
		   *Class->GetName(), *GetTypedNameSafe(Ptr), Ptr);
#endif
	return Ptr;
}

UMMPSingletons::UMMPSingletons()
{
	if (MMPSingletonsManager::TrueOnFirstCall([] {}))
	{
		// GEngine->OnWorldAdded().AddLambda();
		// GEngine->OnWorldDestroyed().AddLambda();
		FWorldDelegates::OnWorldCleanup.AddLambda(
			[](UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/) {
				MMPSingletonsManager::Remove(World);
			});

		// 	FWorldDelegates::OnPreWorldInitialization.AddLambda(
		// 		[](UWorld* Wrold, const UWorld::InitializationValues IVS) { UMMPSingletons::GetManager(Wrold); });
	}
}

UMMPSingletons* UMMPSingletons::GetManager(UWorld* World, bool bEnsure)
{
	// FIXME how PIE work? destory duration?
	check(!World || IsValid(World));
	auto& Ptr = MMPSingletonsManager::FindOrAdd(World);
	if (!Ptr.IsValid())
	{
		if (!World)
		{
			auto Instance = MMPSingletonsManager::FindInstance();
			ensureMsgf(!bEnsure || Instance != nullptr, TEXT("MMPSingletonsManager::FindInstance Error"));
			auto Obj = NewObject<UMMPSingletons>();
			Ptr = Obj;
			if (Instance)
			{
				Instance->RegisterReferencedObject(Obj);
			}
			else
			{
				Ptr->AddToRoot();
			}
		}
		else
		{
			auto Obj = NewObject<UMMPSingletons>(World);
			Ptr = Obj;
			World->ExtraReferencedObjects.AddUnique(Obj);
		}
#if !SHIPPING_EXTERNAL
		UE_LOG(LogTemp, Log, TEXT("UMMPSingletons::NewManager %s(%p) -> %s(%p)"), *GetTypedNameSafe(World), World,
			   *GetTypedNameSafe(Ptr.Get()), Ptr.Get());
#endif
		check(Ptr.IsValid());
	}
	return Ptr.Get();
}

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class FMMPSPlugin final : public IModuleInterface
{
public:
	// IModuleInterface implementation
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
	// End of IModuleInterface implementation
};

IMPLEMENT_MODULE(FMMPSPlugin, MMPS)
