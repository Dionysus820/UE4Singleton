// Fill out your copyright notice in the Description page of Project Settings.

#include "UE4Singleton.h"
#include "UObjectGlobals.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/StreamableManager.h"
#include "Engine/AssetManager.h"
#include "IConsoleManager.h"
#include "Launch/Resources/Version.h"

#if !UE_SERVER
#include "UserWidget.h"
#endif

namespace NamespaceUE4Singleton
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
	TWeakObjectPtr<UE4Singleton> Manager;
};
static TArray<FWorldPair> Managers;
static TWeakObjectPtr<UE4Singleton>& FindOrAdd(UWorld* InWorld)
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
		{
			Instance = GameEngine->GameInstance;
		}
	}
	UE_LOG(LogTemp, Log, TEXT("UE4Singleton::FindInstance %s(%p)"), Instance ? *Instance->GetName() : TEXT("Instance"),
		   Instance);
	return Instance;
}

}  // namespace NamespaceUE4Singleton

UObject* UE4Singleton::RegisterAsSingletonImpl(UObject* Object, const UObject* WorldContextObject, bool bReplaceExist,
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
		UE_LOG(LogTemp, Warning, TEXT("UE4Singleton::RegisterAsSingleton Error"));
		return nullptr;
	}
	auto Mgr = GetManager(World, true);
	auto ObjectClass = Object->GetClass();

	UObject* LastPtr = nullptr;
	if (!bReplaceExist)
	{
		LastPtr = Mgr->Singletons.FindOrAdd(ObjectClass);
		if (!LastPtr && InNativeClass)
			LastPtr = Mgr->Singletons.FindOrAdd(InNativeClass);
		if (LastPtr)
		{
			UE_LOG(LogTemp, Log, TEXT("UE4Singleton::RegisterAsSingleton Exist %s(%p) -> %s -> %s(%p)"),
				   *GetTypedNameSafe(World), World, *ObjectClass->GetName(), *GetTypedNameSafe(LastPtr), LastPtr);
			return LastPtr;
		}
	}

	for (auto CurClass = ObjectClass; CurClass; CurClass = CurClass->GetSuperClass())
	{
		UObject*& Ptr = Mgr->Singletons.FindOrAdd(CurClass);
		LastPtr = Ptr;

		// 替换至InNativeClass类的情况下，确保中间祖先类都没有被注册
		ensureAlways(!bReplaceExist || !Ptr || (!InNativeClass && CurClass->IsNative()) ||
					 InNativeClass /* == CurClass*/);

		{
			Ptr = Object;
#if !UE_BUILD_SHIPPING
			UE_LOG(LogTemp, Log, TEXT("UE4Singleton::RegisterAsSingleton %s(%p) -> %s -> %s(%p)"),
				   *GetTypedNameSafe(World), World, *CurClass->GetName(), *GetTypedNameSafe(Ptr), Ptr);
#endif
		}

		// 直到InNativeClass类， 或者第一个Native类
		if ((InNativeClass && CurClass == InNativeClass) || (!InNativeClass && CurClass->IsNative()))
			break;
	}

	return LastPtr;
}

