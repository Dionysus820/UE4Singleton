// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include <type_traits>
#include "UE4Singleton.generated.h"

class UWorld;

// 支持C++层的自定义
template<typename T, typename = void>
struct TSingletonsConstructAction;

// 一个简便强大的单例系统
UCLASS(Transient)
class UE4SINGLETON_API UMMPSingletons : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UMMPSingletons();

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
	template<typename T>
	static T* GetSingleton(const UObject* WorldContextObject, bool bCreate = true)
	{
		if (auto Ptr = Cast<T>(GetSingletonImpl(T::StaticClass(), WorldContextObject, false)))
			return Ptr;

		return bCreate ? TryGetSingleton<T>(
							 WorldContextObject,
							 [&] { return TSingletonsConstructAction<T>::CustomConstruct(WorldContextObject); })
					   : nullptr;
	}

	// 尝试获取单例,利用lambda提供自定义初始化能力
	// 当尚未有单例注册时，通过ConstructFunc的自定义创建函数, 创建并注册一个。
	template<typename T, typename F>
	static T* TryGetSingleton(const UObject* WorldContextObject, const F& ConstructFunc)
	{
		static_assert(std::is_convertible<decltype(ConstructFunc()), T*>::value, "err");
		auto Mgr = GetManager(WorldContextObject ? WorldContextObject->GetWorld() : nullptr);
		auto& Ptr = Mgr->Singletons.FindOrAdd(T::StaticClass());
		if (!Ptr)
		{
			Ptr = ConstructFunc();
			if (ensureAlwaysMsgf(Ptr, TEXT("TryGetSingleton Failed %s"), *T::StaticClass()->GetName()))
				RegisterAsSingletonImpl(Ptr, WorldContextObject, true, T::StaticClass());
		}
		return (T*)Ptr;
	}

	// 扩展，特化TSingletonsConstructAction模板定制构造函数参数
	template<typename T, typename... Args>
	static T* TryCreateSingleton(const UObject* WorldContextObject, Args&&... args)
	{
		return TryGetSingleton<T>(WorldContextObject, [&] {
			return TSingletonsConstructAction<T>::CustomConstruct(WorldContextObject, Forward<Args>(args)...);
		});
	}

private:
	UPROPERTY(Transient)
	TMap<UClass*, UObject*> Singletons;

	template<typename T, typename>
	friend struct TSingletonsConstructAction;

	static UMMPSingletons* GetManager(UWorld* InWorld, bool bEnsure);

	static UObject* CreateInstance(UClass* Class, const UObject* WorldContextObject);

	template<typename T>
	static T* CreateInstance(const UObject* WorldContextObject)
	{
		return Cast<T>(CreateInstance(T::StaticClass(), WorldContextObject));
	}

	template<typename T>
	static FString GetTypedNameSafe(const T* Obj)
	{
		return Obj ? Obj->GetName() : T::StaticClass()->GetName();
	}
};

template<typename T, typename V>
struct TSingletonsConstructAction
{
	static T* CustomConstruct(const UObject* WorldContextObject)
	{
		static_assert(std::is_same<V, void>::value, "error");
		return UMMPSingletons::CreateInstance<T>(WorldContextObject);
	}
};
