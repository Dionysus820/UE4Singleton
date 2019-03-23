// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include <type_traits>
#include "SharedPointer.h"
#include "UE4Singleton.generated.h"

class UWorld;

// 支持C++层的自定义
template<typename T, typename = void>
struct TSingletonConstructAction;
// 	static T* CustomConstruct(const UObject* WorldContextObject, UClass* SubClass = nullptr);

DECLARE_DELEGATE_OneParam(FStreamableAsyncObjectDelegate, class UObject*);

// 一个简便强大的单例系统
UCLASS(Transient)
class UE4SINGLETON_API UE4Singleton : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UE4Singleton();

	//////////////////////////////////////////////////////////////////////////
	// BP
	// 一些在level中的Actor可以使用重载PostLoad/PostDuplicate中使用该方式自动注册为单例
	// 注意：由于BP子类的存在关系，这里会将强制祖先类统一注册一遍，直到第一个Native类
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "RegisterAsSingleton", WorldContext = "WorldContextObject",
										 HidePin = "InNativeClass"))
	static UObject* RegisterAsSingletonImpl(UObject* Object, const UObject* WorldContextObject,
											bool bReplaceExist = true, UClass* InNativeClass = nullptr);
	// 获取单例，如果没有注册过则自动创建一个
	UFUNCTION(BlueprintCallable,
			  meta = (DisplayName = "GetSingleton", WorldContext = "WorldContextObject", HidePin = "RegClass"))
	static UObject* GetSingletonImpl(UClass* Class, const UObject* WorldContextObject = nullptr, bool bCreate = true,
									 UClass* RegClass = nullptr);

	//////////////////////////////////////////////////////////////////////////
	// C++
	template<typename T>
	FORCEINLINE static UObject* RegisterAsSingleton(T* Object, const UObject* WorldContextObject,
													bool bReplaceExist = true)
	{
		return RegisterAsSingletonImpl(Object, WorldContextObject, bReplaceExist, T::StaticClass());
	}

	// 简化版
	template<typename T>
	static T* GetSingleton(const UObject* WorldContextObject, bool bCreate = true)
	{
		if (auto Ptr = Cast<T>(GetSingletonImpl(T::StaticClass(), WorldContextObject, false)))
			return Ptr;

		return bCreate
				   ? TryGetSingleton<T>(
						 WorldContextObject,
						 [&] { return TSingletonConstructAction<T>::CustomConstruct(WorldContextObject, nullptr); })
				   : nullptr;
	}

	// 尝试获取单例,利用lambda提供自定义初始化能力
	// 当尚未有单例注册时，通过ConstructFunc的自定义创建函数, 创建并注册一个。
	template<typename T, typename F>
	static T* TryGetSingleton(const UObject* WorldContextObject, const F& ConstructFunc)
	{
		static_assert(std::is_convertible<decltype(ConstructFunc()), T*>::value, "err");
		auto Mgr = GetManager(WorldContextObject ? WorldContextObject->GetWorld() : nullptr, true);
		auto& Ptr = Mgr->Singletons.FindOrAdd(T::StaticClass());
		if (!Ptr)
		{
			Ptr = ConstructFunc();
			if (ensureAlwaysMsgf(Ptr, TEXT("TryGetSingleton Failed %s"), *T::StaticClass()->GetName()))
				RegisterAsSingletonImpl(Ptr, WorldContextObject, true, T::StaticClass());
		}
		return (T*)Ptr;
	}

public:
	// 异步加载
	static TSharedPtr<struct FStreamableHandle> AsyncLoad(const FString& InPath, FStreamableAsyncObjectDelegate Cb,
														  bool bSkipInvalid = false, TAsyncLoadPriority Priority = 0);

	// 异步加载并创建
	static bool AsyncCreate(const UObject* BindedObject, const FString& InPath, FStreamableAsyncObjectDelegate Cb);

	// AOP创建
	template<typename T = UObject>
	static T* CreateInstance(const UObject* WorldContextObject, UClass* SubClass = nullptr)
	{
		return TSingletonConstructAction<T>::CustomConstruct(WorldContextObject, SubClass);
	}

	// 异步加载并AOP创建
	template<typename T, typename F>
	static bool AsyncCreate(const UObject* BindedObject, const FString& InPath, F&& f)
	{
		FWeakObjectPtr WeakObj = BindedObject;
		auto Lambda = [WeakObj, f{MoveTemp(f)}](UObject* ResolvedObj) {
			f(CreateInstance<T>(WeakObj.Get(), Cast<UClass>(ResolvedObj)));
		};

		auto Handle =
			AsyncLoad(InPath, BindedObject ? FStreamableAsyncObjectDelegate::CreateLambda(BindedObject, Lambda);
					  : FStreamableAsyncObjectDelegate::CreateLambda(Lambda));

		return Handle.IsValid();
	}

public:
	template<typename T>
	static FString GetTypedNameSafe(const T* Obj)
	{
		return Obj ? Obj->GetName() : T::StaticClass()->GetName();
	}

private:
	UPROPERTY(Transient)
	TMap<UClass*, UObject*> Singletons;

	template<typename T, typename>
	friend struct TSingletonConstructAction;

	static UE4Singleton* GetManager(UWorld* InWorld, bool bEnsure);
	static UObject* CreateInstanceImpl(const UObject* WorldContextObject, UClass* SubClass);
	template<typename T>
	static T* CreateInstanceImpl(const UObject* WorldContextObject, UClass* SubClass)
	{
		if (!SubClass || ensureAlways(SubClass->IsChildOf<T>()))
			return (T*)UE4Singleton::CreateInstanceImpl(WorldContextObject, SubClass ? SubClass : T::StaticClass());
		return nullptr;
	}
};

template<typename T, typename V>
struct TSingletonConstructAction
{
	static T* CustomConstruct(const UObject* WorldContextObject, UClass* SubClass = nullptr)
	{
		static_assert(std::is_same<V, void>::value, "error");
		return UE4Singleton::CreateInstanceImpl<T>(WorldContextObject, SubClass);
	}
};