UObject* UE4Singleton::GetSingletonImpl(UClass* Class, const UObject* WorldContextObject, bool bCreate,
										 UClass* RegClass)
{
	check(Class);
	if (!RegClass)
		RegClass = Class;

	auto World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	auto Mgr = GetManager(World, bCreate);
	UObject*& Ptr = Mgr->Singletons.FindOrAdd(RegClass);
	if (!IsValid(Ptr))
	{
		if (!IsValid(Ptr) && bCreate)
		{
			Ptr = CreateInstanceImpl(World, Class);
#if !UE_BUILD_SHIPPING
			UE_LOG(LogTemp, Log, TEXT("UE4Singleton::NewSingleton %s(%p) -> %s -> %s(%p)"), *GetTypedNameSafe(World),
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

UObject* UE4Singleton::CreateInstanceImpl(const UObject* WorldContextObject, UClass* Class)
{
	check(Class);
	UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	UObject* Ptr = nullptr;
	bool bIsActorClass = Class->IsChildOf(AActor::StaticClass());
	if (!World)
	{
		// World都没有,创建不了Actor类！！
		ensureAlways(!bIsActorClass);
		auto Instance = NamespaceUE4Singleton::FindInstance();
		if (ensure(Instance) && NamespaceUE4Singleton::SingletonsCreateMethod.GetValueOnGameThread() == 0)
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
#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("UE4Singleton::CreateInstanceImpl %s(%p) -> %s -> %s(%p)"), *GetTypedNameSafe(World),
		   World, *Class->GetName(), *GetTypedNameSafe(Ptr), Ptr);
#endif
	return Ptr;
}

UE4Singleton::UE4Singleton()
{
	if (NamespaceUE4Singleton::TrueOnFirstCall([] {}))
	{
		FWorldDelegates::OnWorldCleanup.AddLambda(
			[](UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/) {
				NamespaceUE4Singleton::Remove(World);
			});
	}
}

TSharedPtr<class FStreamableHandle> UE4Singleton::AsyncLoad(const FString& InPath,
															 FStreamableAsyncObjectDelegate DelegateCallback,
															 bool bSkipInvalid, TAsyncLoadPriority Priority)
{
	auto& StreamableMgr = UAssetManager::GetStreamableManager();
	FSoftObjectPath SoftPath = InPath;
	return UAssetManager::GetStreamableManager().RequestAsyncLoad(
		SoftPath, FStreamableDelegate::CreateLambda([bSkipInvalid, Cb{MoveTemp(DelegateCallback)}, SoftPath] {
			auto* Obj = SoftPath.ResolveObject();
			if (!bSkipInvalid || Obj)
				Cb.ExecuteIfBound(Obj);
		}),
		Priority, true
#if UE_BUILD_DEBUG
		,
		false, FString::Printf(TEXT("RequestAsyncLoad [%s]"), *InPath)
#endif
	);
}

bool UE4Singleton::AsyncCreate(const UObject* BindedObject, const FString& InPath, FStreamableAsyncObjectDelegate Cb)
{
	FWeakObjectPtr WeakObj = BindedObject;
	auto Lambda = [WeakObj, Cb{MoveTemp(Cb)}](UObject* ResolvedObj) {
		auto ResolvedClass = Cast<UClass>(ResolvedObj);
		UObject* Obj = CreateInstanceImpl(WeakObj.Get(), ResolvedClass);
		Cb.ExecuteIfBound(Obj);
	};

	auto Handle =
		UE4Singleton::AsyncLoad(InPath,
								 BindedObject ? FStreamableAsyncObjectDelegate::CreateLambda(BindedObject, Lambda)
											  : FStreamableAsyncObjectDelegate::CreateLambda(Lambda));

	return Handle.IsValid();
}

UE4Singleton* UE4Singleton::GetManager(UWorld* World, bool bEnsure)
{
	// FIXME how PIE work? destory duration?
	check(!World || IsValid(World));
	auto& Ptr = NamespaceUE4Singleton::FindOrAdd(World);
	if (!Ptr.IsValid())
	{
		if (!World)
		{
			auto Instance = NamespaceUE4Singleton::FindInstance();
			ensureMsgf(!bEnsure || Instance != nullptr, TEXT("NamespaceUE4Singleton::FindInstance Error"));
			auto Obj = NewObject<UE4Singleton>();
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
			auto Obj = NewObject<UE4Singleton>(World);
			Ptr = Obj;
			World->ExtraReferencedObjects.AddUnique(Obj);
		}
#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Log, TEXT("UE4Singleton::NewManager %s(%p) -> %s(%p)"), *GetTypedNameSafe(World), World,
			   *GetTypedNameSafe(Ptr.Get()), Ptr.Get());
#endif
		check(Ptr.IsValid());
	}
	return Ptr.Get();
}
